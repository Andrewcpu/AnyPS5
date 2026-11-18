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
        if ((segment.Flags & 1u) == 0) continue;
        if (address < segment.Virtual || address >= segment.Virtual + segment.Bytes.size()) continue;
        const auto offset = static_cast<std::size_t>(address - segment.Virtual);
        return {segment.Bytes.data() + offset, segment.Bytes.size() - offset};
    }
    Fail("control flow leaves executable bytes");
    return {};
}

void AddTarget(std::set<std::uint64_t>& extra, std::uint64_t target) { extra.insert(target); }

}

std::vector<WasmBlock> DiscoverWasmBlocks(const WasmImage& image) {
    std::set<std::uint64_t> starts;
    starts.insert(image.Entry);
    std::vector<WasmBlock> blocks;
    bool grew = true;
    while (grew) {
        grew = false;
        if (starts.size() > 4096) Fail("too many basic blocks");
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
                    block.Target = insn.Target;
                    block.Fall = insn.Next;
                    if (insn.Op == WasmMnemonic::Jmp) {
                        block.EdgeKind = WasmBlock::Kind::Jump;
                        AddTarget(extra, insn.Target);
                    } else if (insn.Op == WasmMnemonic::Jcc) {
                        block.EdgeKind = WasmBlock::Kind::Branch;
                        AddTarget(extra, insn.Target);
                        AddTarget(extra, insn.Next);
                    } else if (insn.Op == WasmMnemonic::Call) {
                        block.EdgeKind = WasmBlock::Kind::Call;
                        AddTarget(extra, insn.Target);
                        AddTarget(extra, insn.Next);
                    } else if (insn.Op == WasmMnemonic::Ret) {
                        block.EdgeKind = WasmBlock::Kind::Return;
                    } else {
                        block.EdgeKind = WasmBlock::Kind::Halt;
                    }
                    break;
                }
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
