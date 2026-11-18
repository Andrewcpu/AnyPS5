#include <wasm/Decode.hpp>
#include <domain/Types.hpp>
#include <string>

namespace Relinker::Wasm {
namespace {

[[noreturn]] void Fail(std::uint64_t address, const std::string& message) {
    throw Domain::RelinkerException("wasm lift: " + message + " at 0x" + ([&] {
        std::string hex;
        const char* digits = "0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4) hex.push_back(digits[(address >> shift) & 0xf]);
        auto first = hex.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : hex.substr(first);
    })());
}

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t available, std::uint64_t address)
        : data(data), available(available), address(address) {}

    std::uint8_t Take() {
        if (cursor >= available || cursor >= 15) Fail(address, "instruction is truncated or longer than 15 bytes");
        return data[cursor++];
    }

    std::uint8_t Peek() const {
        if (cursor >= available) Fail(address, "instruction is truncated");
        return data[cursor];
    }

    std::uint32_t Take32() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(Take()) << (8 * index);
        return value;
    }

    std::uint64_t Take64() {
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(Take()) << (8 * index);
        return value;
    }

    std::uint64_t Address() const { return address; }
    std::uint64_t Next() const { return address + cursor; }
    std::uint8_t Length() const { return static_cast<std::uint8_t>(cursor); }

private:
    const std::uint8_t* data;
    std::size_t available;
    std::uint64_t address;
    std::size_t cursor = 0;
};

struct Rex {
    bool Present = false;
    bool W = false;
    bool R = false;
    bool X = false;
    bool B = false;
};

struct Modrm {
    int Extension = 0;
    int Reg = 0;
    bool Memory = false;
    bool Rip = false;
    int Base = -1;
    int Index = -1;
    int Scale = 1;
    std::int64_t Disp = 0;
};

int Extend(int field, bool high) { return field + (high ? 8 : 0); }

std::int64_t Sign32(std::uint32_t value) { return static_cast<std::int32_t>(value); }

Modrm ReadModrm(Reader& reader, const Rex& rex) {
    const auto byte = reader.Take();
    const int mod = byte >> 6;
    const int reg = (byte >> 3) & 7;
    const int rm = byte & 7;
    Modrm out;
    out.Extension = reg;
    out.Reg = Extend(reg, rex.R);
    if (mod == 3) {
        out.Base = Extend(rm, rex.B);
        return out;
    }
    out.Memory = true;
    int baseField = rm;
    bool sib = rm == 4;
    if (sib) {
        const auto sibByte = reader.Take();
        const int scale = sibByte >> 6;
        const int index = (sibByte >> 3) & 7;
        baseField = sibByte & 7;
        out.Scale = 1 << scale;
        if (!(index == 4 && !rex.X)) out.Index = Extend(index, rex.X);
    }
    if (mod == 0 && baseField == 5) {
        out.Disp = Sign32(reader.Take32());
        if (!sib) out.Rip = true;
        return out;
    }
    out.Base = Extend(baseField, rex.B);
    if (mod == 1) out.Disp = static_cast<std::int8_t>(reader.Take());
    else if (mod == 2) out.Disp = Sign32(reader.Take32());
    return out;
}

WasmOperand Register(int reg) {
    WasmOperand operand;
    operand.Kind = WasmOperandKind::Reg;
    operand.Reg = reg;
    return operand;
}

WasmOperand Xmm(int reg) {
    auto operand = Register(reg);
    operand.Xmm = true;
    return operand;
}

WasmOperand Immediate(std::uint64_t value) {
    WasmOperand operand;
    operand.Kind = WasmOperandKind::Imm;
    operand.Imm = value;
    return operand;
}

WasmOperand Memory(const Modrm& modrm) {
    WasmOperand operand;
    operand.Kind = WasmOperandKind::Mem;
    operand.Base = modrm.Base;
    operand.Index = modrm.Index;
    operand.Scale = modrm.Scale;
    operand.Disp = modrm.Disp;
    operand.Rip = modrm.Rip;
    return operand;
}

WasmOperand Rm(const Modrm& modrm) {
    if (!modrm.Memory) return Register(modrm.Base);
    return Memory(modrm);
}

std::uint64_t Relative(Reader& reader, std::int64_t displacement) {
    return reader.Next() + static_cast<std::uint64_t>(displacement);
}

WasmInsn Finish(Reader& reader, WasmMnemonic op, std::uint8_t width, WasmOperand dst, WasmOperand src, std::uint8_t cc = 0, std::uint64_t target = 0) {
    WasmInsn insn;
    insn.Op = op;
    insn.Width = width;
    insn.Cc = cc;
    insn.Length = reader.Length();
    insn.Address = reader.Address();
    insn.Next = reader.Next();
    insn.Target = target;
    insn.Dst = dst;
    insn.Src = src;
    return insn;
}

WasmMnemonic AluMnemonic(int group) {
    switch (group) {
    case 0: return WasmMnemonic::Add;
    case 1: return WasmMnemonic::Or;
    case 4: return WasmMnemonic::And;
    case 5: return WasmMnemonic::Sub;
    case 6: return WasmMnemonic::Xor;
    case 7: return WasmMnemonic::Cmp;
    default: return WasmMnemonic::Nop;
    }
}

WasmInsn DecodeAlu(Reader& reader, int opcode, const Rex& rex) {
    const int group = opcode >> 3;
    const int form = opcode & 7;
    if (group > 7 || (group != 0 && group != 1 && group != 4 && group != 5 && group != 6 && group != 7)) Fail(reader.Address(), "unsupported arithmetic opcode");
    const auto mnemonic = AluMnemonic(group);
    const std::uint8_t width = rex.W ? 64 : 32;
    if (form == 1 || form == 3) {
        const auto modrm = ReadModrm(reader, rex);
        const auto reg = Register(modrm.Reg);
        const auto rm = Rm(modrm);
        if (form == 1) return Finish(reader, mnemonic, width, rm, reg);
        return Finish(reader, mnemonic, width, reg, rm);
    }
    if (form == 5) {
        std::uint64_t immediate = reader.Take32();
        if (rex.W) immediate = static_cast<std::uint64_t>(Sign32(static_cast<std::uint32_t>(immediate)));
        return Finish(reader, mnemonic, width, Register(0), Immediate(immediate));
    }
    Fail(reader.Address(), "unsupported arithmetic form");
    return {};
}

WasmInsn DecodeGroup1(Reader& reader, int opcode, const Rex& rex) {
    const auto modrm = ReadModrm(reader, rex);
    if (modrm.Extension == 2 || modrm.Extension == 3) Fail(reader.Address(), "ADC and SBB are not lifted");
    const auto mnemonic = AluMnemonic(modrm.Extension);
    if (mnemonic == WasmMnemonic::Nop && modrm.Extension != 0) Fail(reader.Address(), "unsupported group 1 opcode");
    const std::uint8_t width = rex.W ? 64 : 32;
    std::uint64_t immediate = 0;
    if (opcode == 0x81) {
        const auto raw = reader.Take32();
        immediate = rex.W ? static_cast<std::uint64_t>(Sign32(raw)) : raw;
    } else {
        immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(reader.Take())));
        if (!rex.W) immediate &= 0xffffffffu;
    }
    return Finish(reader, mnemonic, width, Rm(modrm), Immediate(immediate));
}

WasmInsn DecodeSse(Reader& reader, const Rex& rex) {
    const auto opcode = reader.Take();
    const auto modrm = ReadModrm(reader, rex);
    if (opcode == 0xef) {
        WasmOperand src = modrm.Memory ? Memory(modrm) : Xmm(modrm.Base);
        src.Xmm = !modrm.Memory;
        return Finish(reader, WasmMnemonic::Pxor, 128, Xmm(modrm.Reg), src);
    }
    if (opcode == 0x6e) {
        if (modrm.Memory) {
            auto src = Memory(modrm);
            return Finish(reader, rex.W ? WasmMnemonic::MovqToXmm : WasmMnemonic::MovdToXmm, rex.W ? 64 : 32, Xmm(modrm.Reg), src);
        }
        return Finish(reader, rex.W ? WasmMnemonic::MovqToXmm : WasmMnemonic::MovdToXmm, rex.W ? 64 : 32, Xmm(modrm.Reg), Register(modrm.Base));
    }
    if (opcode == 0x7e) {
        if (modrm.Memory) {
            auto dst = Memory(modrm);
            return Finish(reader, rex.W ? WasmMnemonic::MovqFromXmm : WasmMnemonic::MovdFromXmm, rex.W ? 64 : 32, dst, Xmm(modrm.Reg));
        }
        return Finish(reader, rex.W ? WasmMnemonic::MovqFromXmm : WasmMnemonic::MovdFromXmm, rex.W ? 64 : 32, Register(modrm.Base), Xmm(modrm.Reg));
    }
    Fail(reader.Address(), "unsupported SSE opcode");
    return {};
}

WasmInsn DecodeTwoByte(Reader& reader, const Rex& rex, bool operand16) {
    if (operand16) return DecodeSse(reader, rex);
    const auto opcode = reader.Take();
    if (opcode == 0x0b || opcode == 0x05) return Finish(reader, WasmMnemonic::Halt, 64, {}, {});
    if (opcode >= 0x80 && opcode <= 0x8f) {
        const auto target = Relative(reader, Sign32(reader.Take32()));
        return Finish(reader, WasmMnemonic::Jcc, 64, {}, {}, static_cast<std::uint8_t>(opcode & 0xf), target);
    }
    Fail(reader.Address(), "unsupported two-byte opcode");
    return {};
}

WasmInsn DecodePrimary(Reader& reader, int opcode, const Rex& rex) {
    const std::uint8_t width = rex.W ? 64 : 32;
    if (opcode == 0x90 && !rex.Present) return Finish(reader, WasmMnemonic::Nop, 64, {}, {});
    if (opcode == 0xC3 && !rex.Present) return Finish(reader, WasmMnemonic::Ret, 64, {}, {});
    if (opcode == 0xF4 || opcode == 0xCC) return Finish(reader, WasmMnemonic::Halt, 64, {}, {});
    if (opcode >= 0x50 && opcode <= 0x57) return Finish(reader, WasmMnemonic::Push, 64, {}, Register(Extend(opcode & 7, rex.B)));
    if (opcode >= 0x58 && opcode <= 0x5f) return Finish(reader, WasmMnemonic::Pop, 64, Register(Extend(opcode & 7, rex.B)), {});
    if (opcode == 0x9c) return Finish(reader, WasmMnemonic::PushFlags, 64, {}, {});
    if (opcode >= 0x70 && opcode <= 0x7f) {
        const auto target = Relative(reader, static_cast<std::int8_t>(reader.Take()));
        return Finish(reader, WasmMnemonic::Jcc, 64, {}, {}, static_cast<std::uint8_t>(opcode & 0xf), target);
    }
    if (opcode == 0xeb) {
        const auto target = Relative(reader, static_cast<std::int8_t>(reader.Take()));
        return Finish(reader, WasmMnemonic::Jmp, 64, {}, {}, 0, target);
    }
    if (opcode == 0xe9) {
        const auto target = Relative(reader, Sign32(reader.Take32()));
        return Finish(reader, WasmMnemonic::Jmp, 64, {}, {}, 0, target);
    }
    if (opcode == 0xe8) {
        const auto target = Relative(reader, Sign32(reader.Take32()));
        return Finish(reader, WasmMnemonic::Call, 64, {}, {}, 0, target);
    }
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        const auto reg = Register(Extend(opcode & 7, rex.B));
        if (rex.W) return Finish(reader, WasmMnemonic::MovImm, 64, reg, Immediate(reader.Take64()));
        return Finish(reader, WasmMnemonic::MovImm, 32, reg, Immediate(reader.Take32()));
    }
    if (opcode >= 0x00 && opcode <= 0x3f) return DecodeAlu(reader, opcode, rex);
    if (opcode == 0x80 || opcode == 0x82) Fail(reader.Address(), "8-bit arithmetic is not lifted");
    if (opcode == 0x81 || opcode == 0x83) return DecodeGroup1(reader, opcode, rex);
    if (opcode == 0x88 || opcode == 0x8a) Fail(reader.Address(), "8-bit move is not lifted");
    if (opcode == 0x89 || opcode == 0x8b) {
        const auto modrm = ReadModrm(reader, rex);
        const auto reg = Register(modrm.Reg);
        const auto rm = Rm(modrm);
        if (opcode == 0x89) return Finish(reader, WasmMnemonic::Mov, width, rm, reg);
        return Finish(reader, WasmMnemonic::Mov, width, reg, rm);
    }
    if (opcode == 0x8d) {
        const auto modrm = ReadModrm(reader, rex);
        if (!modrm.Memory) Fail(reader.Address(), "LEA without a memory operand");
        return Finish(reader, WasmMnemonic::Lea, width, Register(modrm.Reg), Memory(modrm));
    }
    if (opcode == 0xc6) Fail(reader.Address(), "8-bit move is not lifted");
    if (opcode == 0xc7) {
        const auto modrm = ReadModrm(reader, rex);
        if (modrm.Extension != 0) Fail(reader.Address(), "unsupported C7 opcode");
        const auto raw = reader.Take32();
        const auto immediate = rex.W ? static_cast<std::uint64_t>(Sign32(raw)) : raw;
        return Finish(reader, WasmMnemonic::MovImm, width, Rm(modrm), Immediate(immediate));
    }
    if (opcode == 0x85) {
        const auto modrm = ReadModrm(reader, rex);
        return Finish(reader, WasmMnemonic::Test, width, Rm(modrm), Register(modrm.Reg));
    }
    if (opcode == 0xf6) Fail(reader.Address(), "8-bit arithmetic is not lifted");
    if (opcode == 0xf7) {
        const auto modrm = ReadModrm(reader, rex);
        if (modrm.Extension != 0) Fail(reader.Address(), "unsupported unary opcode");
        const auto raw = reader.Take32();
        const auto immediate = rex.W ? static_cast<std::uint64_t>(Sign32(raw)) : raw;
        return Finish(reader, WasmMnemonic::Test, width, Rm(modrm), Immediate(immediate));
    }
    if (opcode == 0xfe) Fail(reader.Address(), "8-bit inc/dec is not lifted");
    if (opcode == 0xff) {
        const auto modrm = ReadModrm(reader, rex);
        if (modrm.Extension == 0 || modrm.Extension == 1) {
            return Finish(reader, modrm.Extension == 0 ? WasmMnemonic::Inc : WasmMnemonic::Dec, width, Rm(modrm), {});
        }
        if (modrm.Extension == 2 || modrm.Extension == 4) {
            if (modrm.Memory && modrm.Rip) return Finish(reader, modrm.Extension == 2 ? WasmMnemonic::CallSlot : WasmMnemonic::JmpSlot, 64, {}, Memory(modrm));
            Fail(reader.Address(), "indirect branch is not lifted");
        }
        if (modrm.Extension == 6) return Finish(reader, WasmMnemonic::Push, 64, {}, Rm(modrm));
        Fail(reader.Address(), "unsupported FF opcode");
    }
    Fail(reader.Address(), "unsupported opcode");
    return {};
}

}

bool WasmTerminates(WasmMnemonic op) {
    return op == WasmMnemonic::Jmp || op == WasmMnemonic::Jcc || op == WasmMnemonic::Call || op == WasmMnemonic::CallSlot || op == WasmMnemonic::JmpSlot || op == WasmMnemonic::Ret || op == WasmMnemonic::Halt;
}

WasmInsn DecodeWasmInsn(const std::uint8_t* data, std::size_t available, std::uint64_t address) {
    Reader reader(data, available, address);
    bool operand16 = false;
    Rex rex;
    while (reader.Peek(), true) {
        const auto byte = reader.Peek();
        if (byte == 0x66) {
            reader.Take();
            operand16 = true;
            continue;
        }
        if (byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 || byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65)
            Fail(address, "unsupported prefix");
        if (byte >= 0x40 && byte <= 0x4f) {
            reader.Take();
            rex.Present = true;
            rex.W = (byte & 0x8) != 0;
            rex.R = (byte & 0x4) != 0;
            rex.X = (byte & 0x2) != 0;
            rex.B = (byte & 0x1) != 0;
        }
        break;
    }
    const auto opcode = reader.Take();
    if (opcode == 0x0f) {
        auto insn = DecodeTwoByte(reader, rex, operand16);
        if (insn.Length == 0) Fail(address, "empty decode");
        return insn;
    }
    if (operand16) Fail(address, "16-bit operand size is not lifted");
    if (opcode == 0xc4 || opcode == 0xc5 || opcode == 0x62) Fail(address, "VEX and EVEX opcodes are not lifted");
    return DecodePrimary(reader, opcode, rex);
}

}
