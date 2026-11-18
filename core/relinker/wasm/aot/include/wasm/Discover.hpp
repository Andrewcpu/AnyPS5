#ifndef WASM_AOT_DISCOVER_HPP
#define WASM_AOT_DISCOVER_HPP

#include <wasm/Decode.hpp>
#include <wasm/WasmTarget.hpp>
#include <cstdint>
#include <vector>

namespace Relinker::Wasm {

struct WasmBlock {
    enum class Kind : std::uint8_t { Fall, Jump, Branch, Call, Return, Halt };

    std::uint64_t Start = 0;
    std::uint64_t Edge = 0;
    std::uint64_t Target = 0;
    std::uint64_t Fall = 0;
    std::uint8_t Cc = 0;
    Kind EdgeKind = Kind::Fall;
    std::vector<WasmInsn> Insns;
};

[[nodiscard]] std::vector<WasmBlock> DiscoverWasmBlocks(const WasmImage& image);

}
#endif
