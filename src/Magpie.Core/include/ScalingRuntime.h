#pragma once
#include "Event.h"

namespace Magpie {

enum class ScalingState {
	Idle,
	Scaling,
	Waiting
};

class ScalingRuntime {
public:
	ScalingRuntime();
	~ScalingRuntime();

	bool Start(HWND hwndSrc, struct ScalingOptions&& options, bool force);

	void ToggleScaling(bool isWindowedMode);

	void SwitchToolbarState();

	void TakeScreenshot();

	void SetBackgroundRendering(bool value);

	void Stop();

	ScalingState State() const noexcept {
		return _state.load(std::memory_order_relaxed);
	}

	// 当前缩放窗口的句柄，没有则为 NULL。供外部（如快捷键处理）把缩放窗口
	// 映射回源窗口。
	HWND ScalingWindowHandle() const noexcept {
		return _hwndScaling.load(std::memory_order_acquire);
	}

	// 调用者应处理线程同步
	MultithreadEvent<ScalingState> StateChanged;

private:
	void _ScalingThreadProc() noexcept;

	// 确保 _dispatcher 完成初始化
	const winrt::DispatcherQueue& _Dispatcher() noexcept;

	void _State(ScalingState value);

	std::thread _scalingThread;

	winrt::DispatcherQueue _dispatcher{ nullptr };
	std::atomic<bool> _dispatcherInitialized = false;
	// 只能在主线程访问，省下检查 _dispatcherInitialized 的开销
	bool _dispatcherInitializedCache = false;

	std::atomic<ScalingState> _state = ScalingState::Idle;
	// 由缩放线程维护
	std::atomic<HWND> _hwndScaling = NULL;
};

}
