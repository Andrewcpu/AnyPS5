#include <wasm/Codegen.hpp>
#include <wasm/Emit.hpp>
#include <wasm/WasmTarget.hpp>

namespace Relinker::Wasm {

WasmImage PlanWasmImage(const std::vector<std::uint8_t>& elf);

std::vector<std::uint8_t> CompileWasm(const std::vector<std::uint8_t>& elf) {
    const auto image = PlanWasmImage(elf);
    const auto blocks = DiscoverWasmBlocks(image);
    return LinkWasmModule(EmitWasmModule(image, blocks));
}

}
