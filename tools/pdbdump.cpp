// pdbdump: parses a PE file's CodeView debug entry and prints the PDB name,
// GUID (both raw memory-order and swapped), and age — for symbol server
// key validation.
// Usage: pdbdump <pe-file>

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("usage: pdbdump <pe-file>\n");
        return 1;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, argv[1], "rb") != 0 || file == nullptr) {
        std::printf("cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    std::fread(bytes.data(), 1, bytes.size(), file);
    fclose(file);

    const auto rd8 = [&](std::size_t off) {
        return *reinterpret_cast<const std::uint64_t*>(bytes.data() + off);
    };
    const auto rd16 = [&](std::size_t off) {
        return *reinterpret_cast<const std::uint16_t*>(bytes.data() + off);
    };
    const auto rd32 = [&](std::size_t off) {
        return *reinterpret_cast<const std::uint32_t*>(bytes.data() + off);
    };

    if (rd16(0) != IMAGE_DOS_SIGNATURE) {
        std::printf("not a PE (dos)\n");
        return 1;
    }
    const std::size_t lfanew = static_cast<std::size_t>(rd32(0x3C));
    if (rd32(lfanew) != IMAGE_NT_SIGNATURE) {
        std::printf("not a PE (nt)\n");
        return 1;
    }
    const std::uint16_t magic = rd16(lfanew + 24);
    const std::size_t ddOffset = (magic == 0x20B) ? 112 : 96;
    const std::size_t numSections = rd16(lfanew + 6);
    const std::size_t optSize = rd16(lfanew + 20);
    const std::size_t sectionStart = lfanew + 24 + optSize;

    const auto rvaToOffset = [&](std::uint32_t rva) -> std::size_t {
        for (std::size_t i = 0; i < numSections; ++i) {
            const std::size_t s = sectionStart + i * 40;
            const std::uint32_t va = rd32(s + 12);
            const std::uint32_t vSize = rd32(s + 8);
            const std::uint32_t rawPtr = rd32(s + 20);
            if (rva >= va && rva < va + vSize) {
                return rva - va + rawPtr;
            }
        }
        return rva;
    };

    const std::size_t ddEntryOffset = lfanew + 24 + ddOffset + 6 * 8;
    const std::uint32_t debugRva = rd32(ddEntryOffset);
    const std::uint32_t debugSize = rd32(ddEntryOffset + 4);
    if (debugSize == 0) {
        std::printf("no debug directory\n");
        return 1;
    }
    const std::size_t debugOff = rvaToOffset(debugRva);
    const std::size_t count = debugSize / 28;
    std::printf("debug entries: %zu\n", count);

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = debugOff + i * 28;
        const std::uint32_t type = rd32(e + 12);
        if (type != IMAGE_DEBUG_TYPE_CODEVIEW) {
            continue;
        }
        const std::size_t cvOff = rvaToOffset(rd32(e + 24));
        if (std::memcmp(bytes.data() + cvOff, "RSDS", 4) != 0) {
            std::printf("not RSDS\n");
            continue;
        }

        const std::uint8_t* guid = bytes.data() + cvOff + 4;
        const std::uint32_t age = rd32(cvOff + 20);
        const char* name =
            reinterpret_cast<const char*>(bytes.data() + cvOff + 24);

        auto hex8 = [](std::uint32_t v) {
            char buf[9] = {};
            std::snprintf(buf, sizeof(buf), "%08X", v);
            return std::string(buf);
        };
        const std::uint32_t d1 = *reinterpret_cast<const std::uint32_t*>(guid);
        const std::uint16_t d2 = *reinterpret_cast<const std::uint16_t*>(guid + 4);
        const std::uint16_t d3 = *reinterpret_cast<const std::uint16_t*>(guid + 6);

        // Key A: natural GUID string order (D1, D2, D3 big-endian display).
        std::string keyA = hex8(d1);
        auto hex16 = [](std::uint16_t v) {
            char buf[5] = {};
            std::snprintf(buf, sizeof(buf), "%04X", v);
            return std::string(buf);
        };
        keyA += hex16(d2);
        keyA += hex16(d3);
        for (int k = 0; k < 8; ++k) {
            char buf[3] = {};
            std::snprintf(buf, sizeof(buf), "%02X", guid[8 + k]);
            keyA += buf;
        }

        // Key B: raw memory order.
        std::string keyB;
        char buf[3] = {};
        for (int k = 0; k < 16; ++k) {
            std::snprintf(buf, sizeof(buf), "%02X", guid[k]);
            keyB += buf;
        }

        std::printf("name: %s\nage: %u\n", name, age);
        std::printf("keyA (guid-string order): %s%u\n", keyA.c_str(), age);
        std::printf("keyB (raw memory order): %s%u\n", keyB.c_str(), age);
        std::printf("url A: https://msdl.microsoft.com/download/symbols/%s/%s%u/%s\n",
                    name, keyA.c_str(), age, name);
        std::printf("url B: https://msdl.microsoft.com/download/symbols/%s/%s%u/%s\n",
                    name, keyB.c_str(), age, name);
    }
    return 0;
}
