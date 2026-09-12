#pragma once
#include "ImGuiBackend.h"
#include <mutex>
#include <parallel_hashmap/phmap.h>

namespace Magpie {

class DeviceResources;
struct OverlayWindowOption;

class ImGuiImpl {
public:
	ImGuiImpl() = default;
	ImGuiImpl(const ImGuiImpl&) = delete;
	ImGuiImpl(ImGuiImpl&&) = delete;

	~ImGuiImpl() noexcept;

	// ImGui 的「当前上下文」是进程内的全局变量（GImGui），而每个缩放窗口都有自己的
	// ImGuiContext 且运行在各自的线程上。因此所有 ImGui 调用必须串行化，并在进入时
	// 显式绑定本实例的上下文，否则多个窗口会互相踩踏彼此的上下文。
	// 返回值为 RAII 锁，调用方保持它的生命周期即可覆盖整段 ImGui 操作。
	// 锁是可重入的，因此嵌套调用是安全的。
	[[nodiscard]] std::unique_lock<std::recursive_mutex> LockContext() const noexcept;

	bool Initialize(DeviceResources& deviceResource) noexcept;

	bool BuildFonts() noexcept;

	void NewFrame(
		phmap::flat_hash_map<std::string, OverlayWindowOption>& windowOptions,
		float fittsLawAdjustment,
		float dpiScale
	) noexcept;

	void Draw(POINT drawOffset) noexcept;

	void ClearStates() noexcept;

	void MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

	std::optional<ImVec4> GetWindowRect(const char* id) const noexcept;

	const char* GetHoveredWindowId() const noexcept;

	// 将提示窗口限制在屏幕内
	void Tooltip(
		const char* content,
		float dpiScale,
		const char* description = nullptr,
		float maxWidth = -1.0f
	) noexcept;
private:
	void _UpdateMousePos(float fittsLawAdjustment) noexcept;

	ImGuiBackend _backend;

	// 本实例专属的 ImGui 上下文，见 LockContext()
	ImGuiContext* _ctx = nullptr;

	phmap::flat_hash_map<std::string, ImVec4> _windowRects;
};

}
