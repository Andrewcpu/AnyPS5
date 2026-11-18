#include <wasm/Emit.hpp>
#include <algorithm>
#include <string>

namespace Relinker::Wasm {
namespace {

const char* kFlags = R"(
define internal i64 @anyps5_flags(i64 %old, i64 %result, i64 %lhs, i64 %rhs, i32 %width, i32 %kind) {
entry:
  %is32 = icmp eq i32 %width, 32
  %shift = select i1 %is32, i64 31, i64 63
  %islogic = icmp eq i32 %kind, 2
  %issub = icmp eq i32 %kind, 1
  %isdec = icmp eq i32 %kind, 4
  %dosub = or i1 %issub, %isdec
  %keepcf = icmp sge i32 %kind, 3
  %cfadd = icmp ult i64 %result, %lhs
  %cfsub = icmp ult i64 %lhs, %rhs
  %cfraw = select i1 %dosub, i1 %cfsub, i1 %cfadd
  %xorops = xor i64 %lhs, %rhs
  %notxor = xor i64 %xorops, -1
  %xorres = xor i64 %lhs, %result
  %ofaddv = and i64 %notxor, %xorres
  %ofsubv = and i64 %xorops, %xorres
  %ofv = select i1 %dosub, i64 %ofsubv, i64 %ofaddv
  %signbit = shl i64 1, %shift
  %ofbit = and i64 %ofv, %signbit
  %ofraw = icmp ne i64 %ofbit, 0
  %of = select i1 %islogic, i1 false, i1 %ofraw
  %afmix = xor i64 %xorops, %result
  %afbit = and i64 %afmix, 16
  %afraw = icmp ne i64 %afbit, 0
  %af = select i1 %islogic, i1 false, i1 %afraw
  %sfshift = lshr i64 %result, %shift
  %sfmask = and i64 %sfshift, 1
  %sf = icmp ne i64 %sfmask, 0
  %zf = icmp eq i64 %result, 0
  %p0 = and i64 %result, 255
  %p1 = lshr i64 %p0, 4
  %p2 = xor i64 %p0, %p1
  %p3 = lshr i64 %p2, 2
  %p4 = xor i64 %p2, %p3
  %p5 = lshr i64 %p4, 1
  %p6 = xor i64 %p4, %p5
  %p7 = and i64 %p6, 1
  %pf = icmp eq i64 %p7, 0
  %notkeep = xor i1 %keepcf, true
  %usefull = or i1 %islogic, %notkeep
  %clearmask = select i1 %usefull, i64 2261, i64 2260
  %notmask = xor i64 %clearmask, -1
  %cleared = and i64 %old, %notmask
  %base = or i64 %cleared, 2
  %notlogic = xor i1 %islogic, true
  %wantcf = and i1 %usefull, %notlogic
  %cf = and i1 %wantcf, %cfraw
  %cfv = select i1 %cf, i64 1, i64 0
  %pfv = select i1 %pf, i64 4, i64 0
  %afv = select i1 %af, i64 16, i64 0
  %zfv = select i1 %zf, i64 64, i64 0
  %sfv = select i1 %sf, i64 128, i64 0
  %ofvv = select i1 %of, i64 2048, i64 0
  %o1 = or i64 %base, %cfv
  %o2 = or i64 %o1, %pfv
  %o3 = or i64 %o2, %afv
  %o4 = or i64 %o3, %zfv
  %o5 = or i64 %o4, %sfv
  %o6 = or i64 %o5, %ofvv
  ret i64 %o6
}
)";

std::string Number(std::uint64_t value) {
    return std::to_string(static_cast<std::int64_t>(value));
}

std::string Hex(std::uint64_t value) {
    if (value == 0) return "0";
    std::string text;
    while (value != 0) {
        text.push_back("0123456789abcdef"[value & 0xf]);
        value >>= 4;
    }
    std::reverse(text.begin(), text.end());
    return text;
}

std::string LlvmBytes(const std::vector<std::uint8_t>& bytes) {
    std::string text = "c\"";
    for (const auto byte : bytes) {
        text.push_back('\\');
        text.push_back("0123456789ABCDEF"[byte >> 4]);
        text.push_back("0123456789ABCDEF"[byte & 0xf]);
    }
    text.push_back('"');
    return text;
}

int ScaleShift(int scale) {
    if (scale == 2) return 1;
    if (scale == 4) return 2;
    if (scale == 8) return 3;
    return 0;
}

class Ir {
public:
    explicit Ir(std::uint64_t guestBase, std::uint64_t guestLimit) : guestBase(guestBase), guestLimit(guestLimit) {}

    std::string Text;
    std::uint64_t guestBase;
    std::uint64_t guestLimit;

    std::string Temp() { return "%t" + std::to_string(temp++); }

    void Line(const std::string& line) { Text += "  " + line + "\n"; }
    void Label(const std::string& name) { Text += name + ":\n"; }

    std::string Const(std::uint64_t value) const { return Number(value); }

    std::string Bin(const char* op, const std::string& left, const std::string& right, const char* type = "i64") {
        const auto name = Temp();
        Line(name + " = " + op + " " + type + " " + left + ", " + right);
        return name;
    }

    std::string Add(const std::string& left, const std::string& right) { return Bin("add", left, right); }
    std::string Sub(const std::string& left, const std::string& right) { return Bin("sub", left, right); }
    std::string And(const std::string& left, const std::string& right) { return Bin("and", left, right); }
    std::string Or(const std::string& left, const std::string& right) { return Bin("or", left, right); }
    std::string Xor(const std::string& left, const std::string& right) { return Bin("xor", left, right); }
    std::string Lshr(const std::string& left, const std::string& right) { return Bin("lshr", left, right); }
    std::string Shl(const std::string& left, int amount) { return Bin("shl", left, std::to_string(amount)); }

    std::string Icmp(const char* condition, const std::string& left, const std::string& right, const char* type = "i64") {
        const auto name = Temp();
        Line(name + " = icmp " + condition + " " + type + " " + left + ", " + right);
        return name;
    }

    std::string BitOp(const char* op, const std::string& left, const std::string& right) {
        const auto name = Temp();
        Line(name + " = " + op + " i1 " + left + ", " + right);
        return name;
    }

    std::string Not(const std::string& value) { return BitOp("xor", value, "true"); }

    std::string Select(const std::string& condition, const std::string& taken, const std::string& other) {
        const auto name = Temp();
        Line(name + " = select i1 " + condition + ", i64 " + taken + ", i64 " + other);
        return name;
    }

    std::string Trunc32(const std::string& value) {
        const auto name = Temp();
        Line(name + " = trunc i64 " + value + " to i32");
        return name;
    }

    std::string Zext64(const std::string& value) {
        const auto name = Temp();
        Line(name + " = zext i32 " + value + " to i64");
        return name;
    }

    std::string IntToPtr(const std::string& value) {
        const auto name = Temp();
        Line(name + " = inttoptr i32 " + value + " to ptr");
        return name;
    }

    std::string Load(const char* type, const std::string& pointer) {
        const auto name = Temp();
        Line(name + " = load " + type + ", ptr " + pointer + ", align 1");
        return name;
    }

    void Store(const char* type, const std::string& value, const std::string& pointer) {
        Line(std::string("store ") + type + " " + value + ", ptr " + pointer + ", align 1");
    }

    std::string StatePtr(std::uint64_t offset) const {
        return "inttoptr (i32 " + std::to_string(kStateAddress + offset) + " to ptr)";
    }

    std::string LoadGpr(int reg) { return Load("i64", StatePtr(static_cast<std::uint64_t>(reg) * 8)); }
    void StoreGpr(int reg, const std::string& value) { Store("i64", value, StatePtr(static_cast<std::uint64_t>(reg) * 8)); }
    std::string LoadFlags() { return Load("i64", StatePtr(kRflagsOffset)); }
    void StoreFlags(const std::string& value) { Store("i64", value, StatePtr(kRflagsOffset)); }
    std::string LoadXmm(int reg, int half) { return Load("i64", StatePtr(kXmmOffset + static_cast<std::uint64_t>(reg) * 16 + static_cast<std::uint64_t>(half) * 8)); }
    void StoreXmm(int reg, int half, const std::string& value) { Store("i64", value, StatePtr(kXmmOffset + static_cast<std::uint64_t>(reg) * 16 + static_cast<std::uint64_t>(half) * 8)); }
    void StoreRip(const std::string& value) { Store("i64", value, StatePtr(kRipOffset)); }

    void Guard(const std::string& address, int size, std::uint64_t rip) {
        StoreRip(Const(rip));
        const auto end = Add(address, Const(static_cast<std::uint64_t>(size)));
        const auto wrap = Icmp("ult", end, address);
        const auto below = Icmp("ult", address, Const(guestBase));
        const auto above = Icmp("ugt", end, Const(guestLimit));
        const auto low = BitOp("or", wrap, below);
        const auto bad = BitOp("or", low, above);
        const auto ok = "ok" + std::to_string(label++);
        Line("br i1 " + bad + ", label %fault, label %" + ok);
        Label(ok);
    }

    std::string Translate(const std::string& address) {
        const auto name = Temp();
        Line(name + " = call i32 @anyps5_translate(i64 " + address + ")");
        return name;
    }

    void Branch(const std::string& target) { Line("br label %" + target); }

private:
    int temp = 0;
    int label = 0;
};

std::string AddressOf(Ir& ir, const WasmInsn& insn, const WasmOperand& memory) {
    if (memory.Rip) return ir.Const(insn.Next + static_cast<std::uint64_t>(memory.Disp));
    std::string address;
    bool any = false;
    if (memory.Base >= 0) {
        address = ir.LoadGpr(memory.Base);
        any = true;
    }
    if (memory.Index >= 0) {
        auto index = ir.LoadGpr(memory.Index);
        const auto shift = ScaleShift(memory.Scale);
        if (shift != 0) index = ir.Shl(index, shift);
        address = any ? ir.Add(address, index) : index;
        any = true;
    }
    if (memory.Disp != 0 || !any) {
        const auto displacement = ir.Const(static_cast<std::uint64_t>(memory.Disp));
        address = any ? ir.Add(address, displacement) : displacement;
    }
    return address;
}

std::string Mask(Ir& ir, std::string value, int width) {
    if (width == 64) return value;
    return ir.And(std::move(value), ir.Const(0xffffffffu));
}

std::string LoadMem(Ir& ir, const std::string& address, int width, std::uint64_t rip) {
    ir.Guard(address, width / 8, rip);
    const auto wasm = ir.Translate(address);
    if (width == 32) {
        const auto raw = ir.Temp();
        ir.Line(raw + " = call i32 @anyps5_read32(i32 " + wasm + ")");
        return ir.Zext64(raw);
    }
    const auto raw = ir.Temp();
    ir.Line(raw + " = call i64 @anyps5_read64(i32 " + wasm + ")");
    return raw;
}

void StoreMem(Ir& ir, const std::string& address, const std::string& value, int width, std::uint64_t rip) {
    ir.Guard(address, width / 8, rip);
    const auto wasm = ir.Translate(address);
    if (width == 32) {
        const auto low = ir.Trunc32(value);
        ir.Line("call void @anyps5_write32(i32 " + wasm + ", i32 " + low + ")");
        return;
    }
    ir.Line("call void @anyps5_write64(i32 " + wasm + ", i64 " + value + ")");
}

std::string LoadOperand(Ir& ir, const WasmInsn& insn, const WasmOperand& operand, int width) {
    if (operand.Kind == WasmOperandKind::Imm) return ir.Const(operand.Imm);
    if (operand.Kind == WasmOperandKind::Reg) {
        if (operand.Xmm) return ir.LoadXmm(operand.Reg, 0);
        return Mask(ir, ir.LoadGpr(operand.Reg), width);
    }
    return LoadMem(ir, AddressOf(ir, insn, operand), width, insn.Address);
}

void StoreOperand(Ir& ir, const WasmInsn& insn, const WasmOperand& operand, const std::string& value, int width) {
    if (operand.Kind == WasmOperandKind::Reg) {
        if (operand.Xmm) ir.StoreXmm(operand.Reg, 0, value);
        else ir.StoreGpr(operand.Reg, value);
        return;
    }
    StoreMem(ir, AddressOf(ir, insn, operand), value, width, insn.Address);
}

int FlagKind(WasmMnemonic op) {
    if (op == WasmMnemonic::Add) return 0;
    if (op == WasmMnemonic::Sub || op == WasmMnemonic::Cmp) return 1;
    if (op == WasmMnemonic::Inc) return 3;
    if (op == WasmMnemonic::Dec) return 4;
    return 2;
}

void EmitAlu(Ir& ir, const WasmInsn& insn) {
    const auto kind = FlagKind(insn.Op);
    auto lhs = LoadOperand(ir, insn, insn.Dst, insn.Width);
    std::string rhs = kind == 3 || kind == 4 ? ir.Const(1) : LoadOperand(ir, insn, insn.Src, insn.Width);
    lhs = Mask(ir, std::move(lhs), insn.Width);
    rhs = Mask(ir, std::move(rhs), insn.Width);
    std::string result;
    if (kind == 2) {
        const char* op = insn.Op == WasmMnemonic::Or ? "or" : insn.Op == WasmMnemonic::Xor ? "xor" : "and";
        result = ir.Bin(op, lhs, rhs);
    } else if (kind == 0 || kind == 3) {
        result = ir.Add(lhs, rhs);
    } else {
        result = ir.Sub(lhs, rhs);
    }
    result = Mask(ir, std::move(result), insn.Width);
    const auto flags = ir.Temp();
    ir.Line(flags + " = call i64 @anyps5_flags(i64 " + ir.LoadFlags() + ", i64 " + result + ", i64 " + lhs + ", i64 " + rhs + ", i32 " + std::to_string(insn.Width) + ", i32 " + std::to_string(kind) + ")");
    ir.StoreFlags(flags);
    if (insn.Op != WasmMnemonic::Cmp && insn.Op != WasmMnemonic::Test) StoreOperand(ir, insn, insn.Dst, result, insn.Width);
}

std::string Bit(Ir& ir, const std::string& flags, int bit) {
    return ir.Icmp("ne", ir.And(ir.Lshr(flags, ir.Const(static_cast<std::uint64_t>(bit))), ir.Const(1)), ir.Const(0));
}

std::string Condition(Ir& ir, std::uint8_t cc, const std::string& flags) {
    const auto cf = Bit(ir, flags, 0);
    const auto pf = Bit(ir, flags, 2);
    const auto zf = Bit(ir, flags, 6);
    const auto sf = Bit(ir, flags, 7);
    const auto of = Bit(ir, flags, 11);
    const auto sign = ir.BitOp("xor", sf, of);
    const auto belowOrEqual = ir.BitOp("or", cf, zf);
    const auto lessOrEqual = ir.BitOp("or", sign, zf);
    switch (cc) {
    case 0: return of;
    case 1: return ir.Not(of);
    case 2: return cf;
    case 3: return ir.Not(cf);
    case 4: return zf;
    case 5: return ir.Not(zf);
    case 6: return belowOrEqual;
    case 7: return ir.Not(belowOrEqual);
    case 8: return sf;
    case 9: return ir.Not(sf);
    case 10: return pf;
    case 11: return ir.Not(pf);
    case 12: return sign;
    case 13: return ir.Not(sign);
    case 14: return lessOrEqual;
    default: return ir.Not(lessOrEqual);
    }
}

void LoadXmmPair(Ir& ir, const WasmInsn& insn, const WasmOperand& operand, std::string& low, std::string& high) {
    if (operand.Kind == WasmOperandKind::Reg) {
        low = ir.LoadXmm(operand.Reg, 0);
        high = ir.LoadXmm(operand.Reg, 1);
        return;
    }
    const auto address = AddressOf(ir, insn, operand);
    ir.Guard(address, 16, insn.Address);
    const auto lowWasm = ir.Translate(address);
    const auto lowRaw = ir.Temp();
    ir.Line(lowRaw + " = call i64 @anyps5_read64(i32 " + lowWasm + ")");
    low = lowRaw;
    const auto highAddress = ir.Add(address, ir.Const(8));
    const auto highWasm = ir.Translate(highAddress);
    const auto highRaw = ir.Temp();
    ir.Line(highRaw + " = call i64 @anyps5_read64(i32 " + highWasm + ")");
    high = highRaw;
}

void EmitXmm(Ir& ir, const WasmInsn& insn) {
    if (insn.Op == WasmMnemonic::Pxor) {
        std::string leftLow, leftHigh, rightLow, rightHigh;
        LoadXmmPair(ir, insn, insn.Dst, leftLow, leftHigh);
        LoadXmmPair(ir, insn, insn.Src, rightLow, rightHigh);
        ir.StoreXmm(insn.Dst.Reg, 0, ir.Xor(leftLow, rightLow));
        ir.StoreXmm(insn.Dst.Reg, 1, ir.Xor(leftHigh, rightHigh));
        return;
    }
    if (insn.Op == WasmMnemonic::MovdToXmm || insn.Op == WasmMnemonic::MovqToXmm) {
        auto value = LoadOperand(ir, insn, insn.Src, insn.Width);
        value = Mask(ir, std::move(value), insn.Width);
        ir.StoreXmm(insn.Dst.Reg, 0, value);
        ir.StoreXmm(insn.Dst.Reg, 1, ir.Const(0));
        return;
    }
    auto value = ir.LoadXmm(insn.Src.Reg, 0);
    value = Mask(ir, std::move(value), insn.Width);
    StoreOperand(ir, insn, insn.Dst, value, insn.Width);
}

void EmitInsn(Ir& ir, const WasmInsn& insn) {
    switch (insn.Op) {
    case WasmMnemonic::Nop:
        return;
    case WasmMnemonic::Mov:
    case WasmMnemonic::MovImm:
    case WasmMnemonic::Lea: {
        auto value = insn.Op == WasmMnemonic::Lea ? AddressOf(ir, insn, insn.Src) : LoadOperand(ir, insn, insn.Src, insn.Width);
        value = Mask(ir, std::move(value), insn.Width);
        StoreOperand(ir, insn, insn.Dst, value, insn.Width);
        return;
    }
    case WasmMnemonic::Add:
    case WasmMnemonic::Sub:
    case WasmMnemonic::Cmp:
    case WasmMnemonic::And:
    case WasmMnemonic::Or:
    case WasmMnemonic::Xor:
    case WasmMnemonic::Test:
    case WasmMnemonic::Inc:
    case WasmMnemonic::Dec:
        EmitAlu(ir, insn);
        return;
    case WasmMnemonic::Push: {
        const auto value = LoadOperand(ir, insn, insn.Src, 64);
        const auto rsp = ir.LoadGpr(kRsp);
        const auto slot = ir.Sub(rsp, ir.Const(8));
        StoreMem(ir, slot, value, 64, insn.Address);
        ir.StoreGpr(kRsp, slot);
        return;
    }
    case WasmMnemonic::PushFlags: {
        const auto rsp = ir.LoadGpr(kRsp);
        const auto slot = ir.Sub(rsp, ir.Const(8));
        StoreMem(ir, slot, ir.LoadFlags(), 64, insn.Address);
        ir.StoreGpr(kRsp, slot);
        return;
    }
    case WasmMnemonic::Pop: {
        const auto rsp = ir.LoadGpr(kRsp);
        const auto value = LoadMem(ir, rsp, 64, insn.Address);
        ir.StoreGpr(kRsp, ir.Add(rsp, ir.Const(8)));
        StoreOperand(ir, insn, insn.Dst, value, 64);
        return;
    }
    case WasmMnemonic::MovdToXmm:
    case WasmMnemonic::MovqToXmm:
    case WasmMnemonic::MovdFromXmm:
    case WasmMnemonic::MovqFromXmm:
    case WasmMnemonic::Pxor:
        EmitXmm(ir, insn);
        return;
    case WasmMnemonic::Jmp:
    case WasmMnemonic::Jcc:
    case WasmMnemonic::Call:
    case WasmMnemonic::Ret:
    case WasmMnemonic::Halt:
        return;
    }
}

void EmitEdge(Ir& ir, const WasmBlock& block) {
    if (block.EdgeKind == WasmBlock::Kind::Halt) {
        ir.StoreRip(ir.Const(block.Edge));
        ir.Branch("halt");
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::Fall || block.EdgeKind == WasmBlock::Kind::Jump) {
        ir.StoreRip(ir.Const(block.EdgeKind == WasmBlock::Kind::Jump ? block.Target : block.Fall));
        ir.Branch("dispatch");
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::Branch) {
        const auto next = ir.Select(Condition(ir, block.Cc, ir.LoadFlags()), ir.Const(block.Target), ir.Const(block.Fall));
        ir.StoreRip(next);
        ir.Branch("dispatch");
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::Call) {
        const auto rsp = ir.LoadGpr(kRsp);
        const auto slot = ir.Sub(rsp, ir.Const(8));
        StoreMem(ir, slot, ir.Const(block.Fall), 64, block.Edge);
        ir.StoreGpr(kRsp, slot);
        ir.StoreRip(ir.Const(block.Target));
        ir.Branch("dispatch");
        return;
    }
    const auto rsp = ir.LoadGpr(kRsp);
    const auto next = LoadMem(ir, rsp, 64, block.Edge);
    ir.StoreGpr(kRsp, ir.Add(rsp, ir.Const(8)));
    ir.StoreRip(next);
    ir.Branch("dispatch");
}

std::string MemoryHelpers(std::uint64_t guestBase) {
    std::string text;
    text += "define internal i32 @anyps5_translate(i64 %guest) {\n";
    text += "  %delta = sub i64 %guest, " + Number(guestBase) + "\n";
    text += "  %sum = add i64 %delta, " + Number(kWasmGuestBase) + "\n";
    text += "  %wasm = trunc i64 %sum to i32\n";
    text += "  ret i32 %wasm\n}\n";
    text += "define internal i32 @anyps5_read32(i32 %wasm) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  %value = load i32, ptr %ptr, align 1\n";
    text += "  ret i32 %value\n}\n";
    text += "define internal i64 @anyps5_read64(i32 %wasm) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  %value = load i64, ptr %ptr, align 1\n";
    text += "  ret i64 %value\n}\n";
    text += "define internal void @anyps5_write32(i32 %wasm, i32 %value) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  store i32 %value, ptr %ptr, align 1\n";
    text += "  ret void\n}\n";
    text += "define internal void @anyps5_write64(i32 %wasm, i64 %value) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  store i64 %value, ptr %ptr, align 1\n";
    text += "  ret void\n}\n";
    return text;
}

std::vector<std::uint8_t> Content(const WasmSegment& segment) {
    return {segment.Bytes.begin(), segment.Bytes.begin() + static_cast<std::ptrdiff_t>(segment.ContentSize)};
}

}

std::string EmitWasmModule(const WasmImage& image, const std::vector<WasmBlock>& blocks) {
    std::string module;
    module += "target datalayout = \"e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-n32:64-S128-ni:1:10:20\"\n";
    module += "target triple = \"wasm32\"\n\n";
    module += "define internal void @anyps5_memset(ptr %dst, i32 %size) {\n";
    module += "entry:\n  br label %loop\nloop:\n  %index = phi i32 [ 0, %entry ], [ %next, %body ]\n";
    module += "  %done = icmp uge i32 %index, %size\n  br i1 %done, label %exit, label %body\nbody:\n";
    module += "  %slot = getelementptr i8, ptr %dst, i32 %index\n  store i8 0, ptr %slot, align 1\n";
    module += "  %next = add i32 %index, 1\n  br label %loop\nexit:\n  ret void\n}\n";
    module += "define internal void @anyps5_memcpy(ptr %dst, ptr %src, i32 %size) {\n";
    module += "entry:\n  br label %loop\nloop:\n  %index = phi i32 [ 0, %entry ], [ %next, %body ]\n";
    module += "  %done = icmp uge i32 %index, %size\n  br i1 %done, label %exit, label %body\nbody:\n";
    module += "  %from = getelementptr i8, ptr %src, i32 %index\n  %byte = load i8, ptr %from, align 1\n";
    module += "  %to = getelementptr i8, ptr %dst, i32 %index\n  store i8 %byte, ptr %to, align 1\n";
    module += "  %next = add i32 %index, 1\n  br label %loop\nexit:\n  ret void\n}\n";
    module += "@anyps5_steps = global i32 0\n";
    for (std::size_t index = 0; index < image.Segments.size(); ++index) {
        const auto& segment = image.Segments[index];
        if (segment.ContentSize == 0) continue;
        const auto bytes = Content(segment);
        module += "@seg" + std::to_string(index) + " = private unnamed_addr constant [" + std::to_string(bytes.size()) + " x i8] " + LlvmBytes(bytes) + "\n";
    }
    module += MemoryHelpers(image.GuestBase);
    module += kFlags;
    module += "\ndefine i32 @anyps5_run() {\nentry:\n";
    Ir ir(image.GuestBase, image.GuestLimit);
    const auto span = static_cast<std::uint32_t>(image.GuestLimit - image.GuestBase);
    const auto base = ir.IntToPtr(ir.Translate(Number(image.GuestBase)));
    ir.Line("call void @anyps5_memset(ptr " + base + ", i32 " + std::to_string(span) + ")");
    for (std::size_t index = 0; index < image.Segments.size(); ++index) {
        const auto& segment = image.Segments[index];
        if (segment.ContentSize == 0) continue;
        const auto dest = ir.IntToPtr(ir.Translate(Number(segment.Virtual)));
        ir.Line("call void @anyps5_memcpy(ptr " + dest + ", ptr @seg" + std::to_string(index) + ", i32 " + std::to_string(static_cast<std::uint32_t>(segment.ContentSize)) + ")");
    }
    for (int reg = 0; reg < 16; ++reg) {
        ir.StoreGpr(reg, ir.Const(0));
        ir.StoreXmm(reg, 0, ir.Const(0));
        ir.StoreXmm(reg, 1, ir.Const(0));
    }
    ir.StoreGpr(kRsp, ir.Const(image.StackTop));
    ir.StoreRip(ir.Const(image.Entry));
    ir.StoreFlags(ir.Const(kInitialFlags));
    ir.Line("store i32 0, ptr @anyps5_steps, align 4");
    ir.Branch("dispatch");
    ir.Label("dispatch");
    const auto steps = ir.Temp();
    ir.Line(steps + " = load i32, ptr @anyps5_steps, align 4");
    const auto stop = ir.Icmp("uge", steps, std::to_string(kStepLimit), "i32");
    ir.Line("br i1 " + stop + ", label %limit, label %step");
    ir.Label("step");
    const auto advanced = ir.Temp();
    ir.Line(advanced + " = add i32 " + steps + ", 1");
    ir.Line("store i32 " + advanced + ", ptr @anyps5_steps, align 4");
    const auto rip = ir.Load("i64", ir.StatePtr(kRipOffset));
    ir.Text += "  switch i64 " + rip + ", label %missing [\n";
    for (const auto& block : blocks) ir.Text += "    i64 " + Number(block.Start) + ", label %g" + Hex(block.Start) + "\n";
    ir.Text += "  ]\n";
    for (const auto& block : blocks) {
        ir.Label("g" + Hex(block.Start));
        for (const auto& insn : block.Insns) EmitInsn(ir, insn);
        EmitEdge(ir, block);
    }
    ir.Label("halt");
    ir.Line("ret i32 0");
    ir.Label("missing");
    ir.Line("ret i32 1");
    ir.Label("fault");
    ir.Line("ret i32 2");
    ir.Label("limit");
    ir.Line("ret i32 3");
    module += ir.Text;
    module += "}\n\n";
    module += "define i32 @anyps5_state() { ret i32 " + std::to_string(static_cast<std::uint32_t>(kStateAddress)) + " }\n";
    module += "define i64 @anyps5_guest_base() { ret i64 " + Number(image.GuestBase) + " }\n";
    module += "define i64 @anyps5_guest_limit() { ret i64 " + Number(image.GuestLimit) + " }\n";
    module += "define i32 @anyps5_memory_base() { ret i32 " + std::to_string(static_cast<std::uint32_t>(kWasmGuestBase)) + " }\n";
    return module;
}

}
