# relinker

## add Functional

- The [WebAssembly shift flags](../../../core/relinker/wasm/aot/src/Lift.cpp) clear OF when the count is not 1 and clear AF. A count of 0 leaves RFLAGS unchanged. Intel leaves OF undefined for a count other than 1 and leaves AF undefined for every shift. Measured on this host: `shl $1` of `0x7f` and `shl $4` of `0x7f` clear OF, and `shl $1` of 32-bit `0x80000000` sets OF to the result sign XOR CF. Closes when: measured on a PS5.
- The [WebAssembly DIV](../../../core/relinker/wasm/aot/src/Lift.cpp) leaves RFLAGS unchanged and faults when the divisor is 0 or the quotient does not fit the destination. Intel leaves the flags undefined. Measured on this host, `div` of 20 by 6 left the flags of the preceding `xor`. Closes when: measured on a PS5.
- The [WebAssembly IMUL](../../../core/relinker/wasm/aot/src/Lift.cpp) sets CF and OF from signed overflow of the truncated product, sets SF, ZF and PF from that product, and clears AF. Intel leaves SF, ZF, AF and PF undefined. Measured on this host, `imul $3, 5` cleared CF, OF and AF and set PF. Closes when: measured on a PS5.
