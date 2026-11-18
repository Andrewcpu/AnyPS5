#ifndef WASM_AOT_DECODE_HPP
#define WASM_AOT_DECODE_HPP

#include <cstdint>
#include <cstddef>

namespace Relinker::Wasm {

enum class WasmMnemonic : std::uint8_t {
    Nop,
    Mov,
    MovImm,
    Lea,
    Add,
    Sub,
    Cmp,
    And,
    Or,
    Xor,
    Test,
    Inc,
    Dec,
    Push,
    PushFlags,
    Pop,
    Jmp,
    Jcc,
    Call,
    Ret,
    Halt,
    MovdToXmm,
    MovdFromXmm,
    MovqToXmm,
    MovqFromXmm,
    Pxor,
};

enum class WasmOperandKind : std::uint8_t { None, Reg, Imm, Mem };

struct WasmOperand {
    WasmOperandKind Kind = WasmOperandKind::None;
    bool Xmm = false;
    int Reg = -1;
    int Base = -1;
    int Index = -1;
    int Scale = 1;
    std::int64_t Disp = 0;
    bool Rip = false;
    std::uint64_t Imm = 0;
};

struct WasmInsn {
    WasmMnemonic Op = WasmMnemonic::Nop;
    std::uint8_t Width = 64;
    std::uint8_t Cc = 0;
    std::uint8_t Length = 0;
    std::uint64_t Address = 0;
    std::uint64_t Next = 0;
    std::uint64_t Target = 0;
    WasmOperand Dst;
    WasmOperand Src;
};

[[nodiscard]] WasmInsn DecodeWasmInsn(const std::uint8_t* data, std::size_t available, std::uint64_t address);

[[nodiscard]] bool WasmTerminates(WasmMnemonic op);

}
#endif
