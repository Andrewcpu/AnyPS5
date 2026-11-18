#include <wasm/Emit.hpp>
#include <algorithm>
#include <string>

namespace Relinker::Wasm {
namespace {

const char* kFlags = R"(
define internal i64 @anyps5_flags(i64 %old, i64 %result, i64 %lhs, i64 %rhs, i32 %width, i32 %kind) {
entry:
  %is64 = icmp eq i32 %width, 64
  %is8 = icmp eq i32 %width, 8
  %is16 = icmp eq i32 %width, 16
  %s64 = select i1 %is64, i64 63, i64 31
  %s16 = select i1 %is16, i64 15, i64 %s64
  %shift = select i1 %is8, i64 7, i64 %s16
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

const char* kShiftFlags = R"(
define internal i64 @anyps5_shift_flags(i64 %old, i64 %value, i64 %result, i64 %count, i32 %width, i32 %kind) {
entry:
  %zero = icmp eq i64 %count, 0
  br i1 %zero, label %keep, label %calc
keep:
  ret i64 %old
calc:
  %is64 = icmp eq i32 %width, 64
  %wide = select i1 %is64, i64 64, i64 32
  %sign = select i1 %is64, i64 63, i64 31
  %shl = icmp eq i32 %kind, 0
  %sar = icmp eq i32 %kind, 2
  %fromtop = sub i64 %wide, %count
  %below = sub i64 %count, 1
  %cfindex = select i1 %shl, i64 %fromtop, i64 %below
  %cfsrc = lshr i64 %value, %cfindex
  %cfbit = and i64 %cfsrc, 1
  %cf = icmp ne i64 %cfbit, 0
  %one = icmp eq i64 %count, 1
  %rshift = lshr i64 %result, %sign
  %vshift = lshr i64 %value, %sign
  %rmsb = and i64 %rshift, 1
  %vmsb = and i64 %vshift, 1
  %rmsb1 = icmp ne i64 %rmsb, 0
  %vmsb1 = icmp ne i64 %vmsb, 0
  %xorof = xor i1 %rmsb1, %cf
  %notsar = xor i1 %sar, true
  %shrOf = select i1 %notsar, i1 %vmsb1, i1 false
  %of1 = select i1 %shl, i1 %xorof, i1 %shrOf
  %of = select i1 %one, i1 %of1, i1 false
  %sfsrc = lshr i64 %result, %sign
  %sfbit = and i64 %sfsrc, 1
  %sf = icmp ne i64 %sfbit, 0
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
  %mask = xor i64 2261, -1
  %cleared = and i64 %old, %mask
  %base = or i64 %cleared, 2
  %cfv = select i1 %cf, i64 1, i64 0
  %pfv = select i1 %pf, i64 4, i64 0
  %zfv = select i1 %zf, i64 64, i64 0
  %sfv = select i1 %sf, i64 128, i64 0
  %ofv = select i1 %of, i64 2048, i64 0
  %o1 = or i64 %base, %cfv
  %o2 = or i64 %o1, %pfv
  %o3 = or i64 %o2, %zfv
  %o4 = or i64 %o3, %sfv
  %o5 = or i64 %o4, %ofv
  ret i64 %o5
}
)";

const char* kDiv = R"(
define internal { i64, i64 } @anyps5_div64(i64 %hi, i64 %lo, i64 %d) {
entry:
  br label %loop
loop:
  %bit = phi i64 [ 64, %entry ], [ %next, %step ]
  %rem = phi i64 [ %hi, %entry ], [ %newrem, %step ]
  %quot = phi i64 [ 0, %entry ], [ %newquot, %step ]
  %done = icmp eq i64 %bit, 0
  br i1 %done, label %exit, label %step
step:
  %top = lshr i64 %rem, 63
  %shifted = shl i64 %rem, 1
  %index = sub i64 %bit, 1
  %src = lshr i64 %lo, %index
  %bitv = and i64 %src, 1
  %joined = or i64 %shifted, %bitv
  %overflow = icmp ne i64 %top, 0
  %geword = icmp uge i64 %joined, %d
  %ge = or i1 %overflow, %geword
  %sub = sub i64 %joined, %d
  %newrem = select i1 %ge, i64 %sub, i64 %joined
  %qsh = shl i64 %quot, 1
  %qbit = zext i1 %ge to i64
  %newquot = or i64 %qsh, %qbit
  %next = sub i64 %bit, 1
  br label %loop
exit:
  %pair0 = insertvalue { i64, i64 } undef, i64 %quot, 0
  %pair1 = insertvalue { i64, i64 } %pair0, i64 %rem, 1
  ret { i64, i64 } %pair1
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

    std::string Sext64(const std::string& value) {
        const auto name = Temp();
        Line(name + " = sext i32 " + value + " to i64");
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

    std::string NextLabel(const char* prefix) { return std::string(prefix) + std::to_string(label++); }

private:
    int temp = 0;
    int label = 0;
};

class Locals {
public:
    explicit Locals(Ir& ir) : ir(ir) {}

    std::string Read(int reg) {
        if (gpr[reg].empty()) gpr[reg] = ir.LoadGpr(reg);
        return gpr[reg];
    }

    void Write(int reg, const std::string& value) {
        gpr[reg] = value;
        dirty[reg] = true;
    }

    std::string ReadFlags() {
        if (flags.empty()) flags = ir.LoadFlags();
        return flags;
    }

    void WriteFlags(const std::string& value) {
        flags = value;
        flagsDirty = true;
    }

    void Spill() {
        for (int reg = 0; reg < 16; ++reg) {
            if (!dirty[reg]) continue;
            ir.StoreGpr(reg, gpr[reg]);
            dirty[reg] = false;
        }
        if (!flagsDirty) return;
        ir.StoreFlags(flags);
        flagsDirty = false;
    }

private:
    Ir& ir;
    std::string gpr[16];
    bool dirty[16]{};
    std::string flags;
    bool flagsDirty = false;
};

std::string AddressOf(Ir& ir, Locals& locals, const WasmInsn& insn, const WasmOperand& memory) {
    if (memory.Rip) return ir.Const(insn.Next + static_cast<std::uint64_t>(memory.Disp));
    std::string address;
    bool any = false;
    if (memory.Base >= 0) {
        address = locals.Read(memory.Base);
        any = true;
    }
    if (memory.Index >= 0) {
        auto index = locals.Read(memory.Index);
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
    const auto bits = width == 8 ? 0xffull : width == 16 ? 0xffffull : 0xffffffffull;
    return ir.And(std::move(value), ir.Const(bits));
}

std::string LoadMem(Ir& ir, Locals& locals, const std::string& address, int width, std::uint64_t rip) {
    locals.Spill();
    ir.Guard(address, width / 8, rip);
    const auto wasm = ir.Translate(address);
    if (width == 8) {
        const auto raw = ir.Temp();
        ir.Line(raw + " = call i32 @anyps5_read8(i32 " + wasm + ")");
        return ir.Zext64(raw);
    }
    if (width == 32) {
        const auto raw = ir.Temp();
        ir.Line(raw + " = call i32 @anyps5_read32(i32 " + wasm + ")");
        return ir.Zext64(raw);
    }
    const auto raw = ir.Temp();
    ir.Line(raw + " = call i64 @anyps5_read64(i32 " + wasm + ")");
    return raw;
}

void StoreMem(Ir& ir, Locals& locals, const std::string& address, const std::string& value, int width, std::uint64_t rip) {
    locals.Spill();
    ir.Guard(address, width / 8, rip);
    const auto wasm = ir.Translate(address);
    if (width == 8) {
        const auto low = ir.Trunc32(value);
        ir.Line("call void @anyps5_write8(i32 " + wasm + ", i32 " + low + ")");
        return;
    }
    if (width == 32) {
        const auto low = ir.Trunc32(value);
        ir.Line("call void @anyps5_write32(i32 " + wasm + ", i32 " + low + ")");
        return;
    }
    ir.Line("call void @anyps5_write64(i32 " + wasm + ", i64 " + value + ")");
}

std::string LoadOperand(Ir& ir, Locals& locals, const WasmInsn& insn, const WasmOperand& operand, int width) {
    if (operand.Kind == WasmOperandKind::Imm) return ir.Const(operand.Imm);
    if (operand.Kind == WasmOperandKind::Reg) {
        if (operand.Xmm) return ir.LoadXmm(operand.Reg, 0);
        return Mask(ir, locals.Read(operand.Reg), width);
    }
    return LoadMem(ir, locals, AddressOf(ir, locals, insn, operand), width, insn.Address);
}

void StoreOperand(Ir& ir, Locals& locals, const WasmInsn& insn, const WasmOperand& operand, const std::string& value, int width) {
    if (operand.Kind == WasmOperandKind::Reg) {
        if (operand.Xmm) ir.StoreXmm(operand.Reg, 0, value);
        else if (width == 8) {
            const auto kept = ir.And(locals.Read(operand.Reg), ir.Const(~std::uint64_t{0xff}));
            const auto low = ir.And(value, ir.Const(0xff));
            locals.Write(operand.Reg, ir.Or(kept, low));
        } else locals.Write(operand.Reg, value);
        return;
    }
    StoreMem(ir, locals, AddressOf(ir, locals, insn, operand), value, width, insn.Address);
}

int FlagKind(WasmMnemonic op) {
    if (op == WasmMnemonic::Add) return 0;
    if (op == WasmMnemonic::Sub || op == WasmMnemonic::Cmp) return 1;
    if (op == WasmMnemonic::Inc) return 3;
    if (op == WasmMnemonic::Dec) return 4;
    return 2;
}

void EmitAlu(Ir& ir, Locals& locals, const WasmInsn& insn, bool deadFlags) {
    if (deadFlags && (insn.Op == WasmMnemonic::Cmp || insn.Op == WasmMnemonic::Test)) {
        if (insn.Dst.Kind == WasmOperandKind::Mem) (void)LoadOperand(ir, locals, insn, insn.Dst, insn.Width);
        if (insn.Src.Kind == WasmOperandKind::Mem) (void)LoadOperand(ir, locals, insn, insn.Src, insn.Width);
        return;
    }
    const auto kind = FlagKind(insn.Op);
    auto lhs = LoadOperand(ir, locals, insn, insn.Dst, insn.Width);
    std::string rhs = kind == 3 || kind == 4 ? ir.Const(1) : LoadOperand(ir, locals, insn, insn.Src, insn.Width);
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
    if (!deadFlags) {
        const auto flags = ir.Temp();
        ir.Line(flags + " = call i64 @anyps5_flags(i64 " + locals.ReadFlags() + ", i64 " + result + ", i64 " + lhs + ", i64 " + rhs + ", i32 " + std::to_string(insn.Width) + ", i32 " + std::to_string(kind) + ")");
        locals.WriteFlags(flags);
    }
    if (insn.Op != WasmMnemonic::Cmp && insn.Op != WasmMnemonic::Test) StoreOperand(ir, locals, insn, insn.Dst, result, insn.Width);
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

void LoadXmmPair(Ir& ir, Locals& locals, const WasmInsn& insn, const WasmOperand& operand, std::string& low, std::string& high) {
    if (operand.Kind == WasmOperandKind::Reg) {
        low = ir.LoadXmm(operand.Reg, 0);
        high = ir.LoadXmm(operand.Reg, 1);
        return;
    }
    const auto address = AddressOf(ir, locals, insn, operand);
    locals.Spill();
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

void EmitXmm(Ir& ir, Locals& locals, const WasmInsn& insn) {
    if (insn.Op == WasmMnemonic::Pxor) {
        std::string leftLow, leftHigh, rightLow, rightHigh;
        LoadXmmPair(ir, locals, insn, insn.Dst, leftLow, leftHigh);
        LoadXmmPair(ir, locals, insn, insn.Src, rightLow, rightHigh);
        ir.StoreXmm(insn.Dst.Reg, 0, ir.Xor(leftLow, rightLow));
        ir.StoreXmm(insn.Dst.Reg, 1, ir.Xor(leftHigh, rightHigh));
        return;
    }
    if (insn.Op == WasmMnemonic::MovdToXmm || insn.Op == WasmMnemonic::MovqToXmm) {
        auto value = LoadOperand(ir, locals, insn, insn.Src, insn.Width);
        value = Mask(ir, std::move(value), insn.Width);
        ir.StoreXmm(insn.Dst.Reg, 0, value);
        ir.StoreXmm(insn.Dst.Reg, 1, ir.Const(0));
        return;
    }
    auto value = ir.LoadXmm(insn.Src.Reg, 0);
    value = Mask(ir, std::move(value), insn.Width);
    StoreOperand(ir, locals, insn, insn.Dst, value, insn.Width);
}

void EmitShift(Ir& ir, Locals& locals, const WasmInsn& insn, bool deadFlags) {
    const auto mask = insn.Width == 64 ? 63ull : 31ull;
    std::string count;
    if (insn.Src.Kind == WasmOperandKind::Imm) count = ir.Const(insn.Src.Imm & mask);
    else count = ir.And(Mask(ir, LoadOperand(ir, locals, insn, insn.Src, 8), 8), ir.Const(mask));
    auto value = Mask(ir, LoadOperand(ir, locals, insn, insn.Dst, insn.Width), insn.Width);
    std::string shifted;
    if (insn.Op == WasmMnemonic::Sar && insn.Width == 32) {
        const auto bits = ir.Trunc32(value);
        const auto amount = ir.Trunc32(count);
        const auto raw = ir.Temp();
        ir.Line(raw + " = ashr i32 " + bits + ", " + amount);
        shifted = ir.Zext64(raw);
    } else if (insn.Op == WasmMnemonic::Shl) shifted = ir.Bin("shl", value, count);
    else if (insn.Op == WasmMnemonic::Shr) shifted = ir.Bin("lshr", value, count);
    else shifted = ir.Bin("ashr", value, count);
    shifted = Mask(ir, std::move(shifted), insn.Width);
    if (!deadFlags) {
        const int kind = insn.Op == WasmMnemonic::Shl ? 0 : insn.Op == WasmMnemonic::Shr ? 1 : 2;
        const auto flags = ir.Temp();
        ir.Line(flags + " = call i64 @anyps5_shift_flags(i64 " + locals.ReadFlags() + ", i64 " + value + ", i64 " + shifted + ", i64 " + count + ", i32 " + std::to_string(insn.Width) + ", i32 " + std::to_string(kind) + ")");
        locals.WriteFlags(flags);
    }
    StoreOperand(ir, locals, insn, insn.Dst, shifted, insn.Width);
}

void EmitImul(Ir& ir, Locals& locals, const WasmInsn& insn, bool deadFlags) {
    const bool three = insn.Op == WasmMnemonic::Imul3;
    auto left = Mask(ir, LoadOperand(ir, locals, insn, three ? insn.Src : insn.Dst, insn.Width), insn.Width);
    auto right = three ? Mask(ir, ir.Const(insn.Extra), insn.Width) : Mask(ir, LoadOperand(ir, locals, insn, insn.Src, insn.Width), insn.Width);
    auto product = Mask(ir, ir.Bin("mul", left, right), insn.Width);
    if (!deadFlags) {
        std::string overflow;
        if (insn.Width == 32) {
            const auto wide = ir.Bin("mul", ir.Sext64(ir.Trunc32(left)), ir.Sext64(ir.Trunc32(right)));
            overflow = ir.Icmp("ne", wide, ir.Sext64(ir.Trunc32(product)));
        } else {
            const auto minv = ir.Const(0x8000000000000000ull);
            const auto neg1 = ir.Const(~std::uint64_t{0});
            const auto zero = ir.Const(0);
            const auto one = ir.Const(1);
            const auto aMin = ir.Icmp("eq", left, minv);
            const auto bMin = ir.Icmp("eq", right, minv);
            const auto aNeg1 = ir.Icmp("eq", left, neg1);
            const auto bNeg1 = ir.Icmp("eq", right, neg1);
            const auto eitherMin = ir.BitOp("or", aMin, bMin);
            const auto aSmall = ir.BitOp("or", ir.Icmp("eq", left, zero), ir.Icmp("eq", left, one));
            const auto bSmall = ir.BitOp("or", ir.Icmp("eq", right, zero), ir.Icmp("eq", right, one));
            const auto aOverflow = ir.BitOp("and", aMin, ir.Not(bSmall));
            const auto bOverflow = ir.BitOp("and", bMin, ir.Not(aSmall));
            const auto minOverflow = ir.BitOp("or", aOverflow, bOverflow);
            const auto aNeg = ir.Icmp("slt", left, zero);
            const auto bNeg = ir.Icmp("slt", right, zero);
            const auto ua = ir.Select(ir.BitOp("or", aMin, ir.Not(aNeg)), left, ir.Sub(zero, left));
            const auto ub = ir.Select(ir.BitOp("or", bMin, ir.Not(bNeg)), right, ir.Sub(zero, right));
            const auto differ = ir.BitOp("xor", aNeg, bNeg);
            const auto limit = ir.Select(differ, minv, ir.Const(0x7fffffffffffffffull));
            const auto nonzero = ir.BitOp("or", ir.Icmp("eq", left, zero), ir.Icmp("eq", right, zero));
            const auto safeUb = ir.Select(nonzero, one, ub);
            const auto room = ir.Bin("udiv", limit, safeUb);
            const auto absOverflow = ir.Icmp("ugt", ua, room);
            const auto normal = ir.BitOp("and", ir.Not(eitherMin), ir.Not(nonzero));
            overflow = ir.BitOp("or", minOverflow, ir.BitOp("and", normal, absOverflow));
        }
        const auto base = ir.Temp();
        ir.Line(base + " = call i64 @anyps5_flags(i64 " + locals.ReadFlags() + ", i64 " + product + ", i64 " + left + ", i64 " + right + ", i32 " + std::to_string(insn.Width) + ", i32 2)");
        const auto withCarry = ir.Or(base, ir.Select(overflow, ir.Const(1), ir.Const(0)));
        locals.WriteFlags(ir.Or(withCarry, ir.Select(overflow, ir.Const(2048), ir.Const(0))));
    }
    StoreOperand(ir, locals, insn, insn.Dst, product, insn.Width);
}

void EmitDiv(Ir& ir, Locals& locals, const WasmInsn& insn) {
    const auto width = insn.Width;
    const auto divisor = Mask(ir, LoadOperand(ir, locals, insn, insn.Dst, width), width);
    const auto lo = Mask(ir, locals.Read(0), width);
    const auto hi = Mask(ir, locals.Read(2), width);
    const auto zero = ir.Icmp("eq", divisor, ir.Const(0));
    const auto overflow = ir.Icmp("uge", hi, divisor);
    const auto bad = ir.BitOp("or", zero, overflow);
    ir.StoreRip(ir.Const(insn.Address));
    const auto ok = ir.NextLabel("div");
    ir.Line("br i1 " + bad + ", label %fault, label %" + ok);
    ir.Label(ok);
    if (width == 32) {
        const auto dividend = ir.Or(ir.Bin("shl", hi, ir.Const(32)), lo);
        locals.Write(0, Mask(ir, ir.Bin("udiv", dividend, divisor), 32));
        locals.Write(2, Mask(ir, ir.Bin("urem", dividend, divisor), 32));
        return;
    }
    const auto pair = ir.Temp();
    ir.Line(pair + " = call { i64, i64 } @anyps5_div64(i64 " + hi + ", i64 " + lo + ", i64 " + divisor + ")");
    const auto quot = ir.Temp();
    const auto rem = ir.Temp();
    ir.Line(quot + " = extractvalue { i64, i64 } " + pair + ", 0");
    ir.Line(rem + " = extractvalue { i64, i64 } " + pair + ", 1");
    locals.Write(0, quot);
    locals.Write(2, rem);
}

void EmitInsn(Ir& ir, Locals& locals, const WasmInsn& insn, bool deadFlags) {
    switch (insn.Op) {
    case WasmMnemonic::Nop:
        return;
    case WasmMnemonic::Mov:
    case WasmMnemonic::MovImm:
    case WasmMnemonic::Lea:
    case WasmMnemonic::Movsxd:
    case WasmMnemonic::Cdqe: {
        if (insn.Op == WasmMnemonic::Cdqe) {
            const auto rax = locals.Read(0);
            if (insn.Width == 64) {
                locals.Write(0, ir.Sext64(ir.Trunc32(rax)));
                return;
            }
            const auto bits = ir.Temp();
            ir.Line(bits + " = trunc i64 " + rax + " to i16");
            const auto wide = ir.Temp();
            ir.Line(wide + " = sext i16 " + bits + " to i32");
            locals.Write(0, ir.Zext64(wide));
            return;
        }
        if (insn.Op == WasmMnemonic::Movsxd) {
            auto value = LoadOperand(ir, locals, insn, insn.Src, 32);
            StoreOperand(ir, locals, insn, insn.Dst, ir.Sext64(ir.Trunc32(value)), 64);
            return;
        }
        auto value = insn.Op == WasmMnemonic::Lea ? AddressOf(ir, locals, insn, insn.Src) : LoadOperand(ir, locals, insn, insn.Src, insn.Width);
        value = Mask(ir, std::move(value), insn.Width);
        StoreOperand(ir, locals, insn, insn.Dst, value, insn.Width);
        return;
    }
    case WasmMnemonic::Shl:
    case WasmMnemonic::Shr:
    case WasmMnemonic::Sar:
        EmitShift(ir, locals, insn, deadFlags);
        return;
    case WasmMnemonic::Imul:
    case WasmMnemonic::Imul3:
        EmitImul(ir, locals, insn, deadFlags);
        return;
    case WasmMnemonic::Div:
        EmitDiv(ir, locals, insn);
        return;
    case WasmMnemonic::Setcc: {
        const auto bit = ir.Select(Condition(ir, insn.Cc, locals.ReadFlags()), ir.Const(1), ir.Const(0));
        StoreOperand(ir, locals, insn, insn.Dst, bit, 8);
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
        EmitAlu(ir, locals, insn, deadFlags);
        return;
    case WasmMnemonic::Push: {
        const auto value = LoadOperand(ir, locals, insn, insn.Src, 64);
        const auto rsp = locals.Read(kRsp);
        const auto slot = ir.Sub(rsp, ir.Const(8));
        StoreMem(ir, locals, slot, value, 64, insn.Address);
        locals.Write(kRsp, slot);
        return;
    }
    case WasmMnemonic::PushFlags: {
        const auto rsp = locals.Read(kRsp);
        const auto flags = locals.ReadFlags();
        const auto slot = ir.Sub(rsp, ir.Const(8));
        StoreMem(ir, locals, slot, flags, 64, insn.Address);
        locals.Write(kRsp, slot);
        return;
    }
    case WasmMnemonic::Pop: {
        const auto rsp = locals.Read(kRsp);
        const auto value = LoadMem(ir, locals, rsp, 64, insn.Address);
        locals.Write(kRsp, ir.Add(rsp, ir.Const(8)));
        StoreOperand(ir, locals, insn, insn.Dst, value, 64);
        return;
    }
    case WasmMnemonic::MovdToXmm:
    case WasmMnemonic::MovqToXmm:
    case WasmMnemonic::MovdFromXmm:
    case WasmMnemonic::MovqFromXmm:
    case WasmMnemonic::Pxor:
        EmitXmm(ir, locals, insn);
        return;
    case WasmMnemonic::Jmp:
    case WasmMnemonic::Jcc:
    case WasmMnemonic::Call:
    case WasmMnemonic::CallSlot:
    case WasmMnemonic::CallInd:
    case WasmMnemonic::JmpSlot:
    case WasmMnemonic::JmpInd:
    case WasmMnemonic::Ret:
    case WasmMnemonic::Halt:
        return;
    }
}

bool WritesFlags(const WasmInsn& insn) {
    if (insn.Op == WasmMnemonic::Shl || insn.Op == WasmMnemonic::Shr || insn.Op == WasmMnemonic::Sar) {
        if (insn.Src.Kind != WasmOperandKind::Imm) return false;
        const auto mask = insn.Width == 64 ? 63ull : 31ull;
        return (insn.Src.Imm & mask) != 0;
    }
    switch (insn.Op) {
    case WasmMnemonic::Add:
    case WasmMnemonic::Sub:
    case WasmMnemonic::Cmp:
    case WasmMnemonic::And:
    case WasmMnemonic::Or:
    case WasmMnemonic::Xor:
    case WasmMnemonic::Test:
    case WasmMnemonic::Inc:
    case WasmMnemonic::Dec:
    case WasmMnemonic::Imul:
    case WasmMnemonic::Imul3:
        return true;
    default:
        return false;
    }
}

int BlockAt(const std::vector<WasmBlock>& blocks, std::uint64_t address) {
    for (int index = 0; index < static_cast<int>(blocks.size()); ++index) {
        if (blocks[static_cast<std::size_t>(index)].Start == address) return index;
    }
    return -1;
}

int ImportAt(const WasmImage& image, std::uint64_t address) {
    for (int index = 0; index < static_cast<int>(image.Imports.size()); ++index) {
        if (image.Imports[static_cast<std::size_t>(index)].Entry == address) return index;
    }
    return -1;
}

std::string BlockLabel(std::uint64_t address) { return "g" + Hex(address); }

struct FlagPlan {
    std::vector<std::vector<char>> Dead;
};

FlagPlan BuildFlagPlan(const WasmImage& image, const std::vector<WasmBlock>& blocks) {
    const int count = static_cast<int>(blocks.size());
    std::vector<std::vector<int>> next(static_cast<std::size_t>(count));
    std::vector<char> read(static_cast<std::size_t>(count));
    std::vector<char> escape(static_cast<std::size_t>(count));
    std::vector<int> calls;
    for (int index = 0; index < count; ++index) {
        if (blocks[static_cast<std::size_t>(index)].EdgeKind == WasmBlock::Kind::Call) calls.push_back(index);
    }
    for (int index = 0; index < count; ++index) {
        const auto& block = blocks[static_cast<std::size_t>(index)];
        auto add = [&](std::uint64_t address) {
            const auto found = BlockAt(blocks, address);
            if (found >= 0) next[static_cast<std::size_t>(index)].push_back(found);
        };
        if (block.EdgeKind == WasmBlock::Kind::Halt) {
            read[static_cast<std::size_t>(index)] = 1;
        } else if (block.EdgeKind == WasmBlock::Kind::Branch) {
            read[static_cast<std::size_t>(index)] = 1;
            add(block.Target);
            add(block.Fall);
        } else if (block.EdgeKind == WasmBlock::Kind::Fall) {
            add(block.Fall);
        } else if (block.EdgeKind == WasmBlock::Kind::Jump) {
            if (ImportAt(image, block.Target) >= 0) escape[static_cast<std::size_t>(index)] = 1;
            else add(block.Target);
        } else if (block.EdgeKind == WasmBlock::Kind::Call) {
            if (ImportAt(image, block.Target) >= 0) add(block.Fall);
            else if (BlockAt(blocks, block.Target) >= 0) add(block.Target);
            else escape[static_cast<std::size_t>(index)] = 1;
        } else if (block.EdgeKind == WasmBlock::Kind::IndirectCall) {
            escape[static_cast<std::size_t>(index)] = 1;
            add(block.Fall);
        } else if (block.EdgeKind == WasmBlock::Kind::IndirectJump) {
            escape[static_cast<std::size_t>(index)] = 1;
        } else {
            for (const auto call : calls) add(blocks[static_cast<std::size_t>(call)].Fall);
            if (calls.empty()) escape[static_cast<std::size_t>(index)] = 1;
        }
    }
    std::vector<char> live(static_cast<std::size_t>(count));
    FlagPlan plan;
    plan.Dead.resize(static_cast<std::size_t>(count));
    bool changed = true;
    while (changed) {
        changed = false;
        for (int index = count - 1; index >= 0; --index) {
            bool out = read[static_cast<std::size_t>(index)] != 0 || escape[static_cast<std::size_t>(index)] != 0;
            for (const auto successor : next[static_cast<std::size_t>(index)]) {
                if (live[static_cast<std::size_t>(successor)] != 0) out = true;
            }
            bool current = out;
            const auto& insns = blocks[static_cast<std::size_t>(index)].Insns;
            std::vector<char> dead(insns.size());
            for (int insn = static_cast<int>(insns.size()) - 1; insn >= 0; --insn) {
                const auto& instruction = insns[static_cast<std::size_t>(insn)];
                if (instruction.Op == WasmMnemonic::PushFlags) current = true;
                if (WritesFlags(instruction)) {
                    dead[static_cast<std::size_t>(insn)] = current ? 0 : 1;
                    if (instruction.Op != WasmMnemonic::Inc && instruction.Op != WasmMnemonic::Dec) current = false;
                }
            }
            if (live[static_cast<std::size_t>(index)] != static_cast<char>(current)) {
                live[static_cast<std::size_t>(index)] = static_cast<char>(current);
                changed = true;
            }
            plan.Dead[static_cast<std::size_t>(index)] = std::move(dead);
        }
    }
    return plan;
}

std::vector<std::uint64_t> CollectReturns(const std::vector<WasmBlock>& blocks) {
    std::vector<std::uint64_t> returns;
    for (const auto& block : blocks) {
        if ((block.EdgeKind != WasmBlock::Kind::Call && block.EdgeKind != WasmBlock::Kind::IndirectCall) || BlockAt(blocks, block.Fall) < 0) continue;
        bool seen = false;
        for (const auto address : returns) seen = seen || address == block.Fall;
        if (!seen) returns.push_back(block.Fall);
    }
    return returns;
}

void EmitTick(Ir& ir) {
    const auto steps = ir.Temp();
    ir.Line(steps + " = load i32, ptr @anyps5_steps, align 4");
    const auto stop = ir.Icmp("uge", steps, std::to_string(kStepLimit), "i32");
    const auto body = ir.NextLabel("run");
    ir.Line("br i1 " + stop + ", label %limit, label %" + body);
    ir.Label(body);
    const auto advanced = ir.Temp();
    ir.Line(advanced + " = add i32 " + steps + ", 1");
    ir.Line("store i32 " + advanced + ", ptr @anyps5_steps, align 4");
}

void EmitImport(Ir& ir, const WasmImage& image, int import, std::uint64_t fall, bool resume) {
    ir.StoreRip(ir.Const(image.Imports[static_cast<std::size_t>(import)].Entry));
    const auto status = ir.Temp();
    ir.Line(status + " = call i32 @anyps5_import_" + std::to_string(image.Imports[static_cast<std::size_t>(import)].Kind) + "()");
    const auto bad = ir.Icmp("ne", status, "0", "i32");
    if (!resume) {
        ir.Line("br i1 " + bad + ", label %fault, label %dispatch");
        return;
    }
    const auto back = ir.NextLabel("back");
    ir.Line("br i1 " + bad + ", label %fault, label %" + back);
    ir.Label(back);
    const auto rip = ir.Load("i64", ir.StatePtr(kRipOffset));
    const auto same = ir.Icmp("eq", rip, ir.Const(fall));
    ir.Line("br i1 " + same + ", label %" + BlockLabel(fall) + ", label %dispatch");
}

void EmitEdge(Ir& ir, Locals& locals, const WasmImage& image, const WasmBlock& block, const std::vector<WasmBlock>& blocks, const std::vector<std::uint64_t>& returns) {
    auto known = [&](std::uint64_t address) { return BlockAt(blocks, address) >= 0; };
    if (block.EdgeKind == WasmBlock::Kind::Halt) {
        locals.Spill();
        ir.StoreRip(ir.Const(block.Edge));
        ir.Branch("halt");
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::Fall || block.EdgeKind == WasmBlock::Kind::Jump) {
        const auto target = block.EdgeKind == WasmBlock::Kind::Jump ? block.Target : block.Fall;
        locals.Spill();
        const auto import = ImportAt(image, target);
        if (import >= 0) {
            EmitImport(ir, image, import, 0, false);
            return;
        }
        if (known(target)) ir.Branch(BlockLabel(target));
        else {
            ir.StoreRip(ir.Const(target));
            ir.Branch("dispatch");
        }
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::Branch) {
        const auto cond = Condition(ir, block.Cc, locals.ReadFlags());
        locals.Spill();
        if (known(block.Target) && known(block.Fall)) {
            if (block.Target == block.Fall) ir.Branch(BlockLabel(block.Target));
            else ir.Line("br i1 " + cond + ", label %" + BlockLabel(block.Target) + ", label %" + BlockLabel(block.Fall));
            return;
        }
        ir.StoreRip(ir.Select(cond, ir.Const(block.Target), ir.Const(block.Fall)));
        ir.Branch("dispatch");
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::Call) {
        const auto rsp = locals.Read(kRsp);
        const auto slot = ir.Sub(rsp, ir.Const(8));
        StoreMem(ir, locals, slot, ir.Const(block.Fall), 64, block.Edge);
        locals.Write(kRsp, slot);
        locals.Spill();
        const auto import = ImportAt(image, block.Target);
        if (import >= 0) {
            EmitImport(ir, image, import, block.Fall, known(block.Fall));
            return;
        }
        if (known(block.Target)) {
            ir.Branch(BlockLabel(block.Target));
            return;
        }
        ir.StoreRip(ir.Const(block.Target));
        ir.Branch("dispatch");
        return;
    }
    if (block.EdgeKind == WasmBlock::Kind::IndirectCall || block.EdgeKind == WasmBlock::Kind::IndirectJump) {
        const auto address = LoadOperand(ir, locals, block.EdgeInsn, block.EdgeInsn.Src, 64);
        if (block.EdgeKind == WasmBlock::Kind::IndirectCall) {
            const auto rsp = locals.Read(kRsp);
            const auto slot = ir.Sub(rsp, ir.Const(8));
            StoreMem(ir, locals, slot, ir.Const(block.Fall), 64, block.Edge);
            locals.Write(kRsp, slot);
        }
        locals.Spill();
        ir.StoreRip(address);
        ir.Branch("dispatch");
        return;
    }
    const auto rsp = locals.Read(kRsp);
    const auto next = LoadMem(ir, locals, rsp, 64, block.Edge);
    locals.Write(kRsp, ir.Add(rsp, ir.Const(8)));
    locals.Spill();
    if (returns.empty()) {
        ir.StoreRip(next);
        ir.Branch("dispatch");
        return;
    }
    const auto slow = ir.NextLabel("ret");
    ir.Text += "  switch i64 " + next + ", label %" + slow + " [\n";
    for (const auto address : returns) ir.Text += "    i64 " + Number(address) + ", label %" + BlockLabel(address) + "\n";
    ir.Text += "  ]\n";
    ir.Label(slow);
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
    text += "define internal i32 @anyps5_read8(i32 %wasm) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  %value = load i8, ptr %ptr, align 1\n";
    text += "  %wide = zext i8 %value to i32\n";
    text += "  ret i32 %wide\n}\n";
    text += "define internal void @anyps5_write32(i32 %wasm, i32 %value) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  store i32 %value, ptr %ptr, align 1\n";
    text += "  ret void\n}\n";
    text += "define internal void @anyps5_write64(i32 %wasm, i64 %value) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  store i64 %value, ptr %ptr, align 1\n";
    text += "  ret void\n}\n";
    text += "define internal void @anyps5_write8(i32 %wasm, i32 %value) {\n";
    text += "  %ptr = inttoptr i32 %wasm to ptr\n";
    text += "  %byte = trunc i32 %value to i8\n";
    text += "  store i8 %byte, ptr %ptr, align 1\n";
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
    module += kShiftFlags;
    module += kDiv;
    module += EmitWasmImports(image);
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
    if (WasmImportsUse(image, WasmImportKind::ProcessTime)) {
        const auto now = ir.Temp();
        ir.Line(now + " = call i64 @anyps5_host_monotonic_us()");
        ir.Line("store i64 " + now + ", ptr @anyps5_time_origin, align 8");
    }
    ir.Line("store i32 0, ptr @anyps5_steps, align 4");
    if (BlockAt(blocks, image.Entry) >= 0) ir.Branch(BlockLabel(image.Entry));
    else ir.Branch("dispatch");
    ir.Label("dispatch");
    const auto rip = ir.Load("i64", ir.StatePtr(kRipOffset));
    ir.Text += "  switch i64 " + rip + ", label %missing [\n";
    for (const auto& block : blocks) ir.Text += "    i64 " + Number(block.Start) + ", label %" + BlockLabel(block.Start) + "\n";
    for (std::size_t index = 0; index < image.Imports.size(); ++index) ir.Text += "    i64 " + Number(image.Imports[index].Entry) + ", label %i" + std::to_string(index) + "\n";
    ir.Text += "  ]\n";
    const auto flags = BuildFlagPlan(image, blocks);
    const auto returns = CollectReturns(blocks);
    for (int index = 0; index < static_cast<int>(blocks.size()); ++index) {
        const auto& block = blocks[static_cast<std::size_t>(index)];
        ir.Label(BlockLabel(block.Start));
        EmitTick(ir);
        Locals locals(ir);
        for (std::size_t insn = 0; insn < block.Insns.size(); ++insn) {
            EmitInsn(ir, locals, block.Insns[insn], flags.Dead[static_cast<std::size_t>(index)][insn] != 0);
        }
        EmitEdge(ir, locals, image, block, blocks, returns);
    }
    for (std::size_t index = 0; index < image.Imports.size(); ++index) {
        ir.Label("i" + std::to_string(index));
        EmitTick(ir);
        const auto status = ir.Temp();
        ir.Line(status + " = call i32 @anyps5_import_" + std::to_string(image.Imports[index].Kind) + "()");
        const auto bad = ir.Icmp("ne", status, "0", "i32");
        ir.Line("br i1 " + bad + ", label %fault, label %dispatch");
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
