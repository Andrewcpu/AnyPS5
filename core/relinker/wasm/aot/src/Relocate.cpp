#include <wasm/WasmTarget.hpp>
#include <domain/Types.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

bool Fits(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

bool FitsI32(std::uint64_t value) {
    const auto truncated = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(truncated)) == value;
}

bool IsTls(std::uint32_t type) {
    return (type >= 16 && type <= 23) || type == 34 || type == 35 || type == 36;
}

struct Symbol {
    std::uint64_t Value = 0;
    std::uint16_t Index = 0;
    std::uint8_t Info = 0;
};

struct Located {
    std::uint64_t Offset = 0;
    std::uint64_t Available = 0;
    bool Found = false;
};

Located Locate(const WasmImage& image, std::uint64_t address) {
    for (const auto& segment : image.Segments) {
        if (segment.FileSize == 0 || address < segment.Virtual || address - segment.Virtual >= segment.FileSize) continue;
        const auto delta = address - segment.Virtual;
        Located located;
        located.Found = true;
        located.Offset = segment.FileOffset + delta;
        located.Available = segment.FileSize - delta;
        return located;
    }
    return {};
}

Symbol ReadSymbol(const std::vector<std::uint8_t>& elf, std::uint64_t offset, std::uint64_t size, std::uint64_t entsize, std::uint64_t index) {
    if (entsize == 0) entsize = 24;
    if (entsize < 24) Fail("wasm: symbol entry is too small");
    if (size / entsize <= index) Fail("wasm: relocation symbol index is outside the symbol table");
    const auto at = offset + index * entsize;
    if (!Fits(at, 24, elf.size())) Fail("wasm: symbol table is outside the file");
    Symbol symbol;
    symbol.Info = elf[static_cast<std::size_t>(at + 4)];
    symbol.Index = Read16(elf, static_cast<std::size_t>(at + 6));
    symbol.Value = Read64(elf, static_cast<std::size_t>(at + 8));
    return symbol;
}

std::uint64_t SymbolValue(const Symbol& symbol) {
    const unsigned bind = symbol.Info >> 4;
    if (symbol.Index == 0) {
        if (bind == 2) return 0;
        Fail("wasm: undefined relocation symbol");
    }
    if (symbol.Index == 0xfff2 || (symbol.Index >= 0xff00 && symbol.Index != 0xfff1)) Fail("wasm: unsupported relocation symbol section");
    return symbol.Value;
}

void Write(WasmImage& image, std::uint64_t address, std::uint64_t value, int size, std::vector<std::pair<std::uint64_t, std::uint64_t>>& used) {
    if (size <= 0) return;
    const auto bytes = static_cast<std::uint64_t>(size);
    if (bytes > ~address) Fail("wasm: relocation address wraps");
    const auto end = address + bytes;
    for (const auto& range : used) {
        if (address < range.second && range.first < end) Fail("wasm: overlapping relocations");
    }
    for (auto& segment : image.Segments) {
        if (segment.MemorySize == 0 || address < segment.Virtual) continue;
        const auto offset = address - segment.Virtual;
        if (offset >= segment.MemorySize || bytes > segment.MemorySize - offset) continue;
        for (int index = 0; index < size; ++index) segment.Bytes[static_cast<std::size_t>(offset) + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value >> (8 * index));
        if (offset + bytes > segment.ContentSize) segment.ContentSize = offset + bytes;
        used.emplace_back(address, end);
        return;
    }
    Fail("wasm: relocation is outside a load segment");
}

void ApplyOne(WasmImage& image, std::uint64_t place, std::uint32_t type, std::int64_t addend, const Symbol* symbol, std::vector<std::pair<std::uint64_t, std::uint64_t>>& used) {
    if (type == 0) return;
    if (IsTls(type)) Fail("wasm: TLS relocation is not supported");
    if (type == 3 || type == 9 || type == 25 || type == 26 || type == 41 || type == 42) Fail("wasm: unsupported relocation type");
    const auto added = [&](std::uint64_t base) { return base + static_cast<std::uint64_t>(addend); };
    if (type == 8) {
        Write(image, place, static_cast<std::uint64_t>(addend), 8, used);
        return;
    }
    if (symbol == nullptr) Fail("wasm: relocation symbol table is missing");
    if (type == 6 || type == 7) {
        if (addend != 0) Fail("wasm: nonzero GLOB_DAT or JUMP_SLOT addend");
        Write(image, place, SymbolValue(*symbol), 8, used);
        return;
    }
    if (type == 1) {
        Write(image, place, added(SymbolValue(*symbol)), 8, used);
        return;
    }
    if (type == 10) {
        const auto value = added(SymbolValue(*symbol));
        if (value > 0xffffffffu) Fail("wasm: relocation overflows");
        Write(image, place, value, 4, used);
        return;
    }
    if (type == 11) {
        const auto value = added(SymbolValue(*symbol));
        if (!FitsI32(value)) Fail("wasm: relocation overflows");
        Write(image, place, value, 4, used);
        return;
    }
    if (type == 2 || type == 4 || type == 24) {
        const auto value = added(SymbolValue(*symbol)) - place;
        if (type != 24 && !FitsI32(value)) Fail("wasm: relocation overflows");
        Write(image, place, value, type == 24 ? 8 : 4, used);
        return;
    }
    Fail("wasm: unsupported relocation type");
}

struct Section {
    std::uint32_t Type = 0;
    std::uint32_t Link = 0;
    std::uint64_t Offset = 0;
    std::uint64_t Size = 0;
    std::uint64_t Entsize = 0;
};

std::vector<Section> Sections(const std::vector<std::uint8_t>& elf) {
    const auto shoff = Read64(elf, 40);
    const auto shentsize = Read16(elf, 58);
    const auto shnum = Read16(elf, 60);
    if (shnum == 0xffff) Fail("wasm: extended section numbering is not supported");
    if (shoff == 0 || shnum == 0) return {};
    if (shentsize != 64) Fail("wasm: unexpected section header size");
    if (!Fits(shoff, static_cast<std::uint64_t>(shnum) * shentsize, elf.size())) Fail("wasm: section headers are outside the file");
    std::vector<Section> sections(shnum);
    for (std::uint16_t index = 0; index < shnum; ++index) {
        const auto header = static_cast<std::size_t>(shoff) + static_cast<std::size_t>(index) * shentsize;
        Section section;
        section.Type = Read32(elf, header + 4);
        section.Offset = Read64(elf, header + 24);
        section.Size = Read64(elf, header + 32);
        section.Link = Read32(elf, header + 40);
        section.Entsize = Read64(elf, header + 56);
        if (section.Size != 0 && !Fits(section.Offset, section.Size, elf.size())) Fail("wasm: section is outside the file");
        sections[index] = section;
    }
    return sections;
}

void ApplyTable(const std::vector<std::uint8_t>& elf, WasmImage& image, std::uint64_t offset, std::uint64_t size, std::uint64_t symbolOffset, std::uint64_t symbolSize, std::uint64_t symbolEntsize, bool haveSymbols, std::vector<std::pair<std::uint64_t, std::uint64_t>>& used) {
    if (size % 24 != 0) Fail("wasm: relocation table size is not a multiple of the entry");
    for (std::uint64_t cursor = 0; cursor < size; cursor += 24) {
        const auto at = offset + cursor;
        const auto place = Read64(elf, static_cast<std::size_t>(at));
        const auto info = Read64(elf, static_cast<std::size_t>(at + 8));
        const auto addend = static_cast<std::int64_t>(Read64(elf, static_cast<std::size_t>(at + 16)));
        const auto type = static_cast<std::uint32_t>(info);
        const auto index = info >> 32;
        Symbol symbol;
        const Symbol* resolved = nullptr;
        if (type != 0 && type != 8) {
            if (!haveSymbols) Fail("wasm: relocation symbol table is missing");
            symbol = ReadSymbol(elf, symbolOffset, symbolSize, symbolEntsize, index);
            resolved = &symbol;
        }
        ApplyOne(image, place, type, addend, resolved, used);
    }
}

bool ApplySections(const std::vector<std::uint8_t>& elf, WasmImage& image, const std::vector<Section>& sections, std::vector<std::pair<std::uint64_t, std::uint64_t>>& used) {
    bool saw = false;
    for (const auto& section : sections) {
        if (section.Type == 9 && section.Size != 0) Fail("wasm: REL relocations are not supported");
        if (section.Type != 4 || section.Size == 0) continue;
        if (section.Entsize != 0 && section.Entsize != 24) Fail("wasm: unexpected relocation entry size");
        if (section.Link >= sections.size()) Fail("wasm: relocation symbol table is missing");
        const auto& symbols = sections[section.Link];
        if (symbols.Type != 2 && symbols.Type != 11) Fail("wasm: relocation symbol table has an unexpected type");
        if (symbols.Entsize != 0 && symbols.Entsize < 24) Fail("wasm: symbol entry is too small");
        ApplyTable(elf, image, section.Offset, section.Size, symbols.Offset, symbols.Size, symbols.Entsize == 0 ? 24 : symbols.Entsize, true, used);
        saw = true;
    }
    return saw;
}

void ApplyDynamic(const std::vector<std::uint8_t>& elf, WasmImage& image, std::vector<std::pair<std::uint64_t, std::uint64_t>>& used) {
    const auto phoff = Read64(elf, 32);
    const auto phentsize = Read16(elf, 54);
    const auto phnum = Read16(elf, 56);
    bool saw = false;
    std::uint64_t rela = 0;
    std::uint64_t relasz = 0;
    std::uint64_t relaent = 24;
    std::uint64_t symtab = 0;
    std::uint64_t syment = 24;
    std::uint64_t jmprel = 0;
    std::uint64_t pltrelsz = 0;
    std::int64_t pltrel = 0;
    bool haveRela = false;
    bool haveSymtab = false;
    bool haveJmp = false;
    for (std::uint16_t index = 0; index < phnum; ++index) {
        const auto header = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(index) * phentsize;
        if (Read32(elf, header) != 2) continue;
        if (saw) Fail("wasm: multiple PT_DYNAMIC segments");
        saw = true;
        const auto offset = Read64(elf, header + 8);
        const auto size = Read64(elf, header + 32);
        if (size % 16 != 0 || !Fits(offset, size, elf.size())) Fail("wasm: dynamic segment is truncated");
        bool terminated = false;
        for (std::uint64_t cursor = 0; cursor < size; cursor += 16) {
            const auto tag = static_cast<std::int64_t>(Read64(elf, static_cast<std::size_t>(offset + cursor)));
            const auto value = Read64(elf, static_cast<std::size_t>(offset + cursor + 8));
            if (tag == 0) {
                terminated = true;
                break;
            }
            if (tag == 6) {
                symtab = value;
                haveSymtab = true;
            } else if (tag == 7) {
                rela = value;
                haveRela = true;
            } else if (tag == 8) relasz = value;
            else if (tag == 9) relaent = value;
            else if (tag == 2) pltrelsz = value;
            else if (tag == 11) syment = value;
            else if (tag == 18) {
                if (value != 0) Fail("wasm: REL relocations are not supported");
            } else if (tag == 20) pltrel = static_cast<std::int64_t>(value);
            else if (tag == 23) {
                jmprel = value;
                haveJmp = true;
            }
        }
        if (!terminated) Fail("wasm: dynamic segment is not terminated");
    }
    if ((!haveRela || relasz == 0) && (!haveJmp || pltrelsz == 0)) return;
    if (haveRela && relasz != 0 && relaent != 24) Fail("wasm: unexpected relocation entry size");
    std::uint64_t symbolOffset = 0;
    std::uint64_t symbolSize = 0;
    if (haveSymtab) {
        const auto symbols = Locate(image, symtab);
        if (!symbols.Found) Fail("wasm: dynamic symbol table is outside a load segment");
        symbolOffset = symbols.Offset;
        symbolSize = symbols.Available;
    }
    if (haveRela && relasz != 0) {
        const auto table = Locate(image, rela);
        if (!table.Found || relasz > table.Available) Fail("wasm: dynamic relocation table is outside a load segment");
        ApplyTable(elf, image, table.Offset, relasz, symbolOffset, symbolSize, syment, haveSymtab, used);
    }
    if (!haveJmp || pltrelsz == 0) return;
    if (pltrel != 7) Fail("wasm: REL relocations are not supported");
    const auto jumps = Locate(image, jmprel);
    if (!jumps.Found || pltrelsz > jumps.Available) Fail("wasm: dynamic relocation table is outside a load segment");
    ApplyTable(elf, image, jumps.Offset, pltrelsz, symbolOffset, symbolSize, syment, haveSymtab, used);
}

}

void ApplyWasmRelocations(const std::vector<std::uint8_t>& elf, WasmImage& image) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> used;
    const auto sections = Sections(elf);
    if (ApplySections(elf, image, sections, used)) return;
    ApplyDynamic(elf, image, used);
}

}
