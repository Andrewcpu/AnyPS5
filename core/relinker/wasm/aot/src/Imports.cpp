#include <wasm/WasmTarget.hpp>
#include <domain/ImportModule.hpp>
#include <domain/Types.hpp>
#include <nid/NidCompute.hpp>
#include <string>
#include <string_view>

namespace Relinker::Wasm {
namespace {

[[noreturn]] void Fail(const std::string& message) {
    throw Domain::RelinkerException(message);
}

std::uint16_t Read16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) Fail("wasm: truncated ELF");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t Read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) Fail("wasm: truncated ELF");
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
    return value;
}

std::uint64_t Read64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 8 > bytes.size()) Fail("wasm: truncated ELF");
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
    return value;
}

bool Fits(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

std::uint64_t FileOffset(const WasmImage& image, std::uint64_t address, std::uint64_t size) {
    for (const auto& segment : image.Segments) {
        if (segment.FileSize == 0 || address < segment.Virtual) continue;
        const auto delta = address - segment.Virtual;
        if (delta >= segment.FileSize || size > segment.FileSize - delta) continue;
        return segment.FileOffset + delta;
    }
    Fail("wasm: dynamic string table is outside a load segment");
    return 0;
}

std::string ReadName(const std::vector<std::uint8_t>& elf, std::uint64_t stringOffset, std::uint64_t stringSize, std::uint32_t name) {
    if (static_cast<std::uint64_t>(name) >= stringSize) Fail("wasm: import symbol name lies outside the string table");
    const auto start = stringOffset + name;
    std::string value;
    for (std::uint64_t index = start; index < stringOffset + stringSize; ++index) {
        if (elf[static_cast<std::size_t>(index)] == 0) return value;
        value.push_back(static_cast<char>(elf[static_cast<std::size_t>(index)]));
    }
    Fail("wasm: unterminated import name");
    return {};
}

bool Qualified(const std::string& name, std::string& key) {
    const auto first = name.find('#');
    if (first == std::string::npos) {
        key = name;
        return false;
    }
    const auto second = name.find('#', first + 1);
    if (second == std::string::npos || second + 1 == name.size()) Fail("wasm: invalid qualified import: " + name);
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::uint64_t id = 0;
    for (const char character : std::string_view(name).substr(second + 1)) {
        const auto digit = alphabet.find(character);
        if (digit == std::string_view::npos || id > (65535u - digit) / 64u) Fail("wasm: invalid import module ID: " + name);
        id = id * 64u + static_cast<std::uint64_t>(digit);
    }
    key = name.substr(0, first);
    return true;
}

struct Wrapper {
    const char* Name;
    WasmImportKind Kind;
};

constexpr Wrapper kWrappers[] = {
    {"memcpy", WasmImportKind::Memcpy},
    {"memset", WasmImportKind::Memset},
    {"strlen", WasmImportKind::Strlen},
    {"sceKernelGetProcessTime", WasmImportKind::ProcessTime},
    {"anyps5_webgpu_submit", WasmImportKind::Graphics},
};

std::uint16_t Lookup(const std::string& key, bool qualified, bool hashPlain) {
    if (key.empty()) Fail("wasm: undefined import");
    for (const auto& wrapper : kWrappers) {
        if (key == Nid::ComputeNid(wrapper.Name, "")) return static_cast<std::uint16_t>(wrapper.Kind);
    }
    if (qualified || !hashPlain) Fail("wasm: undefined import " + key);
    const auto nid = Nid::ComputeNid(key, "");
    for (const auto& wrapper : kWrappers) {
        if (nid == Nid::ComputeNid(wrapper.Name, "")) return static_cast<std::uint16_t>(wrapper.Kind);
    }
    Fail("wasm: undefined import " + key);
    return 0;
}

std::uint64_t EntryFor(WasmImage& image) {
    const auto entry = image.GuestLimit + static_cast<std::uint64_t>(image.Imports.size()) + 1;
    if (entry <= image.GuestLimit) Fail("wasm: import entry address wraps");
    return entry;
}

}

void PrepareWasmImports(const std::vector<std::uint8_t>& elf, WasmImage& image) {
    if (elf.size() < 64) return;
    const auto phoff = Read64(elf, 32);
    const auto phentsize = Read16(elf, 54);
    const auto phnum = Read16(elf, 56);
    if (phentsize != 56 || phnum == 0 || !Fits(phoff, static_cast<std::uint64_t>(phnum) * phentsize, elf.size())) return;
    bool saw = false;
    std::uint64_t strtab = 0;
    std::uint64_t strsz = 0;
    bool haveStrtab = false;
    bool haveStrsz = false;
    std::vector<std::uint64_t> needed;
    for (std::uint16_t index = 0; index < phnum; ++index) {
        const auto header = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(index) * phentsize;
        if (Read32(elf, header) != 2) continue;
        if (saw) Fail("wasm: multiple PT_DYNAMIC segments");
        saw = true;
        const auto offset = Read64(elf, header + 8);
        const auto size = Read64(elf, header + 32);
        if (size % 16 != 0 || !Fits(offset, size, elf.size())) Fail("wasm: dynamic segment is truncated");
        bool terminated = false;
        for (std::uint64_t cursor = 0; cursor < size; cursor += 16) {
            const auto tag = static_cast<std::int64_t>(Read64(elf, static_cast<std::size_t>(offset + cursor)));
            const auto value = Read64(elf, static_cast<std::size_t>(offset + cursor + 8));
            if (tag == 0) {
                terminated = true;
                break;
            }
            if (tag == 1) needed.push_back(value);
            else if (tag == 5) {
                strtab = value;
                haveStrtab = true;
            } else if (tag == 10) {
                strsz = value;
                haveStrsz = true;
            }
        }
        if (!terminated) Fail("wasm: dynamic segment is not terminated");
    }
    if (needed.empty()) return;
    if (!haveStrtab || !haveStrsz) Fail("wasm: dynamic string table is missing");
    const auto stringOffset = FileOffset(image, strtab, strsz == 0 ? 1 : strsz);
    if (strsz == 0 || !Fits(stringOffset, strsz, elf.size())) Fail("wasm: dynamic string table is missing");
    for (const auto offset : needed) {
        if (offset > 0xffffffffu) Fail("wasm: import symbol name lies outside the string table");
        const auto name = ReadName(elf, stringOffset, strsz, static_cast<std::uint32_t>(offset));
        if (Domain::ConsoleModuleSuffix(name)) image.HashPlainImports = true;
    }
}

void BindWasmSymbol(const std::vector<std::uint8_t>& elf, WasmImage& image, std::uint64_t symbolIndex, std::uint16_t section, std::uint8_t info, std::uint32_t name, std::uint64_t stringOffset, std::uint64_t stringSize) {
    if (section != 0 || (info >> 4) == 2) return;
    for (const auto& import : image.Imports) {
        if (import.Symbol == symbolIndex) return;
    }
    if (!Fits(stringOffset, stringSize == 0 ? 1 : stringSize, elf.size()) || stringSize == 0) Fail("wasm: import symbol name lies outside the string table");
    const auto text = ReadName(elf, stringOffset, stringSize, name);
    std::string key;
    const auto qualified = Qualified(text, key);
    const auto kind = Lookup(key, qualified, image.HashPlainImports);
    std::string nid = key;
    for (const auto& wrapper : kWrappers) {
        const auto computed = Nid::ComputeNid(wrapper.Name, "");
        if (kind == static_cast<std::uint16_t>(wrapper.Kind)) nid = computed;
    }
    WasmImport import;
    import.Entry = EntryFor(image);
    import.Symbol = symbolIndex;
    import.Kind = kind;
    import.Name = text;
    import.Nid = std::move(nid);
    image.Imports.push_back(std::move(import));
}

}
