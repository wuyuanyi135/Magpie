#include "pch.h"
#include "CursorManager.h"
#include "CommonSharedConstants.h"
#include "Logger.h"
#include "Renderer.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include <dwmapi.h>
#include <fstream>
#include <magnification.h>
#include <mutex>
#include <utility>
#include <vector>

namespace Magpie {

// 系统光标速度是全局的，而每个缩放窗口都有自己的 CursorManager。多窗口并存时若各
// 实例独立保存/恢复速度，后进入的窗口会把已被调慢的速度当成「原始速度」存下来，在
// 多个缩放窗口间切换时光标会越来越慢。因此进程内共享「原始速度」和各实例的调整值，
// 只有所有实例都退出捕获状态才真正还原。
static std::mutex g_cursorSpeedMutex;
static INT g_originCursorSpeed = 0;	// 0 表示当前没有任何实例处于捕获状态
// (实例, 调整后的速度)，按调整时间排序，用于在某个实例退出捕获时恢复其他实例的调整值
static std::vector<std::pair<const void*, INT>> g_adjustedCursorSpeeds;

// 系统光标的显示与隐藏（ShowSystemCursor）同样是全局的。多窗口并存时若各实例独立
// 记忆可见性，一个实例改变全局状态后，仍处于捕获状态的其它实例会因「缓存与期望一致」
// 跳过真正的调用，可见性和实际状态脱钩：典型表现是切换缩放窗口或停止其中一个缩放后，
// 系统光标浮在仍在捕获的全屏窗口上。因此进程内共享「要求隐藏的实例数」和全局实际状态，
// 只有全局状态真正变化时才调用。
static std::mutex g_cursorVisibilityMutex;
static int g_hideSystemCursorCount = 0;	// 要求隐藏系统光标的实例数
static bool g_systemCursorHidden = false;	// 系统光标当前是否实际处于隐藏状态

// 光标限制区域（ClipCursor）同样是桌面级的全局状态，而每个缩放窗口都有自己的
// CursorManager。多窗口并存时，A 实例设置的裁剪区会被 B 实例覆盖，B 又可能把 A
// 缓存里的矩形当成自己的来回写；再加上切换前台窗口时 OS 会自动清空裁剪区，快速
// Alt+Tab 时很容易把光标困在源窗口那一小块里（表现为光标不跟随鼠标，再切一次
// 前台窗口才恢复）。因此进程内用一把锁串行化所有 ClipCursor 读写，并记录当前
// 裁剪区由哪个实例持有。
static std::mutex g_clipCursorMutex;
static const void* g_clipCursorOwner = nullptr;
static HWND g_clipCursorOwnerHwnd = NULL;

static std::string _FormatClipRect(const RECT& rect) noexcept {
	return fmt::format("({},{})-({},{})", rect.left, rect.top, rect.right, rect.bottom);
}

// 进程在光标速度被调慢期间崩溃的话，系统光标速度会永远停留在调慢值。因此首次调整时
// 把原始速度写入标记文件，所有实例退出时删除；下次启动若标记仍在，说明上次异常退出，
// 据此还原（见 RestoreCursorSpeedIfCrashed）。
static std::wstring _CursorSpeedFlagPath() noexcept {
	return std::wstring(CommonSharedConstants::LOGS_DIR) + L"\\cursor_speed.flag";
}

void RestoreCursorSpeedIfCrashed() noexcept {
	std::ifstream flagFile(_CursorSpeedFlagPath());
	if (!flagFile) {
		return;
	}

	INT originSpeed = 0;
	if (flagFile >> originSpeed && originSpeed >= 1 && originSpeed <= 20) {
		if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)originSpeed, 0)) {
			Logger::Get().Info(fmt::format("上次异常退出，已还原系统光标速度为 {}", originSpeed));
		} else {
			Logger::Get().Win32Error("还原系统光标速度失败");
		}
	} else {
		Logger::Get().Warn("光标速度标记文件无效，已忽略");
	}

	flagFile.close();
	DeleteFile(_CursorSpeedFlagPath().c_str());
}

// 将源窗口的光标位置映射到缩放后的光标位置。当光标位于源窗口之外，与源窗口的距离不会缩放。
// 对于光标，第一个像素映射到第一个像素，最后一个像素映射到最后一个像素，因此光标区域的缩放
// 倍率和窗口缩放倍率不同！
static POINT SrcToScaling(POINT pt, bool skipBorder) noexcept {
	const Renderer& renderer = ScalingWindow::Get().Renderer();
	const RECT& srcRect = renderer.SrcRect();
	const RECT& destRect = renderer.DestRect();
	const RECT& rendererRect = ScalingWindow::Get().RendererRect();

	POINT result{};

	if (pt.x >= srcRect.right) {
		result.x = (skipBorder ? rendererRect.right : destRect.right) + pt.x - srcRect.right;
	} else if (pt.x < srcRect.left) {
		result.x = (skipBorder ? rendererRect.left : destRect.left) + pt.x - srcRect.left;
	} else {
		double pos = double(pt.x - srcRect.left) / (srcRect.right - srcRect.left - 1);
		result.x = std::lround(pos * (destRect.right - destRect.left - 1)) + destRect.left;
	}

	if (pt.y >= srcRect.bottom) {
		result.y = (skipBorder ? rendererRect.bottom : destRect.bottom) + pt.y - srcRect.bottom;
	} else if (pt.y < srcRect.top) {
		result.y = (skipBorder ? rendererRect.top : destRect.top) + pt.y - srcRect.top;
	} else {
		double pos = double(pt.y - srcRect.top) / (srcRect.bottom - srcRect.top - 1);
		result.y = std::lround(pos * (destRect.bottom - destRect.top - 1)) + destRect.top;
	}

	return result;
}

enum class RoundMethod {
	Round,
	Floor,
	Ceil
};

static POINT ScalingToSrc(POINT pt, RoundMethod roundType = RoundMethod::Round) noexcept {
	const Renderer& renderer = ScalingWindow::Get().Renderer();
	const RECT& srcRect = renderer.SrcRect();
	const RECT& destRect = renderer.DestRect();

	const SIZE srcSize = Win32Helper::GetSizeOfRect(srcRect);
	const SIZE destSize = Win32Helper::GetSizeOfRect(destRect);

	POINT result = { srcRect.left, srcRect.top };

	if (pt.x >= destRect.right) {
		result.x += srcSize.cx + pt.x - destRect.right;
	} else if (pt.x < destRect.left) {
		result.x += pt.x - destRect.left;
	} else {
		double pos = double(pt.x - destRect.left) / (destSize.cx - 1);
		double delta = pos * (srcSize.cx - 1);

		if (roundType == RoundMethod::Round) {
			result.x += std::lround(delta);
		} else if (roundType == RoundMethod::Floor) {
			result.x += (LONG)std::floor(delta);
		} else {
			result.x += (LONG)std::ceil(delta);
		}
	}

	if (pt.y >= destRect.bottom) {
		result.y += srcSize.cy + pt.y - destRect.bottom;
	} else if (pt.y < destRect.top) {
		result.y += pt.y - destRect.top;
	} else {
		double pos = double(pt.y - destRect.top) / (destSize.cy - 1);
		double delta = pos * (srcSize.cy - 1);

		if (roundType == RoundMethod::Round) {
			result.y += std::lround(delta);
		} else if (roundType == RoundMethod::Floor) {
			result.y += (LONG)std::floor(delta);
		} else {
			result.y += (LONG)std::ceil(delta);
		}
	}

	return result;
}

CursorManager::~CursorManager() noexcept {
	_ShowSystemCursor(true, true);
	_RestoreClipCursor();

	if (_isUnderCapture) {
		POINT cursorPos;
		if (GetCursorPos(&cursorPos)) {
			_StopCapture(cursorPos, true, "CursorManager 析构");
			_ReliableSetCursorPos(cursorPos);
		} else {
			Logger::Get().Win32Error("GetCursorPos 失败");
			_RestoreClipCursor();
		}
	}
}

void CursorManager::Update() noexcept {
	_UpdateCursorState();
	_UpdateCursorPos();
}

void CursorManager::OnScalingPosChanged() noexcept {
	if (_isUnderCapture && !ScalingWindow::Get().SrcTracker().IsMoving()) {
		// 确保光标的缩放后位置不变
		_ReliableSetCursorPos(ScalingToSrc(_cursorPos));
	}

	_lastCompletedHitTestId = _nextHitTestId++;
	_lastCompletedHitTestPos.x = std::numeric_limits<LONG>::max();
	_lastCompletedHitTestResult = HTNOWHERE;
}

void CursorManager::OnSrcStartMove() noexcept {
	if (!_isUnderCapture) {
		return;
	}

	// 以防 _UpdateCursorState 错过时机没有设置 _localCursorPosOnMoving。
	// 源窗口自己实现拖拽逻辑或标题栏上右键然后立刻左键可能遇到这种情况。
	if (_localCursorPosOnMoving.x == std::numeric_limits<LONG>::max()) {
		const RECT& rendererRect = ScalingWindow::Get().RendererRect();
		_localCursorPosOnMoving.x = _cursorPos.x - rendererRect.left;
		_localCursorPosOnMoving.y = _cursorPos.y - rendererRect.top;
	}

	// 源窗口移动时临时还原光标移动速度
	_RestoreCursorSpeed();
}

void CursorManager::OnSrcEndMove() noexcept {
	if (!_isUnderCapture) {
		return;
	}

	_localCursorPosOnMoving.x = std::numeric_limits<LONG>::max();

	_AdjustCursorSpeed();
}

void CursorManager::OnStartMove() noexcept {
	if (_isUnderCapture) {
		return;
	}

	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	_localCursorPosOnMoving.x = _cursorPos.x - rendererRect.left;
	_localCursorPosOnMoving.y = _cursorPos.y - rendererRect.top;
}

void CursorManager::OnEndResizeMove() noexcept {
	_localCursorPosOnMoving.x = std::numeric_limits<LONG>::max();
}

void CursorManager::OnSrcRectChanged() noexcept {
	_ClearHitTestResult();
}

void CursorManager::IsCursorOnOverlay(bool value) noexcept {
	if (_isOnOverlay == value) {
		return;
	}
	_isOnOverlay = value;

	Logger::Get().Info(fmt::format("叠加层光标状态: 光标{}叠加层上", value ? "位于" : "离开"));

	_UpdateCursorState();
	_UpdateCursorPos();
}

void CursorManager::IsCursorCapturedOnOverlay(bool value) noexcept {
	if (_isCapturedOnOverlay == value) {
		return;
	}
	_isCapturedOnOverlay = value;

	Logger::Get().Info(fmt::format("叠加层光标状态: {}捕获光标",
		value ? "开始" : "结束"));

	_UpdateCursorState();
	_UpdateCursorPos();
}

void CursorManager::OnPark() noexcept {
	// 缩放画面收起到后台时缩放窗口将被隐藏，此后 _UpdateCursorState 不再执行，
	// 因此这里必须无条件退出捕获状态并恢复系统光标
	Logger::Get().Info(fmt::format(
		"画面收起到后台，强制复位光标状态（捕获={}，绘制={}，前台捕获={}，叠加层={}/{}）",
		_isUnderCapture, _shouldDrawCursor, _isCapturedOnForeground,
		_isCapturedOnOverlay, _isOnOverlay));

	if (_isUnderCapture) {
		_RestoreCursorSpeed();
		_isUnderCapture = false;
	}

	_isCapturedOnForeground = false;
	_shouldDrawCursor = false;
	_hCursor = NULL;

	_ShowSystemCursor(true);
	_RestoreClipCursor();
}

void CursorManager::_ShowSystemCursor(bool show, bool onDestory) {
	if (ScalingWindow::Get().Options().IsDebugMode()) {
		return;
	}

	const bool wantHide = !show;

	// 更新本实例贡献的期望值并重算全局状态。关键在于只有全局状态真正变化时才继续：
	// 即使本实例的期望未变，其它实例改变全局状态也不会被本实例以过期的缓存覆盖；
	// 而某个实例退出捕获时，只要还有实例要求隐藏，全局状态就维持隐藏。
	bool newHidden = false;
	int hideCount = 0;
	{
		std::lock_guard<std::mutex> lock(g_cursorVisibilityMutex);

		if (_isHidingSystemCursor == wantHide) {
			return;
		}
		_isHidingSystemCursor = wantHide;

		g_hideSystemCursorCount += wantHide ? 1 : -1;
		assert(g_hideSystemCursorCount >= 0);
		hideCount = g_hideSystemCursorCount;

		newHidden = g_hideSystemCursorCount > 0;
		if (newHidden == g_systemCursorHidden) {
			// 全局状态不受影响，如某个实例退出捕获而另一个实例仍处于捕获状态
			return;
		}

		g_systemCursorHidden = newHidden;
	}

	const bool globalShow = !newHidden;

	Logger::Get().Info(fmt::format("系统光标可见性切换: {}（要求隐藏的实例数 {}）",
		newHidden ? "隐藏" : "显示", hideCount));

	static const auto showSystemCursor =
		Win32Helper::LoadFunction<void WINAPI(BOOL)>(L"user32.dll", "ShowSystemCursor");

	if (showSystemCursor) {
		showSystemCursor((BOOL)globalShow);
	} else {
		// 获取 ShowSystemCursor 失败则回落到 Magnification API
		static bool initialized = []() {
			if (!MagInitialize()) {
				Logger::Get().Win32Error("MagInitialize 失败");
				return false;
			}

			return true;
		}();

		if (initialized) {
			MagShowSystemCursor(globalShow);
		} else {
			Logger::Get().Error("无法显示或隐藏系统光标");
		}
	}

	ScalingWindow::Get().Renderer().OnCursorVisibilityChanged(globalShow, onDestory);
}

void CursorManager::_AdjustCursorSpeed() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (!options.IsAdjustCursorSpeed() || options.IsDebugMode()) {
		return;
	}

	INT origin = 0;
	{
		std::lock_guard<std::mutex> lock(g_cursorSpeedMutex);
		if (g_originCursorSpeed == 0) {
			// 第一个进入捕获状态的实例负责读取真实的原始速度
			if (!SystemParametersInfo(SPI_GETMOUSESPEED, 0, &g_originCursorSpeed, 0)) {
				Logger::Get().Win32Error("获取光标移速失败");
				return;
			}

			// 记录原始速度，供异常退出后恢复（见 RestoreCursorSpeedIfCrashed）
			std::ofstream flagFile(_CursorSpeedFlagPath(), std::ios::trunc);
			if (flagFile) {
				flagFile << g_originCursorSpeed;
			} else {
				Logger::Get().Warn("无法写入光标速度标记文件");
			}
		}
		origin = g_originCursorSpeed;
	}
	_originCursorSpeed = origin;

	// 鼠标加速默认打开
	bool isMouseAccelerationOn = true;
	{
		std::array<INT, 3> values{};
		if (SystemParametersInfo(SPI_GETMOUSE, 0, values.data(), 0)) {
			isMouseAccelerationOn = values[2];
		} else {
			Logger::Get().Win32Error("检索鼠标加速失败");
		}
	}

	const Renderer& renderer = ScalingWindow::Get().Renderer();
	const SIZE srcSize = Win32Helper::GetSizeOfRect(renderer.SrcRect());
	const SIZE destSize = Win32Helper::GetSizeOfRect(renderer.DestRect());
	const double scale = ((double)destSize.cx / srcSize.cx + (double)destSize.cy / srcSize.cy) / 2;

	INT newSpeed = 0;

	// “提高指针精确度”（鼠标加速）打开时光标移速的调整为线性，否则为非线性
	// 参见 https://liquipedia.net/counterstrike/Mouse_Settings#Windows_Sensitivity
	if (isMouseAccelerationOn) {
		newSpeed = std::clamp((INT)lround(origin / scale), 1, 20);
	} else {
		static constexpr std::array<double, 20> SENSITIVITIES = {
			0.03125, 0.0625, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875,
			1.0, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 2.75, 3, 3.25, 3.5
		};

		origin = std::clamp(origin, 1, 20);
		double newSensitivity = SENSITIVITIES[static_cast<size_t>(origin) - 1] / scale;

		auto it = std::lower_bound(SENSITIVITIES.begin(), SENSITIVITIES.end(),
			newSensitivity - FLOAT_EPSILON<double>);
		newSpeed = INT(it - SENSITIVITIES.begin()) + 1;

		if (it != SENSITIVITIES.begin() && it != SENSITIVITIES.end()) {
			// 找到两侧最接近的数值
			if (std::abs(*it - newSensitivity) > std::abs(*(it - 1) - newSensitivity)) {
				--newSpeed;
			}
		}
	}

	if (!SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)newSpeed, 0)) {
		Logger::Get().Win32Error("设置光标移速失败");
		return;
	}

	std::lock_guard<std::mutex> lock(g_cursorSpeedMutex);
	// 已有记录则更新并保持顺序，保证 back() 是最近调整的实例
	std::erase_if(g_adjustedCursorSpeeds, [this](const auto& p) { return p.first == this; });
	g_adjustedCursorSpeeds.emplace_back(this, newSpeed);
}

void CursorManager::_RestoreCursorSpeed() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (!options.IsAdjustCursorSpeed() || options.IsDebugMode()) {
		return;
	}

	INT speedToSet = 0;
	{
		std::lock_guard<std::mutex> lock(g_cursorSpeedMutex);
		std::erase_if(g_adjustedCursorSpeeds, [this](const auto& p) { return p.first == this; });
		if (g_adjustedCursorSpeeds.empty()) {
			// 所有缩放窗口都已退出捕获状态，还原真正的原始速度
			speedToSet = g_originCursorSpeed;
			g_originCursorSpeed = 0;
			DeleteFile(_CursorSpeedFlagPath().c_str());
		} else {
			// 还有其他缩放窗口处于捕获状态，恢复为最近调整的实例的调整值
			speedToSet = g_adjustedCursorSpeeds.back().second;
		}
	}
	_originCursorSpeed = 0;

	if (speedToSet != 0) {
		SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)speedToSet, 0);
	}
}

// SetCursorPos 无法可靠移动光标，虽然调用之后立刻查询光标位置没有问题，但经过一段时
// 间后再次查询会发现光标位置又回到了设置之前。这可能是因为 OS 异步处理硬件输入队列，
// SetCursorPos 时队列中仍有旧事件尚未处理。
// 
// 这个函数使用 ClipCursor 将光标限制在目标位置一段时间，等待系统将输入队列处理完毕。
void CursorManager::_ReliableSetCursorPos(POINT pos) noexcept {
	RECT tempClipRect{ pos.x,pos.y,pos.x + 1,pos.y + 1 };
	RECT originClipRect;

	{
		std::lock_guard<std::mutex> lock(g_clipCursorMutex);
		if (!GetClipCursor(&originClipRect)) {
			Logger::Get().Win32Error("GetClipCursor 失败");
			return;
		}

		ClipCursor(&tempClipRect);
	}

	// 等待一段时间，不能太短
	Sleep(8);

	{
		std::lock_guard<std::mutex> lock(g_clipCursorMutex);

		RECT curClip;
		if (!GetClipCursor(&curClip)) {
			Logger::Get().Win32Error("GetClipCursor 失败");
		} else if (curClip == tempClipRect) {
			// 还原原始光标限制区域
			ClipCursor(&originClipRect);
			if (g_clipCursorOwner == this) {
				_lastRealClip = originClipRect;
			}
		} else {
			// 这 8 毫秒里 OS（切换前台窗口会清空裁剪区）或其他缩放窗口改过裁剪区。
			// 此时绝不能把 originClipRect 写回去，否则可能把光标困在源窗口里。
			Logger::Get().Info(fmt::format(
				"临时 1×1 裁剪区已被外部修改（{} -> {}），跳过还原",
				_FormatClipRect(tempClipRect), _FormatClipRect(curClip)
			));

			if (g_clipCursorOwner == this) {
				g_clipCursorOwner = nullptr;
				g_clipCursorOwnerHwnd = NULL;
				_lastClip = { std::numeric_limits<LONG>::max() };
				_lastRealClip = { std::numeric_limits<LONG>::max() };
			}
		}
	}

	// 有的窗口（比如 Magpie 主窗口）捕获光标后光标形状有时不会主动更新，发送 WM_SETCURSOR
	// 强制更新。
	if (_isUnderCapture) {
		const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

		HWND hwndChild;
		int16_t ht = Win32Helper::AdvancedWindowHitTest(hwndSrc, pos, 10, &hwndChild);
		// wParam 传顶层窗口还是子窗口文档没说明，但测试表明必须传入顶层窗口句柄才能起作用
		PostMessage(hwndChild, WM_SETCURSOR,
			(WPARAM)hwndSrc, MAKELPARAM(ht, WM_MOUSEMOVE));
	}
}

winrt::fire_and_forget CursorManager::_SrcHitTestAsync(POINT screenPos) noexcept {
	const uint32_t runId = ScalingWindow::Get().RunId();
	const uint32_t id = _nextHitTestId++;
	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

	// resume_background 之后会回到线程池，那里没有绑定的实例，因此提前捕获
	// 本实例及其 dispatcher 队列
	ScalingWindow* const scalingWindow = &ScalingWindow::Get();
	const winrt::DispatcherQueue dispatcher = scalingWindow->Dispatcher();

	co_await winrt::resume_background();

	const int16_t area = Win32Helper::AdvancedWindowHitTest(hwndSrc, screenPos, 100);

	co_await dispatcher;

	if (runId != ScalingWindow::Get().RunId() || id <= _lastCompletedHitTestId) {
		co_return;
	}

	_lastCompletedHitTestId = id;
	_lastCompletedHitTestPos = screenPos;
	if (_lastCompletedHitTestResult != area) {
		_lastCompletedHitTestResult = area;
		// 命中测试变化则立刻重新计算捕获
		_UpdateCursorState();
	}
}

void CursorManager::_ClearHitTestResult() noexcept {
	_lastCompletedHitTestId = _nextHitTestId++;
	_lastCompletedHitTestPos.x = std::numeric_limits<LONG>::max();
	_lastCompletedHitTestResult = HTNOWHERE;
}

static bool PtInWindow(HWND hWnd, POINT pt) noexcept {
	// 检查窗口是否可见
	if (!IsWindowVisible(hWnd)) {
		return false;
	}

	// 检查是否在窗口内
	RECT windowRect;
	if (!GetWindowRect(hWnd, &windowRect) || !Win32Helper::PtInRect(windowRect, pt)) {
		return false;
	}

	// 检查窗口是否对鼠标透明
	if (GetWindowExStyle(hWnd) & WS_EX_TRANSPARENT) {
		return false;
	}

	// 检查窗口是否被冻结。这个调用比较耗时，因此稍晚检查
	{
		UINT isCloaked = 0;
		HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
		if (SUCCEEDED(hr) && isCloaked) {
			return false;
		}
	}

	// 进一步检查窗口是否对鼠标透明，这比较耗时，因此稍晚检查。除了 WS_EX_TRANSPARENT，还存在两种透明机制:
	// 
	// 1. 分层窗口
	// 2. 使用 SetWindowRgn 自定义形状的窗口
	// 
	// 注意无需考虑 HTTRANSPARENT，它只能作用于子窗口。
	// 
	// 由于前者只能使客户区域透明，ChildWindowFromPointEx 可以完美处理，该接口
	// 也会考虑自定义形状的窗口。反之如果位于非客户区，我们需手动处理后者。
	//
	// 可以参考 ChildWindowFromPointEx 的实现:
	// https://github.com/Blinue/nt5src/blob/daad8a087a4e75422ec96b7911f1df4669989611/Source/XPSP1/NT/windows/core/ntuser/kernel/winwhere.c#L47

	RECT clientRect;
	if (!Win32Helper::GetClientScreenRect(hWnd, clientRect)) {
		// 出错返回 true，因为已经确定光标在窗口内
		return true;
	}

	if (Win32Helper::PtInRect(clientRect, pt)) {
		// 使用 ChildWindowFromPointEx 检查客户区是否透明。
		// 不关心子窗口，因此跳过尽可能多的子窗口以提高性能。
		SetLastError(0);
		if (ChildWindowFromPointEx(
			hWnd,
			{ pt.x - clientRect.left, pt.y - clientRect.top },
			CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT
		)) {
			return true;
		}

		// 如果因权限不足等原因失败则视为不透明
		if (GetLastError() == 0) {
			// 命中了透明像素
			return false;
		}
	}

	// 不在客户区或 ChildWindowFromPointEx 失败则检查窗口区域
	static HRGN hRgn = CreateRectRgn(0, 0, 0, 0);
	const int regionType = GetWindowRgn(hWnd, hRgn);
	if (regionType == SIMPLEREGION || regionType == COMPLEXREGION) {
		if (!PtInRegion(hRgn, pt.x - windowRect.left, pt.y - windowRect.top)) {
			return false;
		}
	}

	return true;
}

// 检测光标位于哪个窗口上，是否检测缩放窗口由 clickThroughHost 指定
static HWND WindowFromPoint(HWND hwndScaling, const RECT& rendererRect, POINT pt, bool clickThroughHost) noexcept {
	struct EnumData {
		HWND result;
		HWND hwndScaling;
		RECT rendererRect;
		POINT pt;
		bool clickThroughHost;
	} data{ NULL, hwndScaling, rendererRect, pt, clickThroughHost };

	EnumWindows([](HWND hWnd, LPARAM lParam) {
		EnumData& data = *(EnumData*)lParam;
		if (hWnd == data.hwndScaling) {
			if (Win32Helper::PtInRect(data.rendererRect, data.pt) && !data.clickThroughHost) {
				data.result = hWnd;
				return FALSE;
			} else {
				return TRUE;
			}
		}

		if (PtInWindow(hWnd, data.pt)) {
			data.result = hWnd;
			return FALSE;
		} else {
			return TRUE;
		}
	}, (LPARAM)&data);

	return data.result;
}

static bool IsEdgeArea(int16_t area) noexcept {
	return area >= HTSIZEFIRST && area <= HTSIZELAST;
}

void CursorManager::_LogStateSnapshot(std::string_view tag) noexcept {
	ScalingWindow& scalingWindow = ScalingWindow::Get();
	const ScalingOptions& options = scalingWindow.Options();
	const Renderer& renderer = scalingWindow.Renderer();
	const HWND hwndSrc = scalingWindow.SrcTracker().Handle();
	const HWND hwndScaling = scalingWindow.Handle();
	const RECT& srcRect = renderer.SrcRect();
	const RECT& destRect = renderer.DestRect();
	const RECT rendererRect = scalingWindow.RendererRect();

	POINT cursorPos{};
	GetCursorPos(&cursorPos);
	const POINT srcCursorPos = ScalingToSrc(cursorPos);

	const HWND hwndFore = GetForegroundWindow();
	const DWORD exStyle = hwndScaling ? (DWORD)GetWindowExStyle(hwndScaling) : 0;

	GUITHREADINFO info{ .cbSize = sizeof(info) };
	GetGUIThreadInfo(NULL, &info);

	// 系统光标的真实可见性。我们缓存了 g_systemCursorHidden，但别的程序或 OS 可能改变
	// 实际状态，只有 GetCursorInfo 能看到真相
	CURSORINFO cursorInfo{ .cbSize = sizeof(cursorInfo) };
	const BOOL gotCursorInfo = GetCursorInfo(&cursorInfo);

	// 遮挡判定的直接依据：光标在缩放坐标和源坐标下分别命中了哪个窗口
	HWND hwndAtScalingCursor = NULL;
	HWND hwndAtSrcCursor = NULL;
	if (hwndScaling) {
		hwndAtScalingCursor = WindowFromPoint(hwndScaling, rendererRect, cursorPos, false);
		hwndAtSrcCursor = WindowFromPoint(hwndScaling, rendererRect, srcCursorPos, true);
	}

	Logger::Get().Info(fmt::format(
		"[光标诊断:{}] 更新#{} 捕获={} 绘制={} 前台捕获={} 叠加层={}/{}(捕获/光标) "
		"hitTest={}(已完成位置=({},{}))"
		" | 光标=({},{}) 源坐标=({},{})"
		" | 源窗口=0x{:X}(前台={} 移动={}) 前台窗口=0x{:X} 前台捕获窗口=0x{:X} 前台GUI标志=0x{:X}"
		" | 缩放窗口=0x{:X}(置顶={} 透明={} 可见={} 最小化={})"
		" | 源矩形=({},{})-({},{}) 目标矩形=({},{})-({},{}) 渲染矩形=({},{})-({},{})"
		" | 光标处窗口(缩放坐标)=0x{:X} 光标处窗口(源坐标)=0x{:X}"
		" | 实际系统光标: 可见={} 句柄=0x{:X} (查询{})"
		" | 3D={} 窗口模式={} 调试={}",
		tag, _updateCount, _isUnderCapture, _shouldDrawCursor, _isCapturedOnForeground,
		_isCapturedOnOverlay, _isOnOverlay, _lastCompletedHitTestResult,
		_lastCompletedHitTestPos.x, _lastCompletedHitTestPos.y,
		cursorPos.x, cursorPos.y, srcCursorPos.x, srcCursorPos.y,
		(uintptr_t)hwndSrc, scalingWindow.SrcTracker().IsFocused(),
		scalingWindow.SrcTracker().IsMoving(),
		(uintptr_t)hwndFore, (uintptr_t)info.hwndCapture, (uintptr_t)info.flags,
		(uintptr_t)hwndScaling, (exStyle & WS_EX_TOPMOST) != 0,
		(exStyle & WS_EX_TRANSPARENT) != 0,
		hwndScaling && IsWindowVisible(hwndScaling) != FALSE,
		hwndScaling && IsIconic(hwndScaling) != FALSE,
		srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
		destRect.left, destRect.top, destRect.right, destRect.bottom,
		rendererRect.left, rendererRect.top, rendererRect.right, rendererRect.bottom,
		(uintptr_t)hwndAtScalingCursor, (uintptr_t)hwndAtSrcCursor,
		gotCursorInfo && (cursorInfo.flags & CURSOR_SHOWING),
		(uintptr_t)cursorInfo.hCursor, gotCursorInfo != FALSE,
		options.Is3DGameMode(), options.IsWindowedMode(), options.IsDebugMode()
	));
}

void CursorManager::_LogCaptureBlocked(std::string_view reason, HWND hwndCur) noexcept {
	// 相同原因限流：原因变化立即记录，否则每 3 秒记录一次
	size_t reasonHash = 1469598103934665603ull;
	for (char c : reason) {
		reasonHash ^= (size_t)(uint8_t)c;
		reasonHash *= 1099511628211ull;
	}

	const auto now = std::chrono::steady_clock::now();
	if (reasonHash == _lastBlockLogReasonHash &&
		now - _lastBlockLogTime < std::chrono::seconds(3)) {
		return;
	}
	_lastBlockLogReasonHash = reasonHash;
	_lastBlockLogTime = now;

	_LogStateSnapshot(fmt::format("未能进入光标捕获: {}{}", reason,
		hwndCur ? fmt::format("（光标处窗口 0x{:X}）", (uintptr_t)hwndCur) : std::string()));
}

void CursorManager::_UpdateCursorState() noexcept {
	++_updateCount;

	// 渲染循环一旦暂停，本方法就不再被调用，光标会冻结在最后一帧的状态上。这里用调用
	// 间隔把「更新中断」本身记录下来，否则日志里只会出现一段无法解释的空白
	const auto now = std::chrono::steady_clock::now();
	if (_lastUpdateTime != std::chrono::steady_clock::time_point{}) {
		const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - _lastUpdateTime).count();
		if (gapMs >= 1000) {
			Logger::Get().Info(fmt::format(
				"光标状态更新中断 {} ms（期间渲染循环未调用 Update），第 {} 次更新恢复",
				gapMs, _updateCount));
		}
	}
	_lastUpdateTime = now;

	if (ScalingWindow::Get().IsResizingOrMoving()) {
		_LogCaptureBlocked("缩放窗口正在调整大小/移动", NULL);
		_RestoreClipCursor();
		return;
	}

	if (ScalingWindow::Get().SrcTracker().IsMoving()) {
		_LogCaptureBlocked("源窗口正在移动", NULL);

		if (_isUnderCapture) {
			// 防止缩放后光标超出屏幕
			_ClipCursorOnSrcMoving();
		} else {
			_RestoreClipCursor();
		}

		return;
	}

	const ScalingOptions& options = ScalingWindow::Get().Options();
	const Renderer& renderer = ScalingWindow::Get().Renderer();
	const RECT& srcRect = renderer.SrcRect();
	const RECT& destRect = renderer.DestRect();

	// 优先级: 
	// 1. 3D 游戏模式: 每帧都限制一次，不退出捕获，不支持多屏幕
	// 2. 常规: 根据多屏幕限制光标，捕获/取消捕获，支持 UI 和多屏幕

	if (options.Is3DGameMode()) {
		if (!_isUnderCapture) {
			POINT cursorPos;
			GetCursorPos(&cursorPos);
			_StartCapture(cursorPos, "3D 游戏模式");
			_ReliableSetCursorPos(cursorPos);

			_shouldDrawCursor = true;
			_ShowSystemCursor(false);

			// 缩放窗口始终透明
			HWND hwndScaling = ScalingWindow::Get().Handle();
			SetWindowLong(hwndScaling, GWL_EXSTYLE,
				GetWindowExStyle(hwndScaling) | WS_EX_TRANSPARENT);
		}

		// 开启“在 3D 游戏中限制光标”则每帧都限制一次光标
		_SetClipCursor(srcRect, true);
		return;
	}

	if (_isCapturedOnOverlay) {
		_LogCaptureBlocked("叠加层正在捕获光标（拖动工具栏等），本帧跳过光标状态更新", NULL);

		// 光标被叠加层捕获时将光标限制在输出区域内
		_SetClipCursor(destRect);
		return;
	}

	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

	{
		// 如果前台窗口捕获了光标，应避免在光标移入/移出缩放窗口或叠加层时跳跃。为了解决
		// 前一个问题，此时则将光标限制在前台窗口内，因此不会移出缩放窗口。为了解决后一个
		// 问题，叠加层将不会试图捕获光标。
		_isCapturedOnForeground = false;

		GUITHREADINFO info{ .cbSize = sizeof(info) };
		GetGUIThreadInfo(NULL, &info);

		if (info.hwndCapture &&
			!(info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)))
		{
			_isCapturedOnForeground = true;

			// 记录鼠标输入的归属。未处于捕获状态时缩放窗口必须是透明的，否则它会吞掉
			// 本该送到捕获窗口的鼠标消息，表现为「游戏收不到鼠标」，同时光标被 Magpie
			// 绘制并冻结在最后一帧的位置——正是「光标不跟随、切出切回才恢复」的症状
			if (!_loggedCapturedOnForeground) {
				_loggedCapturedOnForeground = true;
				Logger::Get().Info(fmt::format(
					"前台窗口开始捕获光标: hwndCapture=0x{:X}（源窗口=0x{:X}，缩放窗口=0x{:X}），GUI 标志=0x{:X}",
					(uintptr_t)info.hwndCapture, (uintptr_t)hwndSrc,
					(uintptr_t)ScalingWindow::Get().Handle(), (uintptr_t)info.flags));
			} else if (!_isUnderCapture) {
				_LogCaptureBlocked("前台窗口捕获着光标，本窗口未处于捕获状态，本帧跳过光标状态机", NULL);
			}

			// 拖拽源窗口时应确保光标位置稳定，为此我们需要确定初始光标位置。但为什么在这
			// 里而不是 OnSrcStartMove 中？虽然很难注意到，但光标在标题栏上轻微移动时不会
			// 触发拖拽，而是移动距离足够或者一段时间后才会触发，这意味着 OnSrcStartMove
			// 中光标可能已经不在初始位置。幸运的是，左键按下后前台窗口会立刻捕获光标，这是
			// 确定初始光标位置的最好时机。
			if (info.hwndCapture == hwndSrc &&
				_localCursorPosOnMoving.x == std::numeric_limits<LONG>::max())
			{
				const RECT& rendererRect = ScalingWindow::Get().RendererRect();
				_localCursorPosOnMoving.x = _cursorPos.x - rendererRect.left;
				_localCursorPosOnMoving.y = _cursorPos.y - rendererRect.top;
			}

			// 如果光标不在缩放窗口内或通过标题栏拖动窗口时不应限制光标
			if (_isUnderCapture && !(info.flags & GUI_INMOVESIZE)) {
				_SetClipCursor(srcRect);
			} else {
				_RestoreClipCursor();
			}

			// 当光标被前台窗口捕获时我们除了限制光标外什么也不做，即光标
			// 可以在缩放窗口上自由移动。
			return;
		}

		if (_loggedCapturedOnForeground) {
			_loggedCapturedOnForeground = false;
			Logger::Get().Info("前台窗口已释放光标");
		}

		// 处理只是点击了标题栏而没有拖动的情况
		if (info.hwndCapture != hwndSrc) {
			_localCursorPosOnMoving.x = std::numeric_limits<LONG>::max();
		}
	}

	const HWND hwndScaling = ScalingWindow::Get().Handle();
	// 复制是特意的，用于检查缩放窗口的位置和尺寸更改
	const RECT rendererRect = ScalingWindow::Get().RendererRect();
	const bool isSrcFocused = ScalingWindow::Get().SrcTracker().IsFocused();
	
	// 缩放窗口的点击穿透状态决定鼠标消息能否到达源窗口。未处于捕获状态却又不穿透时，
	// 缩放窗口会吞掉本该送到源窗口的输入，因此每次切换都记录下来
	auto setClickThrough = [hwndScaling](bool value) {
		const DWORD cur = (DWORD)GetWindowExStyle(hwndScaling);
		if (((cur & WS_EX_TRANSPARENT) != 0) == value) {
			return;
		}

		SetWindowLong(hwndScaling, GWL_EXSTYLE,
			value ? (cur | WS_EX_TRANSPARENT) : (cur & ~WS_EX_TRANSPARENT));
		Logger::Get().Info(fmt::format("缩放窗口点击穿透: {}",
			value ? "开启（鼠标消息送往源窗口）" : "关闭（缩放窗口自己接收鼠标）"));
	};

	POINT cursorPos;
	if (!GetCursorPos(&cursorPos)) {
		_LogCaptureBlocked("GetCursorPos 失败", NULL);
		_RestoreClipCursor();
		return;
	}

	bool shouldClearHitTestResult = true;

	// 光标在缩放窗口上却没能进入捕获的原因，用于诊断日志（见函数末尾）
	std::string_view blockReason;
	HWND blockHwnd = NULL;

	const POINT originCursorPos = cursorPos;

	if (_isUnderCapture) {
		///////////////////////////////////////////////////////////
		// 
		// 处于捕获状态
		// --------------------------------------------------------------
		// 					|   缩放位置被遮挡	|     缩放位置未被遮挡
		// --------------------------------------------------------------
		// 实际位置被遮挡	|      退出捕获		| 退出捕获，缩放窗口不透明
		// --------------------------------------------------------------
		// 实际位置未被遮挡	|      退出捕获		|         无操作
		// --------------------------------------------------------------
		// 
		///////////////////////////////////////////////////////////

		HWND hwndCur = WindowFromPoint(hwndScaling, rendererRect, SrcToScaling(cursorPos, isSrcFocused), false);
		_shouldDrawCursor = hwndCur == hwndScaling;

		if (_shouldDrawCursor) {
			// 缩放窗口未被遮挡
			bool stopCapture = _isOnOverlay;

			if (!stopCapture) {
				// 检查源窗口是否被遮挡
				hwndCur = WindowFromPoint(hwndScaling, rendererRect, cursorPos, true);

				stopCapture = hwndCur != hwndSrc &&
					(!IsChild(hwndSrc, hwndCur) || !(GetWindowStyle(hwndCur) & WS_CHILD));

				if (!stopCapture) {
					shouldClearHitTestResult = false;

					if (_lastCompletedHitTestPos != cursorPos) {
						_SrcHitTestAsync(cursorPos);
					}

					stopCapture = IsEdgeArea(_lastCompletedHitTestResult);
					// 窗口模式缩放时可调整大小的区域经常位于缩放窗口边缘，因此使用系统光标
					if (stopCapture) {
						_shouldDrawCursor = !options.IsWindowedMode();
					}
				}
			}

			if (stopCapture) {
				setClickThrough(false);

				// 源窗口被遮挡或者光标位于叠加层上，这时虽然停止捕获光标，但依然将光标隐藏
				std::string_view stopReason;
				if (_isOnOverlay) {
					stopReason = "光标位于叠加层上";
				} else if (IsEdgeArea(_lastCompletedHitTestResult)) {
					stopReason = "命中测试结果为缩放边框";
				} else {
					stopReason = "源窗口在光标处被遮挡";
				}

				_StopCapture(cursorPos, false, stopReason);
			} else {
				// 未处于叠加层时缩放窗口应穿透，否则会挡住源窗口的鼠标输入
				setClickThrough(!_isOnOverlay);
			}
		} else {
			// 缩放窗口被遮挡
			setClickThrough(false);

			if (!_StopCapture(cursorPos, false, "缩放窗口被遮挡")) {
				_shouldDrawCursor = true;
				_LogCaptureBlocked("缩放窗口被遮挡且无法退出捕获（目标位置不在任何屏幕上）", NULL);
			}
		}
	} else {
		/////////////////////////////////////////////////////////
		// 
		// 未处于捕获状态
		// -------------------------------------------------------------
		//					|   缩放位置被遮挡	|   缩放位置未被遮挡
		// -------------------------------------------------------------
		// 实际位置被遮挡	|      无操作		|    缩放窗口不透明
		// -------------------------------------------------------------
		// 实际位置未被遮挡	|      无操作		| 开始捕获，缩放窗口透明
		// -------------------------------------------------------------
		// 
		/////////////////////////////////////////////////////////

		HWND hwndCur = WindowFromPoint(hwndScaling, rendererRect, cursorPos, false);
		_shouldDrawCursor = hwndCur == hwndScaling;

		if (_shouldDrawCursor) {
			// 缩放窗口未被遮挡
			POINT newCursorPos = ScalingToSrc(cursorPos);

			if (Win32Helper::PtInRect(srcRect, newCursorPos)) {
				bool startCapture = !_isOnOverlay;
				if (!startCapture) {
					blockReason = "光标位于叠加层上";
				}

				if (startCapture) {
					// 检查源窗口是否被遮挡
					hwndCur = WindowFromPoint(hwndScaling, rendererRect, newCursorPos, true);

					startCapture = hwndCur == hwndSrc ||
						(IsChild(hwndSrc, hwndCur) && (GetWindowStyle(hwndCur) & WS_CHILD));

					if (!startCapture) {
						blockReason = "源窗口在光标处被遮挡";
						blockHwnd = hwndCur;
					}

					if (startCapture) {
						shouldClearHitTestResult = false;

						if (_lastCompletedHitTestPos != newCursorPos) {
							_SrcHitTestAsync(newCursorPos);
						}

						startCapture = !IsEdgeArea(_lastCompletedHitTestResult);
						// 窗口模式缩放时可调整大小的区域经常位于缩放窗口边缘，因此使用系统光标
						if (!startCapture) {
							_shouldDrawCursor = !options.IsWindowedMode();
							if (_shouldDrawCursor) {
								blockReason = "命中测试结果为缩放边框";
							}
						}
					}
				}

				if (startCapture) {
					setClickThrough(true);

					_StartCapture(cursorPos, "光标位于画面内且源窗口未被遮挡");
				} else {
					setClickThrough(false);
				}
			} else if (isSrcFocused) {
				// 跳过黑边
				if (_isOnOverlay) {
					// 从内部移到外部，此时有 UI 贴边
					if (newCursorPos.x >= srcRect.right) {
						cursorPos.x += rendererRect.right - destRect.right;
					} else if (newCursorPos.x < srcRect.left) {
						cursorPos.x -= destRect.left - rendererRect.left;
					}

					if (newCursorPos.y >= srcRect.bottom) {
						cursorPos.y += rendererRect.bottom - destRect.bottom;
					} else if (newCursorPos.y < srcRect.top) {
						cursorPos.y -= destRect.top - rendererRect.top;
					}

					if (!MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONULL)) {
						// 目标位置不存在屏幕，则将光标限制在输出区域内
						cursorPos.x = std::clamp(cursorPos.x, destRect.left, destRect.right - 1);
						cursorPos.y = std::clamp(cursorPos.y, destRect.top, destRect.bottom - 1);
					}
				} else {
					// 从外部移到内部
					const POINT clampedPos{
						std::clamp(cursorPos.x, destRect.left, destRect.right - 1),
						std::clamp(cursorPos.y, destRect.top, destRect.bottom - 1)
					};

					if (WindowFromPoint(hwndScaling, rendererRect, clampedPos, false) == hwndScaling) {
						setClickThrough(true);

						_StartCapture(cursorPos, "光标从黑边外移入画面");
					} else {
						// 要跳跃的位置被遮挡
						setClickThrough(false);

						blockReason = "黑边外侧的目标位置被遮挡（源窗口在前台）";
					}
				}
			} else {
				// 源窗口不在前台则允许光标进入黑边
				if (!_isOnOverlay) {
					if (Win32Helper::PtInRect(destRect, cursorPos)) {
						setClickThrough(true);

						_StartCapture(cursorPos, "源窗口不在前台，光标进入输出区域");
					} else {
						setClickThrough(false);

						blockReason = "光标在黑边内，且源窗口不在前台";
					}
				} else {
					blockReason = "光标在黑边内，且位于叠加层上";
				}
			}
		}
	}

	if (shouldClearHitTestResult) {
		_ClearHitTestResult();
	}

	// 光标位于缩放窗口上却没有进入捕获：此时系统光标被隐藏、由 Magpie 绘制光标，但源窗口
	// 收不到鼠标输入，表现为「光标不跟随 / 游戏光标没反应」。记录一次状态快照便于定位。
	if (!blockReason.empty() && !_isUnderCapture) {
		_LogCaptureBlocked(blockReason, blockHwnd);
	}

	// 状态变更与心跳：状态机静默时也能从日志还原当时的完整状态，并据此判断渲染循环是否
	// 仍在运行（心跳间隔异常说明 Update 没有被调用）
	{
		const auto heartbeatNow = std::chrono::steady_clock::now();
		const DWORD curStyle = (DWORD)GetWindowExStyle(hwndScaling);
		const bool transparent = (curStyle & WS_EX_TRANSPARENT) != 0;
		const bool topmost = (curStyle & WS_EX_TOPMOST) != 0;

		bool stateChanged = !_hasLoggedState;
		if (!stateChanged) {
			stateChanged = _isUnderCapture != _loggedUnderCapture ||
				_shouldDrawCursor != _loggedShouldDrawCursor ||
				_isCapturedOnForeground != _loggedCapturedOnForeground ||
				_isOnOverlay != _loggedOnOverlay ||
				_isCapturedOnOverlay != _loggedCapturedOnOverlay ||
				transparent != _loggedTransparent ||
				topmost != _loggedTopmost ||
				_lastCompletedHitTestResult != _loggedHitTest;
		}

		if (stateChanged) {
			_hasLoggedState = true;
			_loggedUnderCapture = _isUnderCapture;
			_loggedShouldDrawCursor = _shouldDrawCursor;
			_loggedCapturedOnForeground = _isCapturedOnForeground;
			_loggedOnOverlay = _isOnOverlay;
			_loggedCapturedOnOverlay = _isCapturedOnOverlay;
			_loggedTransparent = transparent;
			_loggedTopmost = topmost;
			_loggedHitTest = _lastCompletedHitTestResult;
			_lastHeartbeatTime = heartbeatNow;

			_LogStateSnapshot("状态变更");
		} else if (heartbeatNow - _lastHeartbeatTime >= std::chrono::seconds(5)) {
			_lastHeartbeatTime = heartbeatNow;

			// 系统光标的真实可见性可能被其他程序或 OS 改变，而我们只缓存了期望值
			CURSORINFO ci{ .cbSize = sizeof(ci) };
			if (GetCursorInfo(&ci)) {
				const bool visible = (ci.flags & CURSOR_SHOWING) != 0;
				if (visible != _loggedActualCursorVisible) {
					_loggedActualCursorVisible = visible;
					Logger::Get().Info(fmt::format(
						"实际系统光标可见性变化: {}（Magpie 期望隐藏={}）",
						visible ? "显示" : "隐藏", _isHidingSystemCursor));
				}
			}

			_LogStateSnapshot(_shouldDrawCursor && !_isUnderCapture
				? "心跳（Magpie 绘制光标但未捕获，请检查点击穿透与遮挡）"
				: "心跳");
		}
	}

	// 只要光标缩放后的位置在缩放窗口上，且该位置未被其他窗口遮挡，便可以隐藏光标。
	// 即使当前并未捕获光标也是如此。
	_ShowSystemCursor(!_shouldDrawCursor);

	_ClipCursorForMonitors(cursorPos);

	// SetCursorPos 应在 ClipCursor 之后，否则会受到上一次 ClipCursor 的影响
	if (cursorPos != originCursorPos) {
		_ReliableSetCursorPos(cursorPos);
	}

	// 光标位置可能改变，需要更新
	_UpdateCursorPos();
}

static BOOL CALLBACK EnumMonitorProc(HMONITOR, HDC, LPRECT monitorRect, LPARAM data) {
	((SmallVectorImpl<RECT>*)data)->push_back(*monitorRect);
	return TRUE;
}

static SmallVector<RECT, 0> ObtainMonitorRects() noexcept {
	SmallVector<RECT, 0> monitorRects;
	if (!EnumDisplayMonitors(NULL, NULL, EnumMonitorProc, (LPARAM)&monitorRects)) {
		Logger::Get().Win32Error("EnumDisplayMonitors 失败");
	}
	return monitorRects;
}

static bool AnyIntersectedMonitor(const SmallVectorImpl<RECT>& monitorRects, const RECT& testRect) noexcept {
	for (const RECT& monitorRect : monitorRects) {
		if (Win32Helper::IsRectOverlap(monitorRect, testRect)) {
			return true;
		}
	}
	return false;
}

void CursorManager::_ClipCursorForMonitors(POINT cursorPos) noexcept {
	if (!_shouldDrawCursor) {
		_RestoreClipCursor();
	}

	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	const Renderer& renderer = ScalingWindow::Get().Renderer();
	const RECT& srcRect = renderer.SrcRect();
	const RECT& destRect = renderer.DestRect();

	const bool isSrcFocused = ScalingWindow::Get().SrcTracker().IsFocused();

	// 根据当前光标位置的四个方向有无屏幕来确定应该在哪些方向限制光标，但这无法
	// 处理屏幕之间存在间隙的情况。解决办法是 _StopCapture 只在目标位置存在屏幕时才取消捕获，
	// 当光标试图移动到间隙中时将被挡住。如果光标的速度足以跨越间隙，则它依然可以在屏幕间移动。
	const POINT scaledPos = _isUnderCapture ? SrcToScaling(cursorPos, true) : cursorPos;

	RECT clips{ LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX };

	const SmallVector<RECT, 0> monitorRects = ObtainMonitorRects();
	if (!monitorRects.empty()) {
		RECT rect{ LONG_MIN, scaledPos.y, rendererRect.left, scaledPos.y + 1 };

		// left
		if (!AnyIntersectedMonitor(monitorRects, rect)) {
			if (_isUnderCapture) {
				// 已确定缩放窗口左侧无屏幕，计算屏幕左边缘
				LONG minLeft = LONG_MAX;
				for (const RECT& monitorRect : monitorRects) {
					if (monitorRect.top <= scaledPos.y && monitorRect.bottom > scaledPos.y) {
						minLeft = std::min(minLeft, monitorRect.left);
					}
				}

				if (minLeft < destRect.right) {
					// 存在黑边且源窗口位于前台时，应阻止光标进入黑边
					if ((minLeft < destRect.left && isSrcFocused) || minLeft == destRect.left) {
						clips.left = srcRect.left;
					} else {
						// 将缩放后光标位置限制在屏幕内
						clips.left = ScalingToSrc({ minLeft,scaledPos.y }, RoundMethod::Ceil).x;
					}
				}
			} else if (isSrcFocused && destRect.left != rendererRect.left) {
				// 源窗口在前台时阻止光标进入黑边
				clips.left = destRect.left;
			}
		}

		// top
		rect = { scaledPos.x, LONG_MIN, scaledPos.x + 1, rendererRect.top };
		if (!AnyIntersectedMonitor(monitorRects, rect)) {
			if (_isUnderCapture) {
				LONG minTop = LONG_MAX;
				for (const RECT& monitorRect : monitorRects) {
					if (monitorRect.left <= scaledPos.x && monitorRect.right > scaledPos.x) {
						minTop = std::min(minTop, monitorRect.top);
					}
				}

				if (minTop < destRect.bottom) {
					if ((minTop < destRect.top && isSrcFocused) || minTop == destRect.top) {
						clips.top = srcRect.top;
					} else {
						clips.top = ScalingToSrc({ scaledPos.x,minTop }, RoundMethod::Ceil).y;
					}
				}
			} else if (isSrcFocused && destRect.top != rendererRect.top) {
				clips.top = destRect.top;
			}
		}

		// right
		rect = { rendererRect.right, scaledPos.y, LONG_MAX, scaledPos.y + 1 };
		if (!AnyIntersectedMonitor(monitorRects, rect)) {
			if (isSrcFocused) {
				clips.right = _isUnderCapture ? srcRect.right : destRect.right;
			} else if (_isUnderCapture && destRect.right == rendererRect.right) {
				clips.right = srcRect.right;
			}

			if (_isUnderCapture) {
				LONG maxRight = LONG_MIN;
				for (const RECT& monitorRect : monitorRects) {
					if (monitorRect.top <= scaledPos.y && monitorRect.bottom > scaledPos.y) {
						maxRight = std::max(maxRight, monitorRect.right);
					}
				}

				if (maxRight > destRect.left) {
					if ((maxRight > destRect.right && isSrcFocused) || maxRight == destRect.right) {
						clips.right = srcRect.right;
					} else {
						clips.right = ScalingToSrc({ maxRight,scaledPos.y }, RoundMethod::Floor).x;
					}
				}
			} else if (isSrcFocused && destRect.right != rendererRect.right) {
				clips.right = destRect.right;
			}
		}

		// bottom
		rect = { scaledPos.x, rendererRect.bottom, scaledPos.x + 1, LONG_MAX };
		if (!AnyIntersectedMonitor(monitorRects, rect)) {
			if (_isUnderCapture) {
				LONG maxBottom = LONG_MIN;
				for (const RECT& monitorRect : monitorRects) {
					if (monitorRect.left <= scaledPos.x && monitorRect.right > scaledPos.x) {
						maxBottom = std::max(maxBottom, monitorRect.bottom);
					}
				}

				if (maxBottom > destRect.top) {
					if ((maxBottom > destRect.bottom && isSrcFocused) || maxBottom == destRect.bottom) {
						clips.bottom = srcRect.bottom;
					} else {
						clips.bottom = ScalingToSrc({ scaledPos.x,maxBottom }, RoundMethod::Floor).y;
					}
				}
			} else if (isSrcFocused && destRect.bottom != rendererRect.bottom) {
				clips.bottom = destRect.bottom;
			}
		}
	}

	if (clips == RECT{ LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX }) {
		_RestoreClipCursor();
	} else {
		_SetClipCursor(clips);
	}
}

void CursorManager::_ClipCursorOnSrcMoving() noexcept {
	assert(ScalingWindow::Get().SrcTracker().IsMoving() && _isUnderCapture);
	assert(_localCursorPosOnMoving.x != std::numeric_limits<LONG>::max());

	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	const POINT scaledPos = {
		_localCursorPosOnMoving.x + rendererRect.left,
		_localCursorPosOnMoving.y + rendererRect.top
	};
	const POINT originPos = ScalingToSrc(scaledPos);

	RECT clips{ LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX };

	SmallVector<RECT, 0> monitorRects = ObtainMonitorRects();
	if (!monitorRects.empty()) {
		// 移动源窗口时，如果只有一个显示器，应将光标限制在工作矩形内。一旦超出工作矩形，
		// 源窗口将无法继续移动。还需检查窗口样式，以和 OS 保持一致，见
		// https://github.com/Blinue/nt5src/blob/daad8a087a4e75422ec96b7911f1df4669989611/Source/XPSP1/NT/windows/core/ntuser/kernel/movesize.c#L1142
		if (monitorRects.size() == 1) {
			const DWORD exStyle = GetWindowExStyle(ScalingWindow::Get().SrcTracker().Handle());
			if ((exStyle & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) == 0) {
				// 获取主显示器句柄，来自 https://devblogs.microsoft.com/oldnewthing/20141106-00/?p=43683
				const HMONITOR hMon = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
				MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
				if (GetMonitorInfo(hMon, &mi)) {
					monitorRects[0] = mi.rcWork;
					clips = mi.rcWork;
				}
			}
		}

		// left
		if (scaledPos.x < originPos.x) {
			LONG minLeft = LONG_MAX;
			for (const RECT& monitorRect : monitorRects) {
				if (monitorRect.top <= scaledPos.y && monitorRect.bottom > scaledPos.y) {
					minLeft = std::min(minLeft, monitorRect.left);
				}
			}
			if (minLeft != LONG_MAX) {
				// 将缩放后位置限制在屏幕内
				clips.left = minLeft + (originPos.x - scaledPos.x);
			}
		}

		// top
		if (scaledPos.y < originPos.y) {
			LONG minTop = LONG_MAX;
			for (const RECT& monitorRect : monitorRects) {
				if (monitorRect.left <= scaledPos.x && monitorRect.right > scaledPos.x) {
					minTop = std::min(minTop, monitorRect.top);
				}
			}
			if (minTop != LONG_MAX) {
				clips.top = minTop + (originPos.y - scaledPos.y);
			}
		}

		// right
		if (scaledPos.x > originPos.x) {
			LONG maxRight = LONG_MIN;
			for (const RECT& monitorRect : monitorRects) {
				if (monitorRect.top <= scaledPos.y && monitorRect.bottom > scaledPos.y) {
					maxRight = std::max(maxRight, monitorRect.right);
				}
			}
			if (maxRight != LONG_MIN) {
				clips.right = maxRight - (scaledPos.x - originPos.x);
			}
		}

		// bottom
		if (scaledPos.y > originPos.y) {
			LONG maxBottom = LONG_MIN;
			for (const RECT& monitorRect : monitorRects) {
				if (monitorRect.left <= scaledPos.x && monitorRect.right > scaledPos.x) {
					maxBottom = std::max(maxBottom, monitorRect.bottom);
				}
			}
			if (maxBottom != LONG_MIN) {
				clips.bottom = maxBottom - (scaledPos.y - originPos.y);
			}
		}
	}

	if (clips == RECT{ LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX }) {
		_RestoreClipCursor();
	} else {
		_SetClipCursor(clips);
	}
}

void CursorManager::_UpdateCursorPos() noexcept {
	if (_shouldDrawCursor) {
		CURSORINFO ci{ .cbSize = sizeof(CURSORINFO) };
		if (!GetCursorInfo(&ci)) {
			_hCursor = NULL;
			return;
		}

		if (ci.flags == CURSOR_SHOWING) {
			if (ScalingWindow::Get().Options().IsWindowedMode()) {
				_hCursor = ci.hCursor;
			} else {
				// 全屏模式缩放时我们阻止了源窗口四周可调整尺寸的区域，这使得鼠标从客户区移到边框
				// 的过程中会有一瞬间的闪烁。为了解决这个问题，这里特别处理调整尺寸时的光标形状。
				if (ci.hCursor == _hDiagonalSize1Cursor || ci.hCursor == _hDiagonalSize2Cursor ||
					ci.hCursor == _hHorizontalSizeCursor || ci.hCursor == _hVerticalSizeCursor) {
					if (_hCursor != ci.hCursor) {
						using std::chrono::steady_clock;

						if (_sizeCursorStartTime == steady_clock::time_point{}) {
							_sizeCursorStartTime = steady_clock::now();
						} else {
							// 延迟 50ms 更新以防止闪烁
							if (steady_clock::now() - _sizeCursorStartTime > 50ms) {
								_hCursor = ci.hCursor;
							}
						}
					}
				} else {
					_sizeCursorStartTime = {};
					_hCursor = ci.hCursor;
				}
			}
		} else {
			_hCursor = NULL;
		}

		_cursorPos = ci.ptScreenPos;
	} else {
		_hCursor = NULL;

		// 光标在缩放窗口外也检索光标位置，叠加层可能需要
		if (!GetCursorPos(&_cursorPos)) {
			return;
		}
	}

	// 拖拽源窗口时肯定处于捕获状态
	const bool isSrcMoving = _isUnderCapture && ScalingWindow::Get().SrcTracker().IsMoving();
	// 拖拽缩放窗口时肯定不处于捕获状态而且光标在工具栏上
	const bool isScalingMoving = !_isUnderCapture &&
		ScalingWindow::Get().IsResizingOrMoving() &&
		_localCursorPosOnMoving.x != std::numeric_limits<LONG>::max();
	
	if (isSrcMoving || isScalingMoving) {
		// 拖拽源窗口和缩放窗口时确保光标位置稳定
		const RECT& rendererRect = ScalingWindow::Get().RendererRect();
		_cursorPos.x = _localCursorPosOnMoving.x + rendererRect.left;
		_cursorPos.y = _localCursorPosOnMoving.y + rendererRect.top;
	} else if (_isUnderCapture) {
		_cursorPos = SrcToScaling(_cursorPos, false);
	}
}

void CursorManager::_StartCapture(POINT& cursorPos, std::string_view reason) noexcept {
	if (_isUnderCapture) {
		return;
	}

	const Renderer& renderer = ScalingWindow::Get().Renderer();
	const RECT& destRect = renderer.DestRect();

	// 在以下情况下进入捕获状态:
	// 1. 当前未捕获
	// 2. 光标进入全屏区域
	// 
	// 进入捕获状态时: 
	// 1. 调整光标速度，全局隐藏光标
	// 2. 将光标移到源窗口的对应位置
	//
	// 在有黑边的情况下自动将光标调整到画面内

	_AdjustCursorSpeed();

	// 移动光标位置，应跳过黑边
	cursorPos = ScalingToSrc(POINT{
		std::clamp(cursorPos.x, destRect.left, destRect.right - 1),
		std::clamp(cursorPos.y, destRect.top, destRect.bottom - 1)
	});

	Logger::Get().Info(fmt::format("进入光标捕获（{}），光标移到源窗口内 ({},{})",
		reason.empty() ? std::string_view("未指定原因") : reason,
		cursorPos.x, cursorPos.y));

	_isUnderCapture = true;
}

bool CursorManager::_StopCapture(POINT& cursorPos, bool onDestroy, std::string_view reason) noexcept {
	if (!_isUnderCapture) {
		return true;
	}

	// 在以下情况下离开捕获状态:
	// 1. 当前处于捕获状态
	// 2. 光标离开源窗口客户区
	// 3. 目标位置存在屏幕
	//
	// 离开捕获状态时:
	// 1. 还原光标速度，全局显示光标
	// 2. 将光标移到全屏窗口外的对应位置
	//
	// 在有黑边的情况下自动将光标调整到全屏窗口外

	POINT newCursorPos = SrcToScaling(cursorPos, ScalingWindow::Get().SrcTracker().IsFocused());

	if (onDestroy || MonitorFromPoint(newCursorPos, MONITOR_DEFAULTTONULL)) {
		cursorPos = newCursorPos;
		_RestoreCursorSpeed();
		_isUnderCapture = false;

		const std::string_view reasonText = reason.empty() ? std::string_view("未指定原因") : reason;
		Logger::Get().Info(fmt::format("退出光标捕获（{}），光标移到 ({},{})，onDestroy={}",
			reasonText, cursorPos.x, cursorPos.y, onDestroy));

		return true;
	} else {
		// 目标位置不存在屏幕，则将光标限制在源窗口内
		const RECT& srcRect = ScalingWindow::Get().Renderer().SrcRect();

		cursorPos.x = std::clamp(cursorPos.x, srcRect.left, srcRect.right - 1);
		cursorPos.y = std::clamp(cursorPos.y, srcRect.top, srcRect.bottom - 1);

		return false;
	}
}

void CursorManager::_SetClipCursor(const RECT& clipRect, bool is3DGameMode) noexcept {
	if (ScalingWindow::Get().Options().IsDebugMode()) {
		return;
	}

	// 限制区域有变化才调用 ClipCursor，因为每次调用 ClipCursor 都会向前台窗口发送
	// WM_MOUSEMOVE 消息，一些程序无法正确处理，如 GH#920 和 GH#927。
	//
	// 曾经尝试过尊重其他程序的裁剪区域（GH#947），和我们的做交集，结果发现不切实际。
	// 一方面我们的场景很复杂，是否捕获光标、源窗口是否在前台、光标位置等都会影响限制
	// 区域，和其他程序协同几乎不可能；另一方面切换窗口后 OS 会自动清空限制区域，很难
	// 遇到需要协同的情况，不值得付出努力。
	//
	// 但多窗口下我们必须知道自己是否仍持有裁剪区：缓存与实际不一致时不能跳过设置。
	std::lock_guard<std::mutex> lock(g_clipCursorMutex);

	if (!is3DGameMode) {
		RECT curClip;
		if (!GetClipCursor(&curClip)) {
			return;
		}

		if (g_clipCursorOwner == this && curClip == _lastRealClip && clipRect == _lastClip) {
			return;
		}
	}

	if (!ClipCursor(&clipRect)) {
		Logger::Get().Win32Warn("ClipCursor 失败");
		return;
	}

	const HWND hwndScaling = ScalingWindow::Get().Handle();
	if (g_clipCursorOwner != this) {
		Logger::Get().Info(fmt::format(
			"光标裁剪区持有者切换: 0x{:X} -> 0x{:X}，区域 {}",
			(uintptr_t)g_clipCursorOwnerHwnd, (uintptr_t)hwndScaling, _FormatClipRect(clipRect)
		));
	}

	g_clipCursorOwner = this;
	g_clipCursorOwnerHwnd = hwndScaling;
	_lastClip = clipRect;
	GetClipCursor(&_lastRealClip);
}

void CursorManager::_RestoreClipCursor() noexcept {
	if (_lastClip.left == std::numeric_limits<LONG>::max()) {
		return;
	}

	std::lock_guard<std::mutex> lock(g_clipCursorMutex);

	// 其他缩放窗口已接管裁剪区，本实例的缓存已经没有意义
	if (g_clipCursorOwner != this) {
		Logger::Get().Info(fmt::format(
			"丢弃过期的光标裁剪区缓存 {}（当前持有者 0x{:X}）",
			_FormatClipRect(_lastClip), (uintptr_t)g_clipCursorOwnerHwnd
		));
		_lastClip = { std::numeric_limits<LONG>::max() };
		return;
	}

	RECT curClip;
	if (!GetClipCursor(&curClip)) {
		Logger::Get().Win32Error("GetClipCursor 失败");
		return;
	}

	// 如果其他程序或 OS 更改了光标限制区域，就放弃更改。注意必须同时放弃持有者身份，
	// 否则后续仍会按过期的缓存判断“不需要重新设置”。
	if (curClip != _lastRealClip) {
		Logger::Get().Info(fmt::format(
			"光标裁剪区已被外部修改（缓存 {}，实际 {}），放弃还原",
			_FormatClipRect(_lastRealClip), _FormatClipRect(curClip)
		));

		g_clipCursorOwner = nullptr;
		g_clipCursorOwnerHwnd = NULL;
		_lastClip = { std::numeric_limits<LONG>::max() };
		return;
	}

	if (ClipCursor(nullptr)) {
		g_clipCursorOwner = nullptr;
		g_clipCursorOwnerHwnd = NULL;
		_lastClip = { std::numeric_limits<LONG>::max() };
	} else {
		Logger::Get().Win32Warn("ClipCursor(nullptr) 失败");
	}
}

}
