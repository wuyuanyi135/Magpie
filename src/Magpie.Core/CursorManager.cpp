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

// 光标捕获的进程级归属。每个缩放窗口都有独立的 CursorManager，各自决定「是否捕获」
// 并把光标搬到自己的映射位置。两个覆盖同一屏幕区域的缩放窗口同时捕获时，会把同一个
// 系统光标在各自的映射点之间来回搬（实测 20~40ms 一轮），表现为光标瞬移、移动不跟随、
// 游戏收不到鼠标。因此同一时刻只允许一个实例处于捕获状态，其余实例必须让行。
//
// 为什么仅靠归属仲裁就够了：遮挡判定依赖 WindowFromPoint，而正在捕获的缩放窗口会被
// 设为 WS_EX_TRANSPARENT（点击穿透），会被 WindowFromPoint 跳过，导致它下面的缩放
// 窗口误判「我的画面没被遮挡」。这个错误判定只可能发生在对方正处于捕获状态时——
// 也就是恰好是持有者身上——让行规则正好拦住它。
static std::mutex g_captureOwnerMutex;
static const CursorManager* g_captureOwner = nullptr;
static HWND g_captureOwnerHwnd = NULL;
// 换手节流：正常换手（用户切到另一个缩放窗口）必须立刻生效，所以不能用固定冷却时间；
// 只对「来回抢」设上限——500ms 内最多换手 4 次，超过就让行，避免两个窗口在边界处
// 高频抖动把光标搬来搬去
static std::array<std::chrono::steady_clock::time_point, 4> g_captureSwitchTimes{};
static size_t g_captureSwitchIndex = 0;
static constexpr auto CAPTURE_SWITCH_WINDOW = std::chrono::milliseconds(500);

// 查询当前捕获持有者，用于诊断日志
static std::pair<HWND, bool> QueryCaptureOwner(const CursorManager* self) noexcept {
	std::lock_guard<std::mutex> lock(g_captureOwnerMutex);
	return { g_captureOwnerHwnd, g_captureOwner == self };
}

static bool IsCaptureOwner(const CursorManager* self) noexcept {
	std::lock_guard<std::mutex> lock(g_captureOwnerMutex);
	return g_captureOwner == self;
}

static bool IsCaptureHeldByOther(const CursorManager* self) noexcept {
	std::lock_guard<std::mutex> lock(g_captureOwnerMutex);
	return g_captureOwner && g_captureOwner != self;
}

// 尝试登记为光标捕获持有者。失败时通过 blockReason 给出让行原因（供诊断日志使用）。
// 注意 blockReason 只能指向静态字符串，调用方持有的是 string_view。
static bool TryAcquireCapture(
	const CursorManager* self,
	HWND hwndScaling,
	std::string_view* blockReason
) noexcept {
	std::lock_guard<std::mutex> lock(g_captureOwnerMutex);

	if (g_captureOwner == self) {
		return true;
	}

	const auto now = std::chrono::steady_clock::now();

	if (g_captureOwner) {
		if (g_captureOwnerHwnd && IsWindow(g_captureOwnerHwnd)) {
			if (blockReason) {
				*blockReason = "另一个缩放窗口持有光标捕获";
			}
			return false;
		}

		// 持有者的窗口已经销毁（例如对应的缩放已结束），回收后继续
		Logger::Get().Info(fmt::format("光标捕获持有者 0x{:X} 的窗口已销毁，回收",
			(uintptr_t)g_captureOwnerHwnd));
		g_captureOwner = nullptr;
		g_captureOwnerHwnd = NULL;
	}

	size_t recentSwitches = 0;
	for (const std::chrono::steady_clock::time_point& t : g_captureSwitchTimes) {
		if (t != std::chrono::steady_clock::time_point{} && now - t < CAPTURE_SWITCH_WINDOW) {
			++recentSwitches;
		}
	}

	if (recentSwitches >= g_captureSwitchTimes.size()) {
		if (blockReason) {
			*blockReason = "光标捕获换手过于频繁";
		}
		return false;
	}

	if (Logger::Get().IsEnabled(spdlog::level::debug)) {
		Logger::Get().Debug(fmt::format("光标捕获持有者切换: 0x{:X} -> 0x{:X}（{} ms 内第 {} 次）",
			(uintptr_t)g_captureOwnerHwnd, (uintptr_t)hwndScaling,
			CAPTURE_SWITCH_WINDOW.count(), recentSwitches + 1));
	}

	g_captureSwitchTimes[g_captureSwitchIndex] = now;
	g_captureSwitchIndex = (g_captureSwitchIndex + 1) % g_captureSwitchTimes.size();

	g_captureOwner = self;
	g_captureOwnerHwnd = hwndScaling;
	return true;
}

static void ReleaseCapture(const CursorManager* self) noexcept {
	std::lock_guard<std::mutex> lock(g_captureOwnerMutex);

	if (g_captureOwner != self) {
		return;
	}

	if (Logger::Get().IsEnabled(spdlog::level::debug)) {
		Logger::Get().Debug(fmt::format("光标捕获持有者释放: 0x{:X}",
			(uintptr_t)g_captureOwnerHwnd));
	}

	g_captureOwner = nullptr;
	g_captureOwnerHwnd = NULL;
}

// 存活的所有缩放窗口。多窗口间的让行与点击穿透判定需要知道「还有谁覆盖着同一块屏幕」。
// 不能用 WindowFromPoint 判断：正在捕获的缩放窗口是 WS_EX_TRANSPARENT，会被它跳过。
static std::mutex g_scalingWindowsMutex;
static std::vector<HWND> g_scalingWindows;

static void RegisterScalingWindow(HWND hwnd) noexcept {
	std::lock_guard<std::mutex> lock(g_scalingWindowsMutex);
	if (std::find(g_scalingWindows.begin(), g_scalingWindows.end(), hwnd) == g_scalingWindows.end()) {
		g_scalingWindows.push_back(hwnd);
	}
}

static void UnregisterScalingWindow(HWND hwnd) noexcept {
	std::lock_guard<std::mutex> lock(g_scalingWindowsMutex);
	std::erase_if(g_scalingWindows, [hwnd](HWND h) { return h == hwnd; });
}

// 光标处是否覆盖着另一个更靠前的缩放窗口。Magpie 的焦点逻辑会把「源窗口在前台」的
// 缩放窗口设为置顶，因此置顶与否就是多窗口之间的前后关系：非置顶的实例应当让行。
static bool ShouldYieldToSibling(HWND hwndMine, POINT screenPt) noexcept {
	if (!hwndMine) {
		return false;
	}

	// 两个都置顶（或多显示器上互不重叠）时按先到先得，不由置顶状态决定
	if ((DWORD)GetWindowExStyle(hwndMine) & WS_EX_TOPMOST) {
		return false;
	}

	std::lock_guard<std::mutex> lock(g_scalingWindowsMutex);
	for (HWND h : g_scalingWindows) {
		if (h == hwndMine || !IsWindow(h) || !IsWindowVisible(h)) {
			continue;
		}

		if (!((DWORD)GetWindowExStyle(h) & WS_EX_TOPMOST)) {
			continue;
		}

		RECT rect;
		if (GetWindowRect(h, &rect) && Win32Helper::PtInRect(rect, screenPt)) {
			return true;
		}
	}

	return false;
}

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

	// 兜底：即使状态不一致也不要让持有者身份泄漏到下一个实例
	ReleaseCapture(this);

	if (_hwndRegistered) {
		UnregisterScalingWindow(_hwndRegistered);
		_hwndRegistered = NULL;
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

	if (Logger::Get().IsEnabled(spdlog::level::debug)) {
		Logger::Get().Debug(fmt::format("叠加层光标状态: 光标{}叠加层上", value ? "位于" : "离开"));
	}

	_UpdateCursorState();
	_UpdateCursorPos();
}

void CursorManager::IsCursorCapturedOnOverlay(bool value) noexcept {
	if (_isCapturedOnOverlay == value) {
		return;
	}
	_isCapturedOnOverlay = value;

	if (Logger::Get().IsEnabled(spdlog::level::debug)) {
		Logger::Get().Debug(fmt::format("叠加层光标状态: {}捕获光标", value ? "开始" : "结束"));
	}

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

	// 收起期间不会再有 Update，必须在这里交出捕获归属，否则另一个缩放窗口会一直
	// 读到「有人持有」而让行（持有者只有在 Update 里才会被释放）
	ReleaseCapture(this);

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
// 某个屏幕点上真正会接收鼠标的窗口，跳过本进程的所有缩放窗口。缩放窗口是盖在源窗口
// 上的叠加层，捕获期间点击穿透、未捕获时又会自己接收鼠标，两者都不是「遮挡源窗口的
// 窗口」，判断点击会落到谁身上时必须把它们排除掉
static HWND WindowBelowScalingWindows(POINT pt) noexcept {
	struct EnumData {
		POINT pt;
		std::vector<HWND> scalingWindows;
		HWND result;
	} data{ pt, {}, NULL };
	{
		std::lock_guard<std::mutex> lock(g_scalingWindowsMutex);
		data.scalingWindows = g_scalingWindows;
	}

	EnumWindows([](HWND hWnd, LPARAM lParam) {
		EnumData& data = *(EnumData*)lParam;
		if (std::find(data.scalingWindows.begin(), data.scalingWindows.end(), hWnd) !=
			data.scalingWindows.end()) {
			return TRUE;
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

// 是否需要「源窗口是否被别的窗口遮挡」这一判定。只有桌面复制抓的是屏幕上的那一块
// 区域，画面里会出现盖在源窗口上的窗口，鼠标应当交给它们。窗口级捕获（WGC / DWM 共享
// 表面 / GDI）的画面只包含源窗口自身——此时按源坐标判定遮挡，会把用户根本看不见的
// 窗口（另一个游戏客户端、后面的浏览器）当成遮挡：缩放窗口既不进入捕获、又被设为
// 不透明，鼠标输入无处可去，形成死区，只有切出切回才恢复
static bool NeedsSourceOcclusionCheck(const ScalingOptions& options) noexcept {
	return options.captureMethod == CaptureMethod::DesktopDuplication;
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

	// 多窗口时的捕获归属：不是持有者却想捕获的实例就是被让行规则拦下的那个
	const auto [captureOwnerHwnd, isCaptureOwner] = QueryCaptureOwner(this);

	Logger::Get().Info(fmt::format(
		"[光标诊断:{}] 更新#{} 捕获={} 绘制={} 前台捕获={} 叠加层={}/{}(捕获/光标) "
		"hitTest={}(已完成位置=({},{}))"
		" | 光标=({},{}) 源坐标=({},{})"
		" | 源窗口=0x{:X}(前台={} 移动={}) 前台窗口=0x{:X} 前台捕获窗口=0x{:X} 前台GUI标志=0x{:X}"
		" | 缩放窗口=0x{:X}(置顶={} 透明={} 可见={} 最小化={}) 捕获持有者=0x{:X}(本实例={})"
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
		(uintptr_t)captureOwnerHwnd, isCaptureOwner,
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
	// 相同原因 3 秒内只记一次。按原因分槽限流：让行、跳过裁剪区等不同原因会交替
	// 出现，否则会互相顶掉限流状态，变成每帧一条
	size_t reasonHash = 1469598103934665603ull;
	for (char c : reason) {
		reasonHash ^= (size_t)(uint8_t)c;
		reasonHash *= 1099511628211ull;
	}
	reasonHash |= 1;	// 0 表示该槽未使用

	const auto now = std::chrono::steady_clock::now();
	const size_t slot = reasonHash % BLOCK_LOG_SLOT_COUNT;

	if (_blockLogReasonHashes[slot] == reasonHash &&
		now - _blockLogTimes[slot] < std::chrono::seconds(3)) {
		return;
	}
	_blockLogReasonHashes[slot] = reasonHash;
	_blockLogTimes[slot] = now;

	const auto [ownerHwnd, isOwner] = QueryCaptureOwner(this);
	_LogStateSnapshot(fmt::format("未能进入光标捕获: {}{}；捕获持有者=0x{:X}（本实例={}）",
		reason,
		hwndCur ? fmt::format("（光标处窗口 0x{:X}）", (uintptr_t)hwndCur) : std::string(),
		(uintptr_t)ownerHwnd, isOwner));
}

void CursorManager::_UpdateCursorState() noexcept {
	++_updateCount;

	// 首次更新时登记本窗口，供多窗口间的让行与点击穿透判定使用
	const HWND hwndOwn = ScalingWindow::Get().Handle();
	if (hwndOwn && hwndOwn != _hwndRegistered) {
		_hwndRegistered = hwndOwn;
		RegisterScalingWindow(hwndOwn);
	}

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

			std::string_view acquireBlock;
			if (!_StartCapture(cursorPos, "3D 游戏模式", &acquireBlock)) {
				// 另一个缩放窗口持有光标捕获：让行，既不隐藏系统光标也不改点击穿透
				_shouldDrawCursor = false;
				_LogCaptureBlocked(acquireBlock, NULL);
				return;
			}

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
			if (!_loggedForegroundCaptureStart) {
				_loggedForegroundCaptureStart = true;
				if (Logger::Get().IsEnabled(spdlog::level::debug)) {
					Logger::Get().Debug(fmt::format(
						"前台窗口开始捕获光标: hwndCapture=0x{:X}（源窗口=0x{:X}，缩放窗口=0x{:X}），GUI 标志=0x{:X}",
						(uintptr_t)info.hwndCapture, (uintptr_t)hwndSrc,
						(uintptr_t)ScalingWindow::Get().Handle(), (uintptr_t)info.flags));
				}
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

			// 源窗口自己长期持有光标捕获（很多游戏在鼠标锁定 / mouse-look 时 SetCapture
			// 之后不释放）时不能像拖拽那样直接跳过：真实光标还留在缩放窗口上，源窗口收到
			// 的坐标全部落在客户区之外，表现为「游戏内光标不响应」，只有切出切回（捕获被
			// 解除）才能恢复。这种情况继续走下面的正常流程，由 _StartCapture 把光标搬进
			// 源窗口
			const bool captureOwnedBySrc = info.hwndCapture == hwndSrc ||
				(IsChild(hwndSrc, info.hwndCapture) &&
					(GetWindowStyle(info.hwndCapture) & WS_CHILD));
			const bool fallThrough = captureOwnedBySrc && !_isUnderCapture &&
				!_isOnOverlay && !(info.flags & GUI_INMOVESIZE);

			if (fallThrough) {
				if (!_loggedSrcCaptureFallthrough) {
					_loggedSrcCaptureFallthrough = true;
					Logger::Get().Info(fmt::format(
						"源窗口持有光标捕获（hwndCapture=0x{:X}），本窗口未捕获，继续正常流程尝试进入捕获",
						(uintptr_t)info.hwndCapture));
				}
			} else {
				// 当光标被前台窗口捕获时我们除了限制光标外什么也不做，即光标
				// 可以在缩放窗口上自由移动。
				return;
			}
		}

		if (_loggedForegroundCaptureStart) {
			_loggedForegroundCaptureStart = false;
			if (Logger::Get().IsEnabled(spdlog::level::debug)) {
				Logger::Get().Debug("前台窗口已释放光标");
			}
		}
		_loggedSrcCaptureFallthrough = false;

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
		if (Logger::Get().IsEnabled(spdlog::level::debug)) {
			Logger::Get().Debug(fmt::format("缩放窗口点击穿透: {}",
				value ? "开启（鼠标消息送往源窗口）" : "关闭（缩放窗口自己接收鼠标）"));
		}
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
		// 光标处覆盖着另一个更靠前的缩放窗口时，光标归它管理，本实例让行
		const bool yieldToSibling =
			hwndCur == hwndScaling && ShouldYieldToSibling(hwndScaling, cursorPos);
		_shouldDrawCursor = hwndCur == hwndScaling && !yieldToSibling;

		if (_shouldDrawCursor) {
			// 缩放窗口未被遮挡
			bool stopCapture = _isOnOverlay;

			if (!stopCapture) {
				// 检查源窗口是否被遮挡。窗口级捕获的画面里不会出现盖在源窗口上的窗口，
				// 这种判定会误伤（见 NeedsSourceOcclusionCheck 的注释）
				if (NeedsSourceOcclusionCheck(options)) {
					hwndCur = WindowFromPoint(hwndScaling, rendererRect, cursorPos, true);

					stopCapture = hwndCur != hwndSrc &&
						(!IsChild(hwndSrc, hwndCur) || !(GetWindowStyle(hwndCur) & WS_CHILD));
				}

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

			if (!_StopCapture(cursorPos, false,
				yieldToSibling ? "另一个缩放窗口在前台（置顶）" : "缩放窗口被遮挡"))
			{
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
		// 光标处覆盖着另一个更靠前的缩放窗口时，光标归它管理，本实例让行
		const bool yieldToSibling =
			hwndCur == hwndScaling && ShouldYieldToSibling(hwndScaling, cursorPos);
		_shouldDrawCursor = hwndCur == hwndScaling && !yieldToSibling;

		if (_shouldDrawCursor) {
			// 缩放窗口未被遮挡
			POINT newCursorPos = ScalingToSrc(cursorPos);

			if (Win32Helper::PtInRect(srcRect, newCursorPos)) {
				bool startCapture = !_isOnOverlay;
				if (!startCapture) {
					blockReason = "光标位于叠加层上";
				}

				if (startCapture) {
					// 检查源窗口是否被遮挡。窗口级捕获的画面里不会出现盖在源窗口上的
					// 窗口，这种判定会误伤（见 NeedsSourceOcclusionCheck 的注释）
					if (NeedsSourceOcclusionCheck(options)) {
						hwndCur = WindowFromPoint(hwndScaling, rendererRect, newCursorPos, true);

						startCapture = hwndCur == hwndSrc ||
							(IsChild(hwndSrc, hwndCur) && (GetWindowStyle(hwndCur) & WS_CHILD));

						if (!startCapture) {
							blockReason = "源窗口在光标处被遮挡";
							blockHwnd = hwndCur;
						}
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

					if (!_StartCapture(cursorPos, "光标位于画面内且源窗口未被遮挡", &blockReason)) {
						// 让行：本实例不绘制光标，隐藏系统光标与绘制交给持有者
						_shouldDrawCursor = false;
					}
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

						if (!_StartCapture(cursorPos, "光标从黑边外移入画面", &blockReason)) {
							_shouldDrawCursor = false;
						}
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

						if (!_StartCapture(cursorPos, "源窗口不在前台，光标进入输出区域", &blockReason)) {
							_shouldDrawCursor = false;
						}
					} else {
						setClickThrough(false);

						blockReason = "光标在黑边内，且源窗口不在前台";
					}
				} else {
					blockReason = "光标在黑边内，且位于叠加层上";
				}
			}
		} else if (yieldToSibling) {
			blockReason = "另一个缩放窗口在前台（置顶），本实例让行";
		}
	}

	if (shouldClearHitTestResult) {
		_ClearHitTestResult();
	}

	// 多窗口保底：本实例不持有光标，而光标处覆盖着另一个置顶的缩放窗口时，必须保持
	// 点击穿透。持有者的缩放窗口是透明的，鼠标消息会穿透它落到本窗口上，本窗口若参与
	// 命中测试就会把输入吃掉——这正是「鼠标失焦、游戏收不到鼠标」的直接原因。
	//
	// 反过来，不再让行且没有别的实例持有光标时必须恢复不透明，否则本窗口永远无法被
	// 命中，也就再也无法重新捕获光标（透明是自锁的：WindowFromPoint 会跳过自己）。
	if (hwndScaling) {
		POINT realCursorPos;
		if (GetCursorPos(&realCursorPos)) {
			const bool yield = !_isUnderCapture && !_isOnOverlay &&
				ShouldYieldToSibling(hwndScaling, realCursorPos);

			if (yield) {
				setClickThrough(true);
			} else if (!_isUnderCapture && !_isOnOverlay &&
				!IsCaptureHeldByOther(this) &&
				Win32Helper::PtInRect(rendererRect, realCursorPos))
			{
				setClickThrough(false);
			}
		}
	}

	// 光标位于缩放窗口上却没有进入捕获：此时系统光标被隐藏、由 Magpie 绘制光标，但源窗口
	// 收不到鼠标输入，表现为「光标不跟随 / 游戏光标没反应」。记录一次状态快照便于定位。
	if (!blockReason.empty() && !_isUnderCapture) {
		_LogCaptureBlocked(blockReason, blockHwnd);
	}

	// 真实系统光标的可见性可能被其他程序或 OS 改变，而我们只缓存了期望值，因此每 5 秒
	// 核对一次，只在变化时记录
	{
		const auto now2 = std::chrono::steady_clock::now();
		if (now2 - _lastHeartbeatTime >= std::chrono::seconds(5)) {
			_lastHeartbeatTime = now2;

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
		}
	}

	// 状态一变就记录完整快照。正常运行时不需要，只在排查难以复现的输入问题时打开
	// （MAGPIE_LOG_LEVEL=debug）
	if (Logger::Get().IsEnabled(spdlog::level::debug)) {
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

			_LogStateSnapshot("状态变更");
		}
	}

	// 只要光标缩放后的位置在缩放窗口上，且该位置未被其他窗口遮挡，便可以隐藏光标。
	// 即使当前并未捕获光标也是如此。
	_ShowSystemCursor(!_shouldDrawCursor);

	_ClipCursorForMonitors(cursorPos);

	// SetCursorPos 应在 ClipCursor 之后，否则会受到上一次 ClipCursor 的影响
	if (cursorPos != originCursorPos) {
		// 另一个缩放窗口已接管光标：此刻只有它的映射有效，本实例再搬一次会把它踩掉，
		// 两个窗口就会开始互相抢光标
		if (IsCaptureHeldByOther(this)) {
			_LogCaptureBlocked("跳过光标移动：另一个缩放窗口持有光标捕获", NULL);
		} else {
			_ReliableSetCursorPos(cursorPos);
		}
	}

	// 光标位置可能改变，需要更新
	_UpdateCursorPos();

	// 回归哨兵：光标由 Magpie 绘制、源窗口在前台，却没有处于捕获状态，意味着源窗口收不到
	// 鼠标输入——点击穿透、遮挡判定、多窗口让行等所有已知缺陷最终都表现为这个状态。
	// 持续 1 秒仍未恢复就报警一次，这样以后的改动若悄悄退化，日志里会直接留下证据，
	// 而不是等到用户复现时才发现「什么都没有记录」
	const bool cursorLost = _shouldDrawCursor && !_isUnderCapture &&
		!_isOnOverlay && !_isCapturedOnOverlay && ScalingWindow::Get().SrcTracker().IsFocused();
	if (cursorLost) {
		const auto canaryNow = std::chrono::steady_clock::now();
		if (_lostCaptureSince == std::chrono::steady_clock::time_point{}) {
			_lostCaptureSince = canaryNow;
		} else if (!_loggedCursorLost && canaryNow - _lostCaptureSince >= std::chrono::seconds(1)) {
			_loggedCursorLost = true;
			Logger::Get().Warn(fmt::format(
				"光标已由 Magpie 绘制但未进入捕获超过 1 秒，源窗口收不到鼠标输入"
				"（光标=({},{})，源窗口=0x{:X}，缩放窗口=0x{:X}，点击穿透={}）",
				_cursorPos.x, _cursorPos.y,
				(uintptr_t)ScalingWindow::Get().SrcTracker().Handle(),
				(uintptr_t)ScalingWindow::Get().Handle(),
				((DWORD)GetWindowExStyle(ScalingWindow::Get().Handle()) & WS_EX_TRANSPARENT) != 0));
		}
	} else {
		_lostCaptureSince = {};
		_loggedCursorLost = false;
	}
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

bool CursorManager::_StartCapture(
	POINT& cursorPos,
	std::string_view reason,
	std::string_view* blockReason
) noexcept {
	if (_isUnderCapture) {
		return true;
	}

	// 多窗口：同一时刻只允许一个缩放窗口捕获光标。另一个覆盖同一区域的缩放窗口若也
	// 进入捕获，会把光标搬到它自己的映射位置，两边来回搬就形成 20~40ms 的抖动
	if (!TryAcquireCapture(this, ScalingWindow::Get().Handle(), blockReason)) {
		return false;
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

	// 捕获期间缩放窗口是点击穿透的，鼠标消息会落到该屏幕点最上面的窗口。如果有别的窗口
	// 盖在源窗口之上（多客户端时很常见，例如另一个游戏窗口正好压在源窗口左上角），点击
	// 就会激活那个窗口——表现为「某一小块区域仿佛变成了另一个客户端」。
	// 源窗口就是当前画面显示的内容，把它抬到最前，保证输入落到它身上。
	_EnsureSourceNotCovered(cursorPos);

	return true;
}

void CursorManager::_EnsureSourceNotCovered(POINT srcPos) noexcept {
	ScalingWindow& scalingWindow = ScalingWindow::Get();
	const HWND hwndSrc = scalingWindow.SrcTracker().Handle();
	if (!hwndSrc) {
		return;
	}

	// 只看真正接收鼠标的窗口：别的缩放窗口正盖在这里也不算遮挡源窗口
	const HWND hwndAbove = WindowBelowScalingWindows(srcPos);
	if (!hwndAbove || hwndAbove == hwndSrc || IsChild(hwndSrc, hwndAbove)) {
		return;
	}

	// 覆盖源窗口的正是用户当前在用的窗口时不抢层级，否则会把它强行压在游戏下面
	if (hwndAbove == GetForegroundWindow()) {
		_LogCaptureBlocked("源窗口被当前前台窗口覆盖，未调整层级", hwndAbove);
		return;
	}

	if (!SetWindowPos(hwndSrc, HWND_TOP, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
		Logger::Get().Win32Error(fmt::format(
			"抬升源窗口 0x{:X} 失败（被 0x{:X} 覆盖）", (uintptr_t)hwndSrc, (uintptr_t)hwndAbove));
		return;
	}

	// 正常情况下抬升后就不再被覆盖，只有用户又激活了别的窗口才会重复出现，因此不必限流
	Logger::Get().Info(fmt::format(
		"源窗口 0x{:X} 被 0x{:X} 覆盖，已将其置于最前，避免点击落到别的窗口",
		(uintptr_t)hwndSrc, (uintptr_t)hwndAbove));
}

bool CursorManager::_StopCapture(POINT& cursorPos, bool onDestroy, std::string_view reason) noexcept {
	if (!_isUnderCapture) {
		return true;
	}

	// 处于捕获状态时本实例理应是持有者。不一致说明登记被其他路径清掉过，记录下来
	// 便于排查，同时仍按正常流程退出，避免状态卡死
	const bool wasOwner = IsCaptureOwner(this);
	if (!wasOwner) {
		Logger::Get().Warn("退出光标捕获时发现本实例并非持有者（捕获状态与登记不一致）");
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
		// 只有持有者才把光标搬回缩放坐标：另一个实例可能已经接管并用它自己的映射
		// 写过光标位置，此时再搬一次会把它踩掉（多窗口抖动的一半来自这里）
		if (wasOwner) {
			cursorPos = newCursorPos;
		}

		_RestoreCursorSpeed();
		_isUnderCapture = false;
		ReleaseCapture(this);

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

	// 多窗口：裁剪区是桌面级全局状态，也只应由光标捕获的持有者控制。否则两个实例会
	// 交替写入自己的裁剪区域，把光标在两点之间夹来夹去（持有者的 1×1 临时裁剪区
	// 也会被另一个实例覆盖，导致「光标移到源窗口内」这一步失效）
	if (IsCaptureHeldByOther(this)) {
		_LogCaptureBlocked("跳过光标裁剪区设置：另一个缩放窗口持有光标捕获", NULL);
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
		if (Logger::Get().IsEnabled(spdlog::level::debug)) {
			Logger::Get().Debug(fmt::format(
				"光标裁剪区持有者切换: 0x{:X} -> 0x{:X}，区域 {}",
				(uintptr_t)g_clipCursorOwnerHwnd, (uintptr_t)hwndScaling, _FormatClipRect(clipRect)
			));
		}
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
