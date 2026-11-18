#include <wasm/WasmTarget.hpp>
#include <domain/Types.hpp>
#include <cstdint>
#include <string>

namespace Relinker::Wasm {
namespace {

[[noreturn]] void Fail(const std::string& message) {
    throw Domain::RelinkerException(message);
}

std::uint16_t Read16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) Fail("wasm: truncated ELF");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t Read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) Fail("wasm: truncated ELF");
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
    return value;
}

std::uint64_t Read64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 8 > bytes.size()) Fail("wasm: truncated ELF");
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
    return value;
}

bool Overlaps(std::uint64_t first, std::uint64_t firstSize, std::uint64_t second, std::uint64_t secondSize) {
    if (firstSize == 0 || secondSize == 0) return false;
    if (first + firstSize < first || second + secondSize < second) return true;
    return first < second + secondSize && second < first + firstSize;
}

}
WasmImage PlanWasmImage(const std::vector<std::uint8_t>& elf) {
    if (elf.size() < 64 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') Fail("wasm: not an ELF");
    if (elf[4] != 2 || elf[5] != 1 || elf[6] != 1) Fail("wasm: ELF is not 64-bit little-endian");
    const auto type = Read16(elf, 16);
    if (type != 2 && type != 3) Fail("wasm: ELF type is not EXEC or DYN");
    if (Read16(elf, 18) != 62) Fail("wasm: ELF machine is not x86-64");
    if (Read32(elf, 20) != 1) Fail("wasm: unsupported ELF version");
    const auto entry = Read64(elf, 24);
    const auto phoff = Read64(elf, 32);
    const auto ehsize = Read16(elf, 52);
    const auto phentsize = Read16(elf, 54);
    const auto phnum = Read16(elf, 56);
    if (ehsize != 64 || phentsize != 56) Fail("wasm: unexpected ELF header sizes");
    if (phnum == 0 || phnum == 0xffff) Fail("wasm: unsupported program header count");
    if (phoff > elf.size() || static_cast<std::uint64_t>(phnum) * phentsize > elf.size() - phoff) Fail("wasm: program headers are outside the file");

    WasmImage image;
    image.Entry = entry;
    bool sawEntry = false;
    for (std::uint16_t index = 0; index < phnum; ++index) {
        const auto header = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(index) * phentsize;
        const auto kind = Read32(elf, header);
        if (kind != 1) continue;
        const auto flags = Read32(elf, header + 4);
        const auto offset = Read64(elf, header + 8);
        const auto virtualAddress = Read64(elf, header + 16);
        const auto fileSize = Read64(elf, header + 32);
        const auto memorySize = Read64(elf, header + 40);
        if (fileSize > memorySize) Fail("wasm: PT_LOAD file size exceeds memory size");
        if (memorySize > 0 && virtualAddress + memorySize < virtualAddress) Fail("wasm: PT_LOAD address wraps");
        if (offset > elf.size() || fileSize > elf.size() - offset) Fail("wasm: PT_LOAD is outside the file");
        if (virtualAddress + memorySize > kStateAddress) Fail("wasm: load segment does not fit below the CPU state page");
        for (const auto& existing : image.Segments) {
            if (Overlaps(existing.Virtual, existing.MemorySize, virtualAddress, memorySize)) Fail("wasm: overlapping PT_LOAD segments");
        }
        WasmSegment segment;
        segment.Virtual = virtualAddress;
        segment.MemorySize = memorySize;
        segment.Flags = flags;
        segment.Bytes.assign(elf.begin() + static_cast<std::ptrdiff_t>(offset), elf.begin() + static_cast<std::ptrdiff_t>(offset + fileSize));
        if ((flags & 1u) != 0 && entry >= virtualAddress && entry < virtualAddress + fileSize) sawEntry = true;
        image.Segments.push_back(std::move(segment));
    }
    if (image.Segments.empty()) Fail("wasm: ELF has no PT_LOAD segment");
    if (!sawEntry) Fail("wasm: entry point is not in an executable PT_LOAD");

    std::uint64_t lowest = image.Segments.front().Virtual;
    std::uint64_t end = 0;
    for (const auto& segment : image.Segments) {
        if (segment.Virtual < lowest) lowest = segment.Virtual;
        const auto segmentEnd = segment.Virtual + segment.MemorySize;
        if (segmentEnd > end) end = segmentEnd;
    }
    const auto stackBase = (end + 0xfff) & ~std::uint64_t{0xfff};
    if (stackBase < end) Fail("wasm: stack address wraps");
    const auto stackTop = stackBase + kStackBytes;
    if (stackTop < stackBase || stackTop > kStateAddress) Fail("wasm: guest image and stack do not fit below the CPU state page");
    image.GuestBase = lowest;
    image.StackBase = stackBase;
    image.StackTop = stackTop;
    image.GuestLimit = stackTop;
    return image;
}

}
