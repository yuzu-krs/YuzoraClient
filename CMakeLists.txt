cmake_minimum_required(VERSION 3.24)

project(YuzoraClient
    VERSION 0.1.0
    DESCRIPTION "YuzoraClient native DLL client"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global compile settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep DLL and EXE artifacts in the same directory so LoaderTest can always
# find YuzoraClient.dll next to its own executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    # High warning level for this project's code; system headers stay quiet.
    add_compile_options(/W4 /permissive- /external:anglebrackets /external:W0)
endif()

# ---------------------------------------------------------------------------
# YuzoraClient.dll
# ---------------------------------------------------------------------------

add_library(YuzoraClient SHARED
    src/dllmain.cpp
    src/core/Client.cpp
    src/core/Logger.cpp
    src/core/SelfTest.cpp
    src/memory/Memory.cpp
    src/memory/Scanner.cpp
    src/memory/Signature.cpp
    src/memory/SignatureManager.cpp
    src/version/GameVersion.cpp
    src/version/VersionManager.cpp
)

target_include_directories(YuzoraClient PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(YuzoraClient PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ---------------------------------------------------------------------------
# LoaderTest.exe
# ---------------------------------------------------------------------------

add_executable(LoaderTest
    tests/LoaderTest.cpp
)

target_compile_definitions(LoaderTest PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
