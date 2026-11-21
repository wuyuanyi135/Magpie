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

	// 返回是否已进入捕获状态。多窗口下同一时刻只允许一个缩放窗口捕获光标，
	// 失败时通过 blockReason 给出让行原因（指向静态字符串，供诊断日志使用）
	bool _StartCapture(
		POINT& cursorPos,
		std::string_view reason = {},
		std::string_view* blockReason = nullptr
	) noexcept;

	bool _StopCapture(POINT& cursorPos, bool onDestroy = false, std::string_view reason = {}) noexcept;

	// 捕获期间缩放窗口点击穿透，鼠标消息会落到该点最上面的窗口。如果别的窗口盖住了源窗口，
	// 点击就会激活那个窗口（多客户端时表现为「一小块区域变成别的客户端」）。这里把源窗口
	// 抬到最前，除非覆盖它的是用户当前正在使用的窗口
	void _EnsureSourceNotCovered(POINT srcPos) noexcept;

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

	// _LogCaptureBlocked 的限流状态。按原因分别限流：让行、跳过裁剪区等不同原因会
	// 交替出现，共用一个时间戳会互相顶掉，导致每帧都记录
	static constexpr size_t BLOCK_LOG_SLOT_COUNT = 8;
	std::array<size_t, BLOCK_LOG_SLOT_COUNT> _blockLogReasonHashes{};
	std::array<std::chrono::steady_clock::time_point, BLOCK_LOG_SLOT_COUNT> _blockLogTimes{};

	// 诊断日志：_UpdateCursorState 的调用计数。渲染循环一旦暂停（例如切换前台窗口、
	// 画面收起到后台），这个方法就不再被调用，用调用间隔可以发现这种中断
	uint64_t _updateCount = 0;
	std::chrono::steady_clock::time_point _lastUpdateTime{};
	// 系统光标真实可见性的核对周期
	std::chrono::steady_clock::time_point _lastHeartbeatTime{};
	// 详细诊断日志（MAGPIE_LOG_LEVEL=debug）用：上一次记录时的关键状态
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
	// 是否已记录过「源窗口自己持有光标捕获，本窗口继续尝试捕获」
	bool _loggedSrcCaptureFallthrough = false;
	// 是否已记录过「前台窗口开始捕获光标」
	bool _loggedForegroundCaptureStart = false;
	// 回归哨兵：「绘制光标但未捕获」的起始时刻与是否已报警
	std::chrono::steady_clock::time_point _lostCaptureSince{};
	bool _loggedCursorLost = false;

	// 本实例是否正在贡献「隐藏系统光标」的期望。系统光标的可见性是进程全局的，
	// 由 CursorManager.cpp 中的 g_hideSystemCursorCount 统一维护
	bool _isHidingSystemCursor = false;

	// 登记在多窗口列表中的缩放窗口句柄（析构时用，避免此时再去问 ScalingWindow）
	HWND _hwndRegistered = NULL;

	static inline const HCURSOR _hDiagonalSize1Cursor = LoadCursor(NULL, IDC_SIZENWSE);
	static inline const HCURSOR _hDiagonalSize2Cursor = LoadCursor(NULL, IDC_SIZENESW);
	static inline const HCURSOR _hHorizontalSizeCursor = LoadCursor(NULL, IDC_SIZEWE);
	static inline const HCURSOR _hVerticalSizeCursor = LoadCursor(NULL, IDC_SIZENS);
};

// 检测上次异常退出导致系统光标速度未还原并修复。应在进程启动时调用一次。
// 声明为自由函数以便其他项目调用（Magpie 项目的包含目录不含本头文件）。
void RestoreCursorSpeedIfCrashed() noexcept;

}
