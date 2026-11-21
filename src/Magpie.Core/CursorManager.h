#pragma once

namespace Magpie {

class CursorManager {
public:
	CursorManager() = default;
	CursorManager(const CursorManager&) = delete;
	CursorManager(CursorManager&&) = delete;

	~CursorManager() noexcept;

	void Update() noexcept;

	void OnScalingPosChanged() noexcept;

	void OnSrcStartMove() noexcept;

	void OnSrcEndMove() noexcept;

	void OnStartMove() noexcept;

	void OnEndResizeMove() noexcept;

	void OnSrcRectChanged() noexcept;

	// 光标不在缩放窗口上或隐藏时为 NULL
	HCURSOR CursorHandle() const noexcept {
		return _hCursor;
	}

	// 屏幕坐标
	POINT CursorPos() const noexcept {
		return _cursorPos;
	}

	bool IsCursorCaptured() const noexcept {
		return _isUnderCapture;
	}

	bool IsCursorCapturedOnForeground() const noexcept {
		return _isCapturedOnForeground;
	}

	bool IsCursorOnOverlay() const noexcept {
		return _isOnOverlay;
	}
	void IsCursorOnOverlay(bool value) noexcept;

	bool IsCursorCapturedOnOverlay() const noexcept {
		return _isCapturedOnOverlay;
	}
	void IsCursorCapturedOnOverlay(bool value) noexcept;

	// 缩放画面收起到后台时调用：无条件退出捕获状态并恢复系统光标
	void OnPark() noexcept;

	int16_t SrcHitTest() const noexcept {
		return _lastCompletedHitTestResult;
	}

private:
	void _ShowSystemCursor(bool show, bool onDestory = false);

	void _AdjustCursorSpeed() noexcept;

	void _RestoreCursorSpeed() noexcept;

	void _ReliableSetCursorPos(POINT pos) noexcept;

	winrt::fire_and_forget _SrcHitTestAsync(POINT screenPos) noexcept;

	void _ClearHitTestResult() noexcept;

	void _UpdateCursorState() noexcept;

	void _ClipCursorForMonitors(POINT cursorPos) noexcept;

	void _ClipCursorOnSrcMoving() noexcept;

	void _UpdateCursorPos() noexcept;

	void _StartCapture(POINT& cursorPos, std::string_view reason = {}) noexcept;

	bool _StopCapture(POINT& cursorPos, bool onDestroy = false, std::string_view reason = {}) noexcept;

	void _SetClipCursor(const RECT& clipRect, bool is3DGameMode = false) noexcept;

	void _RestoreClipCursor() noexcept;

	// 输出一条完整的光标相关状态快照。诊断「光标不跟随 / 游戏收不到鼠标 / 输入被吞」时
	// 只需要这一行就能还原当时的状态机、窗口层级和输入归属
	void _LogStateSnapshot(std::string_view tag) noexcept;

	// 光标位于缩放窗口上却没有进入捕获时记录一次状态快照，用于定位「光标不跟随 / 游戏收不到鼠标」
	// 这类问题。相同原因会限流，避免刷屏。
	void _LogCaptureBlocked(std::string_view reason, HWND hwndCur) noexcept;

	HCURSOR _hCursor = NULL;
	POINT _cursorPos{ std::numeric_limits<LONG>::max() };

	// 用于确保拖拽源窗口和缩放窗口时光标位置稳定，使用相对于渲染矩形的局部坐标
	POINT _localCursorPosOnMoving{ std::numeric_limits<LONG>::max() };

	// 用于防止光标移动到边框的过程中闪烁
	std::chrono::steady_clock::time_point _sizeCursorStartTime{};

	RECT _lastClip{ std::numeric_limits<LONG>::max() };
	RECT _lastRealClip{ std::numeric_limits<LONG>::max() };

	int _originCursorSpeed = 0;

	uint32_t _nextHitTestId = 0;
	uint32_t _lastCompletedHitTestId = 0;
	POINT _lastCompletedHitTestPos{ std::numeric_limits<LONG>::max() };
	int16_t _lastCompletedHitTestResult = HTNOWHERE;

	bool _isUnderCapture = false;
	// 当缩放后的光标位置在交换链窗口上且没有被其他窗口挡住时应绘制光标
	bool _shouldDrawCursor = false;
	
	bool _isCapturedOnForeground = false;

	bool _isOnOverlay = false;
	bool _isCapturedOnOverlay = false;

	// _LogCaptureBlocked 的限流状态
	std::chrono::steady_clock::time_point _lastBlockLogTime{};
	size_t _lastBlockLogReasonHash = 0;

	// 诊断日志：_UpdateCursorState 的调用计数与心跳。渲染循环一旦暂停（例如切换前台窗口、
	// 画面收起到后台），这个方法就不再被调用，只能靠心跳的时间间隔发现
	uint64_t _updateCount = 0;
	std::chrono::steady_clock::time_point _lastUpdateTime{};
	std::chrono::steady_clock::time_point _lastHeartbeatTime{};
	// 状态变更日志用：上一次记录时的关键状态
	bool _hasLoggedState = false;
	bool _loggedUnderCapture = false;
	bool _loggedShouldDrawCursor = false;
	bool _loggedCapturedOnForeground = false;
	bool _loggedOnOverlay = false;
	bool _loggedCapturedOnOverlay = false;
	bool _loggedTransparent = false;
	bool _loggedTopmost = false;
	int16_t _loggedHitTest = HTNOWHERE;
	// 上一次观察到的系统光标真实可见性
	bool _loggedActualCursorVisible = false;

	// 本实例是否正在贡献「隐藏系统光标」的期望。系统光标的可见性是进程全局的，
	// 由 CursorManager.cpp 中的 g_hideSystemCursorCount 统一维护
	bool _isHidingSystemCursor = false;

	static inline const HCURSOR _hDiagonalSize1Cursor = LoadCursor(NULL, IDC_SIZENWSE);
	static inline const HCURSOR _hDiagonalSize2Cursor = LoadCursor(NULL, IDC_SIZENESW);
	static inline const HCURSOR _hHorizontalSizeCursor = LoadCursor(NULL, IDC_SIZEWE);
	static inline const HCURSOR _hVerticalSizeCursor = LoadCursor(NULL, IDC_SIZENS);
};

// 检测上次异常退出导致系统光标速度未还原并修复。应在进程启动时调用一次。
// 声明为自由函数以便其他项目调用（Magpie 项目的包含目录不含本头文件）。
void RestoreCursorSpeedIfCrashed() noexcept;

}
