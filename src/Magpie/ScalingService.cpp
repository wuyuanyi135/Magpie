#include "pch.h"
#include "ScalingService.h"
#include "App.h"
#include "AppSettings.h"
#include "EffectsService.h"
#include "LocalizationService.h"
#include "Logger.h"
#include "ProfileService.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "ShortcutService.h"
#include "ToastService.h"
#include "TouchHelper.h"
#include "Win32Helper.h"
#include "WindowHelper.h"

using namespace winrt::Magpie::implementation;
using namespace winrt;

using winrt::Magpie::ShortcutAction;

namespace Magpie {

// 同时缩放的最大窗口数由设置项控制（见 AppSettings::MaxScalingWindows）。
// 每个缩放运行时占用一条线程和一整套 D3D/捕获资源，窗口数过多会导致显存
// 与内存压力失控，因此仍需上限。

// 定义于 Magpie.Core/CursorManager.cpp。Magpie 项目的包含目录只有
// Magpie.Core/include，无法直接包含 CursorManager.h，因此前向声明。
void RestoreCursorSpeedIfCrashed() noexcept;

ScalingService::~ScalingService() {}

void ScalingService::Initialize() {
	// 缩放运行时改为按需创建：每个源窗口一个（见 _GetOrCreateRuntime）

	// 上次若在光标速度被调慢期间异常退出，系统光标速度会停留在调慢值，这里还原
	RestoreCursorSpeedIfCrashed();

	const DispatcherQueue& dispatcher = App::Get().Dispatcher();

	_countDownTimer = dispatcher.CreateTimer();
	_countDownTimer.Interval(25ms);
	_countDownTimer.Tick({ this, &ScalingService::_CountDownTimer_Tick });

	_checkForegroundTimer = dispatcher.CreateTimer();
	_checkForegroundTimer.Interval(50ms);
	_checkForegroundTimer.Tick({ this, &ScalingService::_CheckForegroundTimer_Tick });
	_checkForegroundTimer.Start();
	
	_shortcutActivatedRevoker = ShortcutService::Get().ShortcutActivated(
		auto_revoke, std::bind_front(&ScalingService::_ShortcutService_ShortcutPressed, this));

	// 立即检查前台窗口
	_CheckForegroundTimer_Tick(nullptr, nullptr);
}

void ScalingService::Uninitialize() noexcept {
	// 计时器在 Initialize 中创建，容忍未初始化就调用（如管理员自提升分支提前退出）
	if (_checkForegroundTimer) {
		_checkForegroundTimer.Stop();
	}
	if (_countDownTimer) {
		_countDownTimer.Stop();
	}

	_runtimes.clear();

	_shortcutActivatedRevoker.Revoke();
}

void ScalingService::StartTimer(bool windowedMode) {
	_curCountdownSeconds = AppSettings::Get().CountdownSeconds();
	_isCurCountdownWindowedMode = windowedMode;
	_timerStartTimePoint = std::chrono::steady_clock::now();
	// 如果计时器已经启动会被重置，这正是我们想要的
	_countDownTimer.Start();
	IsTimerOnChanged.Invoke(true, windowedMode);
}

void ScalingService::StopTimer() {
	if (_curCountdownSeconds == 0) {
		return;
	}

	_curCountdownSeconds = 0;
	_countDownTimer.Stop();
	IsTimerOnChanged.Invoke(false, _isCurCountdownWindowedMode);
}

double ScalingService::SecondsLeft() const noexcept {
	using namespace std::chrono;

	if (!IsTimerOn()) {
		return std::numeric_limits<double>::max();
	}

	// DispatcherTimer 误差很大，因此我们自己计算剩余时间
	auto now = steady_clock::now();
	int msLeft = (int)duration_cast<milliseconds>(_timerStartTimePoint + seconds(_curCountdownSeconds) - now).count();
	return msLeft / 1000.0;
}

bool ScalingService::IsScaling() const noexcept {
	// 等待状态视为未缩放
	// 任一源窗口正在缩放即视为正在缩放
	for (const auto& [hwnd, rt] : _runtimes) {
		if (rt->State() == ScalingState::Scaling) {
			return true;
		}
	}
	return false;
}

void ScalingService::CheckForeground() {
	_hwndChecked = NULL;
	_CheckForegroundTimer_Tick(nullptr, nullptr);
}

void ScalingService::_ShortcutService_ShortcutPressed(ShortcutAction action) {
	switch (action) {
	case ShortcutAction::Scale:
	case ShortcutAction::WindowedModeScale:
	{
		const bool isWindowdMode = action == ShortcutAction::WindowedModeScale;

		// 多窗口并列：每个源窗口各自一个运行时。若当前前台窗口已在缩放则停止它
		// （其余窗口不受影响），否则为它启动缩放，从而实现多个窗口同时放大。
		HWND hwndFore = GetForegroundWindow();
		if (!hwndFore) {
			break;
		}

		// 全屏缩放时前台窗口往往是我们的缩放窗口本身，此时应视为它的源窗口，
		// 否则会误以为要对缩放窗口再做一次缩放
		for (const auto& kv : _runtimes) {
			const HWND hwndScaling = kv.second->ScalingWindowHandle();
			if (hwndScaling && hwndScaling == hwndFore) {
				hwndFore = kv.first;
				break;
			}
		}

		auto it = _runtimes.find(hwndFore);
		if (it != _runtimes.end() && it->second->State() == ScalingState::Scaling) {
			it->second->Stop();
		} else {
			_ScaleForegroundWindow(isWindowdMode, hwndFore);
		}

		break;
	}
	case ShortcutAction::Toolbar:
	{
		if (ScalingRuntime* rt = _ActiveRuntime()) {
			rt->SwitchToolbarState();
		}
		break;
	}
	case ShortcutAction::TakeScreenshot:
	{
		if (ScalingRuntime* rt = _ActiveRuntime()) {
			rt->TakeScreenshot();
		}
		break;	
	}
	case ShortcutAction::BackgroundRendering:
	{
		ToggleBackgroundRendering();
		break;
	}
	default:
		break;
	}
}

void ScalingService::_CountDownTimer_Tick(winrt::DispatcherQueueTimer const&, winrt::IInspectable const&) {
	const double timeLeft = SecondsLeft();

	// 剩余时间在 10 ms 以内计时结束
	if (timeLeft < 0.01) {
		StopTimer();
		_ScaleForegroundWindow(_isCurCountdownWindowedMode);
		return;
	}

	TimerTick.Invoke(timeLeft);
}

static void ShowError(HWND hWnd, ScalingError error) noexcept {
	const wchar_t* key = nullptr;

	bool isFail = true;
	switch (error) {
	case ScalingError::InvalidScalingMode:
		key = L"Message_InvalidScalingMode";
		isFail = false;
		break;
	case ScalingError::TouchSupport:
		key = L"Message_TouchSupport";
		break;
	case ScalingError::Windowed3DGameMode:
		key = L"Message_Windowed3DGameMode";
		isFail = false;
		break;
	case ScalingError::WindowedDesktopDuplication:
		key = L"Message_WindowedDesktopDuplication";
		isFail = false;
		break;
	case ScalingError::InvalidSourceWindow:
		key = L"Message_InvalidSourceWindow";
		break;
	case ScalingError::Maximized:
		key = L"Message_Maximized";
		isFail = false;
		break;
	case ScalingError::LowIntegrityLevel:
		key = L"Message_LowIntegrityLevel";
		isFail = false;
		break;
	case ScalingError::InvalidCropping:
		key = L"Message_InvalidCropping";
		break;
	case ScalingError::BannedInWindowedMode:
		key = L"Message_BannedInWindowedMode";
		isFail = false;
		break;
	case ScalingError::ScalingFailedGeneral:
		key = L"Message_ScalingFailedGeneral";
		break;
	case ScalingError::CaptureFailed:
		key = L"Message_CaptureFailed";
		break;
	case ScalingError::CreateFenceFailed:
		key = L"Message_CreateFenceFailed";
		break;
	case ScalingError::TooManyScalingWindows:
		key = L"Message_TooManyScalingWindows";
		break;
	default:
		assert(false);
		return;
	}

	LocalizationService& ls = LocalizationService::Get();
	hstring title = isFail ? ls.GetLocalizedString(L"Message_ScalingFailed") : hstring{};
	hstring message = ls.GetLocalizedString(key);
	if (error == ScalingError::TooManyScalingWindows) {
		// 该文案带有窗口数上限的占位符
		message = fmt::format(fmt::runtime(std::wstring_view(message)),
			AppSettings::Get().MaxScalingWindows()).c_str();
	}
	ToastService::Get().ShowMessageOnWindow(title, message, hWnd);
	Logger::Get().Error(fmt::format("缩放失败\n\t错误码: {}", (int)error));
}

static bool IsPopupWindow(HWND hwndPopup, HWND hwndOwner) noexcept {
	// 检查所有者关系
	{
		HWND hwndCur = hwndPopup;
		while (bool(hwndCur = GetWindowOwner(hwndCur))) {
			if (hwndCur == hwndOwner) {
				return true;
			}
		}
	}
	
	// 有些游戏不用所有者关系来实现弹窗，而是将主窗口禁用，做到和模态弹窗差不多的效果。
	// 这不可能准确检测，只能尽可能增加限制以减少误判，我们检查三个条件：
	// 
	// 1. 主窗口处于禁用状态
	// 2. 两个窗口位于同一个进程
	// 3. 主窗口没有传统意义的弹窗

	if (IsWindowEnabled(hwndOwner)) {
		return false;
	}

	DWORD pid1 = 0;
	DWORD pid2 = 0;
	GetWindowThreadProcessId(hwndPopup, &pid1);
	GetWindowThreadProcessId(hwndOwner, &pid2);
	if (pid1 != pid2) {
		return false;
	}

	return !GetWindow(hwndOwner, GW_ENABLEDPOPUP);
}

static bool IsReadyForScaling(HWND hwndFore) noexcept {
	// GH#1148
	// 有些游戏刚启动时将窗口创建在屏幕外，初始化完成后再移到屏幕内
	if (!MonitorFromWindow(hwndFore, MONITOR_DEFAULTTONULL)) {
		return false;
	}

	// GH#1200
	// 有些游戏加载时不响应消息，应等待加载完成
	return !Win32Helper::IsWindowHung(hwndFore);
}

void ScalingService::_CheckForegroundTimer_Tick(winrt::DispatcherQueueTimer const&, winrt::IInspectable const&) {
	const HWND hwndFore = GetForegroundWindow();
	if (!hwndFore || hwndFore == _hwndChecked) {
		return;
	}

	// 前台窗口是我们自己的缩放窗口时不触发自动缩放
	for (const auto& kv : _runtimes) {
		const HWND hwndScaling = kv.second->ScalingWindowHandle();
		if (hwndScaling && hwndScaling == hwndFore) {
			_hwndChecked = hwndFore;
			return;
		}
	}

	if (hwndFore != _hwndCurSrc) {
		// 检查自动缩放
		const Profile* profile = ProfileService::Get().GetProfileForWindow(hwndFore, true);
		// 正在缩放窗口时禁止自动缩放它的弹窗
		if (profile && !(_hwndCurSrc && IsPopupWindow(hwndFore, _hwndCurSrc))) {
			// 如果窗口处于某种中间状态则跳过此次检查
			if (!IsReadyForScaling(hwndFore)) {
				return;
			}

			// 自动缩放可以终止当前缩放
			_StartScale(hwndFore, *profile, profile->autoScale == AutoScale::Windowed, true);
		}
	}

	// 避免重复检查
	_hwndChecked = hwndFore;
}

void ScalingService::_ScalingRuntime_StateChanged(ScalingState value) {
	App::Get().Dispatcher().TryEnqueue([this, value]() {
		if (value == ScalingState::Scaling) {
			StopTimer();
		} else if (value == ScalingState::Idle) {
			// 缩放结束后源窗口位于前台则不要检查自动缩放，用户可能刚通过快捷键或
			// 工具栏终止缩放。_CheckForegroundTimer_Tick 也实现了类似功能，但它
			// 的触发频率较低，容易错过时机。
			if (GetForegroundWindow() == _hwndCurSrc) {
				_hwndChecked = _hwndCurSrc;
			}

			// 缩放结束后清空 _hwndCurSrc，等待状态下则保留
			_hwndCurSrc = NULL;
		}

		IsScalingChanged.Invoke(value == ScalingState::Scaling);
	});
}

ScalingRuntime& ScalingService::_GetOrCreateRuntime(HWND hWnd) noexcept {
	// 每个 runtime 都占有一条线程，源窗口销毁后应及时回收，否则线程数会
	// 随使用次数一直增长
	_RecycleIdleRuntimes();

	auto it = _runtimes.find(hWnd);
	if (it == _runtimes.end()) {
		auto rt = std::make_unique<ScalingRuntime>();
		rt->StateChanged(
			std::bind_front(&ScalingService::_ScalingRuntime_StateChanged, this));
		it = _runtimes.emplace(hWnd, std::move(rt)).first;
		Logger::Get().Info(fmt::format("为窗口 0x{:X} 创建缩放运行时（当前 {} 个）",
			(uintptr_t)hWnd, _runtimes.size()));
	}
	return *it->second;
}

void ScalingService::_RecycleIdleRuntimes() noexcept {
	// 析构 runtime 会等待缩放线程退出并泵送消息队列，可能被重入
	if (_recycling) {
		return;
	}
	_recycling = true;

	for (auto it = _runtimes.begin(); it != _runtimes.end();) {
		const HWND hwndSrc = it->first;
		if (IsWindow(hwndSrc) || it->second->State() != ScalingState::Idle) {
			++it;
			continue;
		}

		// 析构 runtime 会等待它的缩放线程退出，期间需要处理消息队列
		it = _runtimes.erase(it);
		Logger::Get().Info("已回收空闲的缩放运行时");
	}

	_recycling = false;
}

ScalingRuntime* ScalingService::_ActiveRuntime() noexcept {
	if (_hwndCurSrc) {
		auto it = _runtimes.find(_hwndCurSrc);
		if (it != _runtimes.end()) {
			return it->second.get();
		}
	}

	// 回退到任一正在缩放的运行时
	for (auto& [hwnd, rt] : _runtimes) {
		if (rt->State() == ScalingState::Scaling) {
			return rt.get();
		}
	}
	return nullptr;
}

void ScalingService::ToggleBackgroundRendering() noexcept {
	AppSettings& settings = AppSettings::Get();
	const bool newValue = !settings.IsBackgroundRendering();
	settings.IsBackgroundRendering(newValue);

	// 应用到所有正在缩放的窗口，由各缩放窗口自行提示
	bool anyScaling = false;
	for (auto& [hwnd, rt] : _runtimes) {
		if (rt->State() == ScalingState::Scaling) {
			anyScaling = true;
		}
		rt->SetBackgroundRendering(newValue);
	}

	if (!anyScaling) {
		LocalizationService& ls = LocalizationService::Get();
		ToastService::Get().ShowMessageInApp({}, ls.GetLocalizedString(
			newValue ? L"Message_BackgroundRenderingOn" : L"Message_BackgroundRenderingOff"));
	}
}

void ScalingService::_ScaleForegroundWindow(bool windowedMode, HWND hWnd) {
	if (!hWnd) {
		hWnd = GetForegroundWindow();
	}
	if (!hWnd) {
		return;
	}

	const Profile& profile = *ProfileService::Get().GetProfileForWindow(hWnd, false);
	_StartScale(hWnd, profile, windowedMode, false);
}

void ScalingService::_StartScale(HWND hWnd, const Profile& profile, bool windowedMode, bool force) {
	assert(hWnd);

	const ScalingError error = _StartScaleImpl(hWnd, profile, windowedMode, force);
	if (error != ScalingError::NoError) {
		ShowError(hWnd, error);
	}
}

ScalingError ScalingService::_StartScaleImpl(HWND hWnd, const Profile& profile, bool windowedMode, bool force) {
	// ScalingRuntime::Start 会检查是否正在缩放，这里提前检查以避免无效操作
	// 只判断"这个源窗口"自己的运行时，其它窗口的缩放不受影响
	auto runtimeIt = _runtimes.find(hWnd);
	if (!force && runtimeIt != _runtimes.end() &&
		runtimeIt->second->State() == ScalingState::Scaling) {
		return ScalingError::NoError;
	}

	// 限制同时缩放的窗口数（force 重启同一窗口时不把该窗口自己算在内）
	if (!force) {
		uint32_t scalingCount = 0;
		for (const auto& kv : _runtimes) {
			if (kv.first != hWnd && kv.second->State() == ScalingState::Scaling) {
				++scalingCount;
			}
		}

		const uint32_t maxScalingWindows = AppSettings::Get().MaxScalingWindows();
		if (scalingCount >= maxScalingWindows) {
			Logger::Get().Error(fmt::format("已达同时缩放窗口数上限 {}，拒绝缩放新窗口",
				maxScalingWindows));
			return ScalingError::TooManyScalingWindows;
		}
	}

	if (WindowHelper::IsForbiddenSystemWindow(hWnd)) {
		return ScalingError::NoError;
	}

	if (profile.scalingMode < 0) {
		return ScalingError::InvalidScalingMode;
	}

	const std::vector<EffectItem>& effects =
		ScalingModesService::Get().GetScalingMode(profile.scalingMode).effects;
	if (effects.empty()) {
		return ScalingError::InvalidScalingMode;
	} else {
		for (const EffectItem& effect : effects) {
			if (!EffectsService::Get().GetEffect(effect.name)) {
				// 存在无法解析的效果
				return ScalingError::InvalidScalingMode;
			}
		}
	}

	if (profile.Is3DGameMode() && windowedMode) {
		return ScalingError::Windowed3DGameMode;
	}

	ScalingOptions options;

	options.effects.reserve(effects.size());
	for (const EffectItem& effectItem : effects) {
		options.effects.push_back((EffectOption)effectItem);
	}

	// 尝试启用触控支持
	bool isTouchSupportEnabled;
	if (!TouchHelper::TryLaunchTouchHelper(isTouchSupportEnabled)) {
		Logger::Get().Error("TryLaunchTouchHelper 失败");
		return ScalingError::TouchSupport;
	}

	options.graphicsCardId = profile.graphicsCardId;
	options.captureMethod = profile.captureMethod;
	if (profile.isFrameRateLimiterEnabled) {
		options.maxFrameRate = profile.maxFrameRate;
	}
	options.multiMonitorUsage = profile.multiMonitorUsage;
	options.outputAlignment = profile.outputAlignment;
	options.cursorInterpolationMode = profile.cursorInterpolationMode;
	options.flags = profile.scalingFlags;

	options.IsWindowedMode(windowedMode);
	options.IsTouchSupportEnabled(isTouchSupportEnabled);

	switch (profile.initialWindowedScaleFactor) {
	case InitialWindowedScaleFactor::Auto:
		options.initialWindowedScaleFactor = 0.0f;
		break;
	case InitialWindowedScaleFactor::x1_25:
		options.initialWindowedScaleFactor = 1.25f;
		break;
	case InitialWindowedScaleFactor::x1_5:
		options.initialWindowedScaleFactor = 1.5f;
		break;
	case InitialWindowedScaleFactor::x1_75:
		options.initialWindowedScaleFactor = 1.75f;
		break;
	case InitialWindowedScaleFactor::x2:
		options.initialWindowedScaleFactor = 2.0f;
		break;
	case InitialWindowedScaleFactor::x3:
		options.initialWindowedScaleFactor = 3.0f;
		break;
	case InitialWindowedScaleFactor::Custom:
		options.initialWindowedScaleFactor = profile.customInitialWindowedScaleFactor;
		break;
	default:
		options.initialWindowedScaleFactor = 0.0f;
		break;
	}

	if (profile.isCroppingEnabled) {
		options.cropping = profile.cropping;
	}

	switch (profile.cursorScaling) {
	case CursorScaling::x0_5:
		options.cursorScaleFactor = 0.5f;
		break;
	case CursorScaling::x0_75:
		options.cursorScaleFactor = 0.75f;
		break;
	case CursorScaling::NoScaling:
		options.cursorScaleFactor = 1.0f;
		break;
	case CursorScaling::x1_25:
		options.cursorScaleFactor = 1.25f;
		break;
	case CursorScaling::x1_5:
		options.cursorScaleFactor = 1.5f;
		break;
	case CursorScaling::x2:
		options.cursorScaleFactor = 2.0f;
		break;
	case CursorScaling::Source:
		// 0 或负值表示和源窗口缩放比例相同
		options.cursorScaleFactor = 0.0f;
		break;
	case CursorScaling::Custom:
		options.cursorScaleFactor = profile.customCursorScaleFactor;
		break;
	default:
		options.cursorScaleFactor = 1.0f;
		break;
	}

	if (profile.isAutoHideCursorEnabled) {
		options.autoHideCursorDelay = profile.autoHideCursorDelay;
	}

	// 应用全局配置
	AppSettings& settings = AppSettings::Get();
	options.IsDeveloperMode(settings.IsDeveloperMode());
	options.IsDebugMode(settings.IsDebugMode());
	options.IsBenchmarkMode(settings.IsBenchmarkMode());
	options.IsTopmostDisabled(settings.IsTopmostDisabled());
	options.IsBackgroundRendering(settings.IsBackgroundRendering());
	options.IsEffectCacheDisabled(settings.IsEffectCacheDisabled());
	options.IsFontCacheDisabled(settings.IsFontCacheDisabled());
	options.IsSaveEffectSources(settings.IsSaveEffectSources());
	options.IsWarningsAreErrors(settings.IsWarningsAreErrors());
	options.IsAllowScalingMaximized(settings.IsAllowScalingMaximized());
	options.IsKeepScreenOn(settings.IsKeepScreenOn());
	options.IsSimulateExclusiveFullscreen(settings.IsSimulateExclusiveFullscreen());
	options.duplicateFrameDetectionMode = settings.DuplicateFrameDetectionMode();
	options.IsStatisticsForDynamicDetectionEnabled(settings.IsStatisticsForDynamicDetectionEnabled());
	options.IsInlineParams(settings.IsInlineParams());
	options.IsFP16Disabled(settings.IsFP16Disabled());

	if (options.maxFrameRate) {
		// 最小帧数不能大于最大帧数
		options.minFrameRate = std::min(settings.MinFrameRate(), *options.maxFrameRate);
	} else {
		options.minFrameRate = settings.MinFrameRate();
	}

	options.fullscreenInitialToolbarState = settings.FullscreenInitialToolbarState();
	options.windowedInitialToolbarState = settings.WindowedInitialToolbarState();
	options.screenshotsDir = settings.ScreenshotsDir();
	if (options.screenshotsDir.empty()) {
		// 回落到使用当前目录
		options.screenshotsDir = L".";
	}

	options.overlayOptions.windows = settings.OverlayWindowOptions();

	options.overlayOptions.scaleShortcut =
		settings.GetShortcut(ShortcutAction::Scale).ToString();
	options.overlayOptions.windowedModeScaleShortcut =
		settings.GetShortcut(ShortcutAction::WindowedModeScale).ToString();
	options.overlayOptions.takeScreenshotShortcut =
		settings.GetShortcut(ShortcutAction::TakeScreenshot).ToString();

	options.showToast = [](HWND hwndTarget, std::wstring_view msg) noexcept {
		ToastService::Get().ShowMessageOnWindow({}, msg, hwndTarget);
	};

	options.showError = &ShowError;

	options.save = [](const ScalingOptions& options, HWND /*hwndScaling*/) noexcept {
		App::Get().Dispatcher().TryEnqueue(
			[overlayOptions(options.overlayOptions)]() mutable {
				AppSettings::Get().OverlayWindowOptions() = std::move(overlayOptions.windows);
				AppSettings::Get().SaveAsync();
			}
		);
	};

	if (!_GetOrCreateRuntime(hWnd).Start(hWnd, std::move(options), force)) {
		return ScalingError::ScalingFailedGeneral;
	}

	_hwndCurSrc = hWnd;
	return ScalingError::NoError;
}

}
