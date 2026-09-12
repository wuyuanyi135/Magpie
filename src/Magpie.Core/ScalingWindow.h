#pragma once
#include "BaseWindow.h"
#include "KeepScreenOnHelper.h"
#include "ScalingOptions.h"
#include "Singleton.h"
#include "SrcTracker.h"

namespace Magpie {

class CursorManager;

class ScalingWindow final : public BaseWindow<ScalingWindow> {
	using base_type = BaseWindow<ScalingWindow>;
	friend base_type;

public:
	// 多实例支持。
	//
	// ScalingWindow 不再继承 Singleton：单个进程需要同时存在多个缩放窗口，
	// 才能把多个放大的画面并排摆放。
	//
	// 既有代码里有上百处 ScalingWindow::Get()，为免一次性改动引入回归，这里
	// 保留同名接口，但语义改为「当前上下文关联的那个实例」（见 _current）。
	// 调用方在执行任何实例相关逻辑前应先用 SetCurrent 绑定，退出时恢复。
	static ScalingWindow& Get() noexcept;

	// 绑定/解绑「当前实例」。传 nullptr 表示解绑。
	// 返回上一个值，便于调用方恢复。
	static ScalingWindow* SetCurrent(ScalingWindow* value) noexcept;

	// 用于检查当前缩放是否结束（每个实例独立）
	uint32_t RunId() const noexcept {
		return _runId;
	}

	void SetDispatcher(const winrt::DispatcherQueue& value) noexcept {
		_dispatcherQueue = value;
	}

	// 进程级回退实例，供无法绑定的线程使用
	static void SetFallback(ScalingWindow* value) noexcept {
		_fallback.store(value, std::memory_order_release);
	}

	// 取得当前实例所属线程的 dispatcher 队列
	static const winrt::DispatcherQueue& Dispatcher() noexcept {
		return Get()._dispatcherQueue;
	}

	// 为任意专用线程（如 Renderer 的后端渲染线程）绑定实例。
	// 这类线程同样只服务一个缩放窗口，因此绑定后 ScalingWindow::Get()
	// 就能正确解析，无需改动调用点。
	class ThreadBinding {
	public:
		explicit ThreadBinding(ScalingWindow& window) noexcept
			: _prev(SetCurrent(&window)) {
		}

		~ThreadBinding() noexcept {
			SetCurrent(_prev);
		}

		ThreadBinding(const ThreadBinding&) = delete;
		ThreadBinding(ThreadBinding&&) = delete;

	private:
		ScalingWindow* _prev;
	};

	// RAII：为当前线程创建一个归本线程所有的缩放窗口，并绑定为「当前实例」；
	// 离开作用域时自动解绑并销毁。
	//
	// 多实例的支撑点：每个 ScalingRuntime 都有自己专用的 dispatcher 线程，
	// 且所有窗口操作都经 TryEnqueue 投递到该线程执行。因此把实例绑定到线程
	// 上，既有代码里上百处 ScalingWindow::Get() 无需任何改动就能拿到属于本
	// 线程的那个实例。
	class ThreadInstance {
	public:
		ThreadInstance() noexcept;
		~ThreadInstance() noexcept;

		ThreadInstance(const ThreadInstance&) = delete;
		ThreadInstance(ThreadInstance&&) = delete;

		ScalingWindow& Get() noexcept {
			return *_window;
		}

	private:
		std::unique_ptr<ScalingWindow> _window;
		ScalingWindow* _prev = nullptr;
	};

	void Start(HWND hwndSrc, ScalingOptions&& options) noexcept;

	void Stop() noexcept;

	void ToggleScaling(bool isWindowedMode) noexcept;

	void SwitchToolbarState() noexcept;

	void TakeScreenshot() noexcept;

	void Render() noexcept;

	const RECT& RendererRect() const noexcept {
		return _rendererRect;
	}

	const ScalingOptions& Options() const noexcept {
		return _options;
	}

	class SrcTracker& SrcTracker() noexcept {
		return _srcTracker;
	}

	const class SrcTracker& SrcTracker() const noexcept {
		return _srcTracker;
	}

	class Renderer& Renderer() noexcept {
		return *_renderer;
	}

	const class Renderer& Renderer() const noexcept {
		return *_renderer;
	}

	class CursorManager& CursorManager() noexcept {
		return *_cursorManager;
	}

	const class CursorManager& CursorManager() const noexcept {
		return *_cursorManager;
	}

	bool IsSrcRepositioning() const noexcept {
		return _isSrcRepositioning;
	}

	void RestartAfterSrcRepositioned() noexcept;

	void CleanAfterSrcRepositioned() noexcept;

	bool IsResizingOrMoving() const noexcept {
		return _isResizingOrMoving;
	}

	void ShowToast(std::wstring_view msg) const noexcept {
		_options.showToast(Handle(), msg);
	}

	void ShowError(ScalingError error) const noexcept {
		_options.showError(_srcTracker.Handle(), error);
	}

	// 由 ScalingRuntime 持有，因此构造与析构必须公开
	ScalingWindow() noexcept;
	~ScalingWindow() noexcept;

protected:
	LRESULT _MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

private:
	ScalingError _StartImpl(HWND hwndSrc) noexcept;

	// 确保渲染窗口长宽比不变，且限制最小和最大尺寸。必须提供 width 和 height 之一，另一个
	// 应为 0。如果 isRendererSize 为真，传入的 width 和 height 为渲染矩形尺寸，否则为缩
	// 放窗口尺寸。返回时 width 和 height 是新的缩放窗口尺寸。
	bool _CalcWindowedScalingWindowSize(int& width, int& height, bool isRendererSize, uint32_t dpi = 0) const noexcept;

	RECT _CalcWindowedRendererRect() const noexcept;

	ScalingError _CalcFullscreenRendererRect(uint32_t& monitorCount) noexcept;

	SIZE _AdjustFullscreenWindowSize(SIZE size, uint32_t dpi = 0) const noexcept;

	ScalingError _InitialMoveSrcWindowInFullscreen() noexcept;

	void _Show() noexcept;

	bool _UpdateSrcState(
		bool& isSrcRepositioning,
		bool& srcFocusedChanged
	) noexcept;

	bool _CheckForegroundFor3DGameMode(HWND hwndFore) const noexcept;

	void _SetWindowProps() const noexcept;

	void _UpdateWindowProps() const noexcept;

	void _UpdateTouchProps(const RECT& srcRect) const noexcept;

	void _RemoveWindowProps() const noexcept;

	static LRESULT CALLBACK _RendererWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void _ResizeRenderer() noexcept;

	void _MoveRenderer() noexcept;

	static LRESULT CALLBACK _BorderHelperWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void _CreateBorderHelperWindows() noexcept;

	void _RepostionBorderHelperWindows() noexcept;

	RECT _CalcSrcTouchRect() const noexcept;

	void _UpdateTouchHoleWindows(bool onInit) noexcept;

	void _UpdateFrameMargins() const noexcept;

	winrt::fire_and_forget _UpdateFocusStateAsync() const noexcept;

	bool _CalcTopmostState() const noexcept;

	bool _IsBorderless() const noexcept;

	void _UpdateRendererRect() noexcept;

	bool _EnsureCaptionVisibleOnScreen() noexcept;

	void _UpdateWindowRectFromWindowPos(const WINDOWPOS& windowPos) noexcept;

	void _DelayedStop(bool onSrcHung = false, bool onSrcRepositioning = false) const noexcept;

	// 未绑定线程上的回退实例：Magpie 里大量 ScalingWindow::Get() 调用发生在
	// 并非缩放线程的地方（线程池回调、第三方回调等），单例时代它们总能拿到
	// 窗口，去单例化后必须有一个进程级回退，否则就是解引用空指针（0xC0000005）。
	// 多窗口并存时回退只能给出其中一个实例，因此凡是能确定归属的线程都应通过
	// ThreadInstance / ThreadBinding 显式绑定。
	static inline std::atomic<ScalingWindow*> _fallback = nullptr;

	// 没有任何实例时的最后兜底，避免解引用空指针。它不代表一个真实窗口，
	// 走到这里说明调用时机有误。
	static ScalingWindow& _NullInstance() noexcept;

	// 「当前上下文实例」，见 Get()/SetCurrent()。每线程独立：缩放线程与后端
	// 渲染线程各自绑定自己的实例，未能绑定的线程走 _fallback。
	static inline thread_local ScalingWindow* _current = nullptr;

	winrt::DispatcherQueue _dispatcherQueue{ nullptr };

	uint32_t _runId = 0;

	RECT _windowRect{};
	RECT _rendererRect{};
	HWND _hwndRenderer = NULL;

	uint32_t _currentDpi = USER_DEFAULT_SCREEN_DPI;
	uint32_t _topBorderThicknessInClient = 0;
	// Win11 中“无边框”窗口的边框在客户区内
	uint32_t _nonTopBorderThicknessInClient = 0;

	ScalingOptions _options;
	std::unique_ptr<class Renderer> _renderer;
	std::unique_ptr<class CursorManager> _cursorManager;

	class SrcTracker _srcTracker;

	KeepScreenOnHelper::Guard _keepScreenOnGuard;
	wil::unique_mutex_nothrow _exclModeMutex;

	std::array<wil::unique_hwnd, 4> _hwndResizeHelpers{};
	std::array<wil::unique_hwnd, 4> _hwndTouchHoles{};

	ScalingError _runtimeError = ScalingError::NoError;

	// 窗口缩放时切换到全屏缩放或最小化前保存尺寸供以后恢复
	LONG _lastWindowedRendererWidth = 0;

	// 第一帧渲染完成后再显示
	bool _isFirstFrame = false;
	bool _isResizingOrMoving = false;
	// 用于区分调整大小和移动
	bool _isPreparingForResizing = false;
	bool _isMovingDueToSrcMoved = false;
	bool _shouldWaitForGpu = false;
	bool _areResizeHelperWindowsVisible = false;
	bool _isSrcRepositioning = false;
};

}
