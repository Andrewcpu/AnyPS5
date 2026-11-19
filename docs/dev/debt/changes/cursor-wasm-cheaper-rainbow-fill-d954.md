# relinker

## add Functional

- The [WebAssembly lifter](../../../core/relinker/wasm/aot/src/Lift.cpp) lowers a two-block counted 32-bit fill, a sign-extended stack index compared with jae against a stack limit and a body that stores one dword at base plus index times 4 then adds one, into one store loop. It runs only when the index and the limit both fit in 31 bits and the written bytes lie in the guest window or the direct heap and do not cover those stack slots. Any other shape keeps its recovered blocks. An infinite direct loop still returns status 3 at sixteen million steps. Closes when: a counted loop outside this shape is lowered the same way.
