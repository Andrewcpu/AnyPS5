# relinker

## add Functional

- The [WebAssembly relocator](../../../core/relinker/wasm/aot/src/Relocate.cpp) applies R_X86_64_NONE, R_X86_64_64, R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_32, R_X86_64_32S, R_X86_64_PC64 and R_X86_64_RELATIVE, plus R_X86_64_GLOB_DAT and R_X86_64_JUMP_SLOT when the addend is 0. A weak undefined symbol resolves to 0 and the load bias is 0. TLS, GOT, COPY, IRELATIVE and every other type throw, as do SHT_REL, a nonzero GLOB_DAT or JUMP_SLOT addend, an undefined global and extended section numbers. A dynamic symbol index is bounded by the file bytes after DT_SYMTAB. Closes when: those remaining System V x86-64 relocations are applied.
