#pragma once
#include "Event.h"
#include "ScalingOptions.h"
#include "ScalingRuntime.h"
#include "Singleton.h"
#include <memory>
#include <unordered_map>
#include <winrt/Magpie.h>

namespace Magpie {

struct Profile;

class ScalingService : public Singleton<ScalingService> {
	friend Singleton<ScalingService>;

public:
	~ScalingService();

	void Initialize();

	void Uninitialize() noexcept;

	void StartTimer(bool windowedMode);

	void StopTimer();

	bool IsTimerOn() const noexcept {
		return _curCountdownSeconds > 0;
	}

	bool IsTimerOn(bool windowedMode) const noexcept {
		return IsTimerOn() && windowedMode == _isCurCountdownWindowedMode;
	}

	double TimerProgress() const noexcept {
		return SecondsLeft() / _curCountdownSeconds;
	}

	double SecondsLeft() const noexcept;

	bool IsScaling() const noexcept;

	// 强制重新检查前台窗口
	void CheckForeground();

	Event<bool, bool> IsTimerOnChanged;
	Event<double> TimerTick;
	Event<bool> IsScalingChanged;

private:
	ScalingService() = default;

	void _ShortcutService_ShortcutPressed(winrt::Magpie::ShortcutAction action);

	void _CountDownTimer_Tick(winrt::DispatcherQueueTimer const&, winrt::IInspectable const&);

	void _CheckForegroundTimer_Tick(winrt::DispatcherQueueTimer const&, winrt::IInspectable const&);

	void _ScalingRuntime_StateChanged(ScalingState value);

	// hWnd 为 NULL 时取前台窗口
	void _ScaleForegroundWindow(bool windowedMode, HWND hWnd = NULL);

	void _StartScale(HWND hWnd, const Profile& profile, bool windowedMode, bool force);

	ScalingError _StartScaleImpl(HWND hWnd, const Profile& profile, bool windowedMode, bool force);

	// 每个被放大的源窗口对应一个 ScalingRuntime，从而支持多个窗口同时放大。
	// ScalingWindow 已去单例（见 ScalingWindow::ThreadInstance，实例绑定到各自
	// 的缩放线程），因此多个 runtime 可以并存；在窗口化模式下它们的缩放窗口
	// 就能并排摆放。（全屏模式下多个窗口会互相遮挡，没有并排意义。）
	std::unordered_map<HWND, std::unique_ptr<ScalingRuntime>> _runtimes;

	// 取（或按需创建）hWnd 对应的运行时
	ScalingRuntime& _GetOrCreateRuntime(HWND hWnd) noexcept;
	// 回收源窗口已销毁且处于空闲的运行时，避免线程数无限增长
	void _RecycleIdleRuntimes() noexcept;
	// 当前"活动"的运行时：最近启动的那个，回退到任一正在缩放的
	ScalingRuntime* _ActiveRuntime() noexcept;

	winrt::DispatcherQueueTimer _countDownTimer{ nullptr };
	winrt::DispatcherQueueTimer _checkForegroundTimer{ nullptr };

	Event<winrt::Magpie::ShortcutAction>::EventRevoker _shortcutActivatedRevoker;

	std::chrono::steady_clock::time_point _timerStartTimePoint;

	uint32_t _curCountdownSeconds = 0;
	bool _isCurCountdownWindowedMode = false;

	HWND _hwndCurSrc = NULL;
	// 1. 避免重复检查同一个窗口
	// 2. 用户使用热键退出全屏后暂时阻止该窗口自动放大
	HWND _hwndChecked = NULL;

	bool _recycling = false;
};

}
