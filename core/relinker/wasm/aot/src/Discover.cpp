#include <wasm/Discover.hpp>
#include <domain/Types.hpp>
#include <set>
#include <string>

namespace Relinker::Wasm {
namespace {

[[noreturn]] void Fail(const std::string& message) {
    throw Domain::RelinkerException("wasm lift: " + message);
}

struct Span {
    const std::uint8_t* Data = nullptr;
    std::size_t Size = 0;
};

Span ExecutableBytes(const WasmImage& image, std::uint64_t address) {
    for (const auto& segment : image.Segments) {
        if ((segment.Flags & 1u) == 0 || segment.FileSize == 0) continue;
        if (address < segment.Virtual || address >= segment.Virtual + segment.FileSize) continue;
        const auto offset = static_cast<std::size_t>(address - segment.Virtual);
        return {segment.Bytes.data() + offset, static_cast<std::size_t>(segment.FileSize - offset)};
    }
    Fail("control flow leaves executable bytes");
    return {};
}

bool ImportEntry(const WasmImage& image, std::uint64_t address) {
    for (const auto& import : image.Imports) if (import.Entry == address) return true;
    return false;
}

bool Executable(const WasmImage& image, std::uint64_t address) {
    for (const auto& segment : image.Segments) {
        if ((segment.Flags & 1u) == 0 || segment.FileSize == 0) continue;
        if (address >= segment.Virtual && address < segment.Virtual + segment.FileSize) return true;
    }
    return false;
}

std::uint64_t ReadQword(const WasmImage& image, std::uint64_t address) {
    for (const auto& segment : image.Segments) {
        if (segment.MemorySize < 8 || address < segment.Virtual) continue;
        const auto delta = address - segment.Virtual;
        if (delta > segment.MemorySize - 8) continue;
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(segment.Bytes[static_cast<std::size_t>(delta) + static_cast<std::size_t>(index)]) << (8 * index);
        return value;
    }
    Fail("indirect branch target is not an import");
    return 0;
}

std::uint64_t ResolveSlot(const WasmImage& image, const WasmInsn& insn) {
    if (!insn.Src.Rip) Fail("indirect branch is not lifted");
    const auto slot = insn.Next + static_cast<std::uint64_t>(insn.Src.Disp);
    const auto value = ReadQword(image, slot);
    if (ImportEntry(image, value) || Executable(image, value)) return value;
    Fail("indirect branch target is not an import");
    return 0;
}

void AddTarget(const WasmImage& image, std::set<std::uint64_t>& extra, std::uint64_t target) {
    if (ImportEntry(image, target)) return;
    extra.insert(target);
}

}

std::vector<WasmBlock> DiscoverWasmBlocks(const WasmImage& image) {
    std::set<std::uint64_t> starts;
    starts.insert(image.Entry);
    for (const auto root : image.CodeRoots) {
        if (Executable(image, root)) starts.insert(root);
    }
    std::vector<WasmBlock> blocks;
    bool grew = true;
    while (grew) {
        grew = false;
        if (starts.size() > 65536) Fail("too many basic blocks");
        blocks.clear();
        std::set<std::uint64_t> extra;
        for (const auto start : starts) {
            WasmBlock block;
            block.Start = start;
            auto address = start;
            while (true) {
                if (address != start && starts.contains(address)) {
                    block.EdgeKind = WasmBlock::Kind::Fall;
                    block.Edge = address;
                    block.Fall = address;
                    break;
                }
                const auto span = ExecutableBytes(image, address);
                const auto insn = DecodeWasmInsn(span.Data, span.Size, address);
                for (const auto mark : starts) {
                    if (mark > address && mark < address + insn.Length) Fail("branch enters an instruction");
                }
                if (WasmTerminates(insn.Op)) {
                    block.Edge = insn.Address;
                    block.Cc = insn.Cc;
                    block.EdgeInsn = insn;
                    block.Target = insn.Op == WasmMnemonic::CallSlot || insn.Op == WasmMnemonic::JmpSlot ? ResolveSlot(image, insn) : insn.Target;
                    block.Fall = insn.Next;
                    if (insn.Op == WasmMnemonic::Jmp || insn.Op == WasmMnemonic::JmpSlot) {
                        block.EdgeKind = WasmBlock::Kind::Jump;
                        AddTarget(image, extra, block.Target);
                    } else if (insn.Op == WasmMnemonic::Jcc) {
                        block.EdgeKind = WasmBlock::Kind::Branch;
                        AddTarget(image, extra, insn.Target);
                        AddTarget(image, extra, insn.Next);
                    } else if (insn.Op == WasmMnemonic::Call || insn.Op == WasmMnemonic::CallSlot) {
                        block.EdgeKind = WasmBlock::Kind::Call;
                        AddTarget(image, extra, block.Target);
                        AddTarget(image, extra, insn.Next);
                    } else if (insn.Op == WasmMnemonic::CallInd) {
                        block.EdgeKind = WasmBlock::Kind::IndirectCall;
                        AddTarget(image, extra, insn.Next);
                    } else if (insn.Op == WasmMnemonic::JmpInd) {
                        block.EdgeKind = WasmBlock::Kind::IndirectJump;
                    } else if (insn.Op == WasmMnemonic::Ret) {
                        block.EdgeKind = WasmBlock::Kind::Return;
                    } else {
                        block.EdgeKind = WasmBlock::Kind::Halt;
                    }
                    break;
                }
                if (insn.Op == WasmMnemonic::Lea && insn.Src.Rip && Executable(image, insn.Next + static_cast<std::uint64_t>(insn.Src.Disp))) AddTarget(image, extra, insn.Next + static_cast<std::uint64_t>(insn.Src.Disp));
                block.Insns.push_back(insn);
                if (block.Insns.size() > 4096) Fail("basic block is too long");
                address = insn.Next;
            }
            blocks.push_back(std::move(block));
        }
        for (const auto target : extra) {
            if (starts.insert(target).second) grew = true;
        }
    }
    return blocks;
}

}
