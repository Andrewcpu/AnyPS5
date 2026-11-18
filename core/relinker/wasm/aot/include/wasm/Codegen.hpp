#ifndef WASM_AOT_CODEGEN_HPP
#define WASM_AOT_CODEGEN_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace Relinker::Wasm {

[[nodiscard]] std::vector<std::uint8_t> LinkWasmModule(const std::string& ir);

}
#endif
