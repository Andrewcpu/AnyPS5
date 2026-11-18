#ifndef WASM_AOT_EMIT_HPP
#define WASM_AOT_EMIT_HPP

#include <wasm/Discover.hpp>
#include <string>

namespace Relinker::Wasm {

[[nodiscard]] std::string EmitWasmModule(const WasmImage& image, const std::vector<WasmBlock>& blocks);
[[nodiscard]] std::string EmitWasmImports(const WasmImage& image);
[[nodiscard]] bool WasmUsesHost(const WasmImage& image);
[[nodiscard]] int WasmHostGroup(const WasmImage& image);
[[nodiscard]] std::string WasmHostDeclare(int group);

}
#endif
