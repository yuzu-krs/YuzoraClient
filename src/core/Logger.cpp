#include "Logger.hpp"

#include <Windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace yuzora {

namespace {

// Serializes log writes so concurrent lines never interleave.
std::mutex& logMutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

void Logger::log(LogLevel level, std::string_view message) {
    const std::string line =
        std::format("[{}] {}{}\n", kTag, levelPrefix(level), message);

    {
        const std::scoped_lock lock{logMutex()};
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }

    // Also surface the line in any attached debugger.
    ::OutputDebugStringA(line.c_str());
}

const char* Logger::levelPrefix(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Warning:
            return "[warning] ";
        case LogLevel::Error:
            return "[error] ";
        case LogLevel::Info:
        default:
            break;
    }
    return "";
}

}  // namespace yuzora
