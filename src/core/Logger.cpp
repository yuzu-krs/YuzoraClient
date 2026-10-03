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

// Diagnostics file sink: the game process runs inside an AppContainer where
// neither stdout nor a debugger may be available, so every line is also
// appended to the first writable candidate path. The package LocalState
// directory is guaranteed writable for a UWP host.
FILE* openLogFile() {
    const char* candidates[] = {
        "C:\\Users\\yuzut\\AppData\\Local\\Packages\\"
        "Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\yuzora.log",
        "yuzora-client.log",
    };
    for (const char* path : candidates) {
        FILE* file = nullptr;
        if (fopen_s(&file, path, "ab") == 0 && file != nullptr) {
            return file;
        }
    }
    return nullptr;
}

FILE* logFile() {
    static FILE* file = openLogFile();
    return file;
}

}  // namespace

void Logger::log(LogLevel level, std::string_view message) {
    const std::string line =
        std::format("[{}] {}{}\n", kTag, levelPrefix(level), message);

    {
        const std::scoped_lock lock{logMutex()};
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
        if (FILE* file = logFile()) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fflush(file);
        }
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
