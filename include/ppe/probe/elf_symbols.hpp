// elf_symbols.hpp -- read function symbols out of an ELF file.
//
// WHY NOT dladdr. dladdr resolves only DYNAMIC symbols, so it names functions a
// shared library exported and nothing else. Profiling a normal executable with
// it symbolizes 0% of samples -- measured, on the first version of the sampler,
// which reported every sample as "(no symbol)" and merged them into one site.
// The static functions doing the work are in .symtab, which dladdr never reads.
//
// WHY MERGING BY PAGE IS NOT A SUBSTITUTE. That first version fell back to
// grouping unresolved addresses by 4 KiB page. Two functions with a deliberate
// 4:1 work ratio landed in the same page and merged into a single entry, so the
// profile was one line and the ratio it existed to show was invisible. Coarse
// attribution is not degraded symbolization; it is a different and wrong answer.
//
// LOAD BIAS IS THE PART THAT IS EASY TO GET WRONG. A symbol's st_value is a
// virtual address in the ELF's own space. A PIE executable or shared library is
// mapped somewhere else entirely, so resolving a runtime address means undoing
// that displacement. The bias is derived from the PT_LOAD segment covering the
// mapping's file offset -- not assumed to be `start - file_offset`, which is
// only correct when the segment happens to begin at vaddr 0.
//
// SCOPE: 64-bit ELF, the host's endianness, .symtab preferred with .dynsym as
// fallback, STT_FUNC symbols only. A stripped binary yields nothing, which is
// reported as such rather than as an empty profile.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#  include <elf.h>
#endif

namespace ppe::probe {

struct elf_symbol {
    std::uint64_t value = 0;   ///< address in the ELF's own virtual space
    std::uint64_t size = 0;
    std::string name;
};

/// Function symbols of an ELF file, sorted by address.
struct elf_symbol_table {
    bool ok = false;
    std::string note;
    std::vector<elf_symbol> symbols;

    /// The PT_LOAD covering `file_offset`, as (p_offset, p_vaddr). Needed to
    /// convert a runtime address into this file's virtual space.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> load_segments;

    /// Name covering `vaddr` in the file's own space, or "".
    const elf_symbol* find(std::uint64_t vaddr) const {
        // Largest symbol whose value is <= vaddr.
        auto it = std::upper_bound(symbols.begin(), symbols.end(), vaddr,
                                   [](std::uint64_t v, const elf_symbol& s) {
                                       return v < s.value;
                                   });
        if (it == symbols.begin()) return nullptr;
        --it;
        // A symbol with a size must actually contain the address; one without a
        // size (some assembly routines) is accepted as the nearest preceding
        // name, which is what a disassembler would show.
        if (it->size != 0 && vaddr >= it->value + it->size) return nullptr;
        return &*it;
    }

    /// Convert a runtime address to this file's virtual space.
    ///
    /// `map_start` and `map_file_offset` come from /proc/self/maps.
    std::uint64_t to_file_vaddr(std::uint64_t ip, std::uint64_t map_start,
                                std::uint64_t map_file_offset) const {
        for (const auto& [p_offset, p_vaddr] : load_segments) {
            // The segment whose file range covers this mapping's offset.
            if (map_file_offset >= p_offset) {
                const std::uint64_t delta = map_file_offset - p_offset;
                const std::uint64_t bias = map_start - (p_vaddr + delta);
                return ip - bias;
            }
        }
        // No program headers read: assume the file is mapped at its own vaddrs.
        return ip - map_start + map_file_offset;
    }
};

namespace detail {

#if defined(__linux__)

inline elf_symbol_table read_elf_symbols(const std::string& path) {
    elf_symbol_table t;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        t.note = "cannot open " + path;
        return t;
    }

    Elf64_Ehdr eh{};
    in.read(reinterpret_cast<char*>(&eh), sizeof(eh));
    if (!in || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64) {
        t.note = path + ": not a 64-bit ELF file";
        return t;
    }

    // Program headers, for the load bias.
    if (eh.e_phoff != 0 && eh.e_phnum > 0) {
        in.seekg(static_cast<std::streamoff>(eh.e_phoff));
        for (unsigned i = 0; i < eh.e_phnum; ++i) {
            Elf64_Phdr ph{};
            in.read(reinterpret_cast<char*>(&ph), sizeof(ph));
            if (!in) break;
            if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) != 0) {
                t.load_segments.emplace_back(ph.p_offset, ph.p_vaddr);
            }
        }
    }

    if (eh.e_shoff == 0 || eh.e_shnum == 0) {
        t.note = path + ": no section headers (stripped?)";
        return t;
    }

    std::vector<Elf64_Shdr> sections(eh.e_shnum);
    in.seekg(static_cast<std::streamoff>(eh.e_shoff));
    in.read(reinterpret_cast<char*>(sections.data()),
            static_cast<std::streamsize>(sizeof(Elf64_Shdr) * eh.e_shnum));
    if (!in) {
        t.note = path + ": truncated section headers";
        return t;
    }

    // .symtab carries static functions and is what a profiler needs; .dynsym
    // holds only exported ones and is the fallback for a stripped-but-dynamic
    // library.
    auto load_from = [&](Elf64_Word want) -> bool {
        for (const Elf64_Shdr& sh : sections) {
            if (sh.sh_type != want) continue;
            if (sh.sh_link >= sections.size()) continue;
            const Elf64_Shdr& str = sections[sh.sh_link];

            std::vector<char> strings(str.sh_size);
            in.seekg(static_cast<std::streamoff>(str.sh_offset));
            in.read(strings.data(), static_cast<std::streamsize>(str.sh_size));
            if (!in) return false;

            const std::size_t count =
                sh.sh_entsize ? sh.sh_size / sh.sh_entsize : 0;
            in.seekg(static_cast<std::streamoff>(sh.sh_offset));
            for (std::size_t i = 0; i < count; ++i) {
                Elf64_Sym sym{};
                in.read(reinterpret_cast<char*>(&sym), sizeof(sym));
                if (!in) break;
                if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC) continue;
                if (sym.st_value == 0) continue;
                if (sym.st_name >= strings.size()) continue;
                elf_symbol s;
                s.value = sym.st_value;
                s.size = sym.st_size;
                s.name = &strings[sym.st_name];
                if (!s.name.empty()) t.symbols.push_back(std::move(s));
            }
            return !t.symbols.empty();
        }
        return false;
    };

    if (!load_from(SHT_SYMTAB)) load_from(SHT_DYNSYM);

    if (t.symbols.empty()) {
        t.note = path + ": no function symbols (stripped)";
        return t;
    }

    std::sort(t.symbols.begin(), t.symbols.end(),
              [](const elf_symbol& a, const elf_symbol& b) { return a.value < b.value; });
    t.ok = true;
    return t;
}

#else

inline elf_symbol_table read_elf_symbols(const std::string& path) {
    elf_symbol_table t;
    t.note = path + ": ELF symbol reading is implemented for Linux only";
    return t;
}

#endif

}  // namespace detail
}  // namespace ppe::probe
