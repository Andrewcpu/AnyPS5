#ifndef WASM_AOT_WASMTARGET_HPP
#define WASM_AOT_WASMTARGET_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace Relinker::Wasm {

inline constexpr std::uint64_t kWasmGuestBase = 0;
inline constexpr std::uint64_t kStateAddress = 0xF0000;
inline constexpr std::uint64_t kGprBytes = 16 * 8;
inline constexpr std::uint64_t kRipOffset = kGprBytes;
inline constexpr std::uint64_t kRflagsOffset = kRipOffset + 8;
inline constexpr std::uint64_t kXmmOffset = kRflagsOffset + 8;
inline constexpr std::uint64_t kStateBytes = kXmmOffset + 16 * 16;
inline constexpr std::uint64_t kGlobalBase = 0x100000;
inline constexpr std::uint64_t kMemoryBytes = 0x200000;
inline constexpr std::uint64_t kStackBytes = 0x10000;
inline constexpr std::uint64_t kInitialFlags = 0x202;
inline constexpr std::uint32_t kStepLimit = 1000000;
inline constexpr int kRsp = 4;

struct WasmSegment {
    std::uint64_t Virtual = 0;
    std::uint64_t MemorySize = 0;
    std::uint64_t FileSize = 0;
    std::uint64_t FileOffset = 0;
    std::uint64_t ContentSize = 0;
    std::uint32_t Flags = 0;
    std::vector<std::uint8_t> Bytes;
};

enum class WasmImportKind : std::uint16_t {
    Memcpy = 1,
    Memset = 2,
    Strlen = 3,
    ProcessTime = 4,
    Graphics = 5,
    Puts = 6,
};

struct WasmImport {
    std::uint64_t Entry = 0;
    std::uint64_t Symbol = 0;
    std::uint16_t Kind = 0;
    std::string Name;
    std::string Nid;
};

struct WasmImage {
    std::uint64_t Entry = 0;
    std::uint64_t GuestBase = 0;
    std::uint64_t StackBase = 0;
    std::uint64_t StackTop = 0;
    std::uint64_t GuestLimit = 0;
    bool HashPlainImports = false;
    std::vector<WasmSegment> Segments;
    std::vector<WasmImport> Imports;
    std::vector<std::uint64_t> CodeRoots;
};

[[nodiscard]] inline bool WasmImportsUse(const WasmImage& image, WasmImportKind kind) {
    const auto value = static_cast<std::uint16_t>(kind);
    for (const auto& import : image.Imports) if (import.Kind == value) return true;
    return false;
}

[[nodiscard]] WasmImage PlanWasmImage(const std::vector<std::uint8_t>& elf);
void PrepareWasmImports(const std::vector<std::uint8_t>& elf, WasmImage& image);
void BindWasmSymbol(const std::vector<std::uint8_t>& elf, WasmImage& image, std::uint64_t symbolIndex, std::uint16_t section, std::uint8_t info, std::uint32_t name, std::uint64_t stringOffset, std::uint64_t stringSize);
void ApplyWasmRelocations(const std::vector<std::uint8_t>& elf, WasmImage& image);
[[nodiscard]] std::vector<std::uint8_t> CompileWasm(const std::vector<std::uint8_t>& elf);

}
#endif
