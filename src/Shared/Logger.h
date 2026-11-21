#pragma once
#include "Singleton.h"
#if defined(_DEBUG) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include <spdlog/spdlog.h>
#if defined(_DEBUG) && defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <fmt/printf.h>

namespace Magpie {

// std::source_location 中的函数名包含整个签名过于冗长，我们只需记录函数名，
// 因此创建自己的 SourceLocation
struct SourceLocation {
	[[nodiscard]] static consteval SourceLocation Current(
		std::uint_least32_t line = __builtin_LINE(),
		const char* file = __builtin_FILE(),
		const char* function = __builtin_FUNCTION()
	) noexcept {
		return SourceLocation{ line, file, function };
	}

	[[nodiscard]] constexpr SourceLocation() noexcept = default;

	[[nodiscard]] constexpr SourceLocation(
		const std::uint_least32_t line,
		const char* file,
		const char* function
	) noexcept : _line(line), _file(file), _function(function) {}

	[[nodiscard]] constexpr std::uint_least32_t Line() const noexcept {
		return _line;
	}

	[[nodiscard]] constexpr const char* FileName() const noexcept {
		return _file;
	}

	constexpr const char* FunctionName() const noexcept {
		return _function;
	}

private:
	const std::uint_least32_t _line = 0;
	const char* _file = nullptr;
	const char* _function = nullptr;
};

class Logger : public Singleton<Logger> {
	friend Singleton<Logger>;

public:
	bool Initialize(spdlog::level::level_enum logLevel, std::wstring logFileName, int logArchiveAboveSize, int logMaxArchiveFiles) noexcept;

	void SetLevel(spdlog::level::level_enum logLevel) noexcept;

	// 详细诊断日志（每帧状态快照、层级变更细节等）用来排查难以复现的问题，默认级别
	// 下应当关闭。格式化参数是即时求值的，因此调用方先用 IsEnabled 判断再构造消息。
	bool IsEnabled(spdlog::level::level_enum logLevel) const noexcept {
		return _logger && _logger->should_log(logLevel);
	}

	void Debug(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::debug, msg, location);
	}

	void Flush() noexcept {
		if (_logger) {
			_logger->flush();
		}
	}

	void Info(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::info, msg, location);
	}

	void Win32Info(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::info, _MakeWin32ErrorMsg(msg), location);
	}

	void NTInfo(std::string_view msg, NTSTATUS status, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::info, _MakeNTErrorMsg(msg, status), location);
	}

	void ComInfo(std::string_view msg, HRESULT hr, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::info, _MakeComErrorMsg(msg, hr), location);
	}

	void Warn(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::warn, msg, location);
	}

	void Win32Warn(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::warn, _MakeWin32ErrorMsg(msg), location);
	}

	void NTWarn(std::string_view msg, NTSTATUS status, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::warn, _MakeNTErrorMsg(msg, status), location);
	}

	void ComWarn(std::string_view msg, HRESULT hr, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::warn, _MakeComErrorMsg(msg, hr), location);
	}

	void Error(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::err, msg, location);
	}

	void Win32Error(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::err, _MakeWin32ErrorMsg(msg), location);
	}

	void NTError(std::string_view msg, NTSTATUS status, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::err, _MakeNTErrorMsg(msg, status), location);
	}

	void ComError(std::string_view msg, HRESULT hr, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::err, _MakeComErrorMsg(msg, hr), location);
	}

	void Critical(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::critical, msg, location);
	}

	void Win32Critical(std::string_view msg, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::critical, _MakeWin32ErrorMsg(msg), location);
	}

	void NTCritical(std::string_view msg, NTSTATUS status, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::critical, _MakeNTErrorMsg(msg, status), location);
	}

	void ComCritical(std::string_view msg, HRESULT hr, const SourceLocation& location = SourceLocation::Current()) noexcept {
		_Log(spdlog::level::critical, _MakeComErrorMsg(msg, hr), location);
	}

private:
	Logger() = default;

	static std::string _MakeWin32ErrorMsg(std::string_view msg) noexcept {
		return fmt::format("{}\n\tLastErrorCode: {}", msg, GetLastError());
	}

	static std::string _MakeNTErrorMsg(std::string_view msg, NTSTATUS status) noexcept {
		return fmt::format("{}\n\tNTSTATUS: {}", msg, status);
	}

	static std::string _MakeComErrorMsg(std::string_view msg, HRESULT hr) noexcept {
		return fmt::sprintf("%s\n\tHRESULT: 0x%X", msg, hr);
	}

	void _Log(spdlog::level::level_enum logLevel, std::string_view msg, const SourceLocation& location) noexcept;

	std::shared_ptr<spdlog::logger> _logger;
};

}
