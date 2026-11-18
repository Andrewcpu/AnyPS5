#ifndef WASM_AOT_WASMTARGET_HPP
#define WASM_AOT_WASMTARGET_HPP

#include <cstdint>
#include <vector>

namespace Relinker::Wasm {

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
    std::uint32_t Flags = 0;
    std::vector<std::uint8_t> Bytes;
};

struct WasmImage {
    std::uint64_t Entry = 0;
    std::uint64_t GuestBase = 0;
    std::uint64_t StackBase = 0;
    std::uint64_t StackTop = 0;
    std::uint64_t GuestLimit = 0;
    std::vector<WasmSegment> Segments;
};

[[nodiscard]] WasmImage PlanWasmImage(const std::vector<std::uint8_t>& elf);
[[nodiscard]] std::vector<std::uint8_t> CompileWasm(const std::vector<std::uint8_t>& elf);

}
#endif
