#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace yuzora {

enum class LogLevel {
    Info,
    Warning,
    Error,
};

// Minimal process-wide logger for the v0.1 foundation.
//
// Each line is written to both the host process stdout (visible in console
// loaders such as LoaderTest) and the debugger channel via
// OutputDebugStringA (visible when a debugger is attached to the host).
// Lines are flushed immediately so their order is never scrambled.
class Logger {
public:
    Logger() = delete;

    template <typename... Args>
    static void info(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void warning(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void error(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
    }

    // Tag prefixed to every log line: "[YuzoraClient] ...".
    static constexpr std::string_view kTag{"YuzoraClient"};

private:
    static void log(LogLevel level, std::string_view message);
    static const char* levelPrefix(LogLevel level) noexcept;
};

}  // namespace yuzora
