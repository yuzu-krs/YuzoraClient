// LoaderTest.exe - loads YuzoraClient.dll exactly the way a host process
// would and drives the exported lifecycle API end to end:
//
//   LoadLibraryW -> YuzoraInitialize -> YuzoraShutdown -> FreeLibrary
//
// Exit code is 0 only when every step succeeded.

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

// Signature shared by the exported YuzoraInitialize / YuzoraShutdown.
using YuzoraEntry = bool (*)();

// Converts UTF-16 text to UTF-8 so console output does not depend on the
// console's active codepage.
std::string toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string converted(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        converted.data(), size, nullptr, nullptr);
    return converted;
}

// Formats a Windows error code as a readable message (localized, converted
// to UTF-8, trailing whitespace trimmed).
std::string formatWindowsError(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring_view wide;
    if (length > 0 && buffer != nullptr) {
        wide = std::wstring_view{buffer, length};
        while (!wide.empty() &&
               (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
            wide.remove_suffix(1);
        }
    }

    const std::string message = toUtf8(wide);

    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    return message.empty() ? "unknown error" : message;
}

// Reports "<action> failed (Windows error <code>): <message>".
// GetLastError() is captured first so no intermediate call can clobber it.
void reportWindowsError(const char* action) {
    const DWORD error = GetLastError();
    std::cout << "[LoaderTest] " << action << " failed (Windows error " << error
              << "): " << formatWindowsError(error) << "\n";
}

// RAII switch of the console output codepage to UTF-8 for the lifetime of
// the program, so paths and error text render identically on every console.
// Harmless when no console is attached (e.g. redirected output).
class ConsoleUtf8 {
public:
    ConsoleUtf8() : previous_{::GetConsoleOutputCP()} { ::SetConsoleOutputCP(CP_UTF8); }
    ~ConsoleUtf8() { ::SetConsoleOutputCP(previous_); }

    ConsoleUtf8(const ConsoleUtf8&) = delete;
    ConsoleUtf8& operator=(const ConsoleUtf8&) = delete;

private:
    UINT previous_;
};

// Directory containing this executable. The CMake setup places
// YuzoraClient.dll in the same directory.
std::filesystem::path executableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            reportWindowsError("GetModuleFileNameW");
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        // Buffer too small (or exactly filled): grow it and retry.
        buffer.resize(buffer.size() * 2);
    }

    return std::filesystem::path{buffer}.parent_path();
}

}  // namespace

int main() {
    const ConsoleUtf8 consoleUtf8;

    // Flush after every line so LoaderTest output and the DLL's per-line
    // flushed log output keep their chronological order on the console.
    std::cout << std::unitbuf;

    const std::filesystem::path directory = executableDirectory();
    if (directory.empty()) {
        std::cout << "[LoaderTest] Cannot determine the executable directory\n";
        return 1;
    }

    const std::filesystem::path dllPath = directory / L"YuzoraClient.dll";

    std::cout << "[LoaderTest] Loading " << toUtf8(dllPath.native()) << "\n";

    const HMODULE module = LoadLibraryW(dllPath.c_str());
    if (module == nullptr) {
        reportWindowsError("LoadLibraryW");
        return 1;
    }

    const auto initialize =
        reinterpret_cast<YuzoraEntry>(GetProcAddress(module, "YuzoraInitialize"));
    if (initialize == nullptr) {
        reportWindowsError("GetProcAddress(YuzoraInitialize)");
        FreeLibrary(module);
        return 1;
    }

    const auto shutdown =
        reinterpret_cast<YuzoraEntry>(GetProcAddress(module, "YuzoraShutdown"));
    if (shutdown == nullptr) {
        reportWindowsError("GetProcAddress(YuzoraShutdown)");
        FreeLibrary(module);
        return 1;
    }

    if (!initialize()) {
        std::cout << "[LoaderTest] YuzoraInitialize failed\n";
        FreeLibrary(module);
        return 1;
    }

    std::cout << "YuzoraClient loaded successfully\n";

    if (!shutdown()) {
        std::cout << "[LoaderTest] YuzoraShutdown failed\n";
        FreeLibrary(module);
        return 1;
    }

    if (!FreeLibrary(module)) {
        reportWindowsError("FreeLibrary");
        return 1;
    }

    std::cout << "[LoaderTest] Unloaded YuzoraClient.dll\n";
    return 0;
}
