# relinker

## add Functional

- The [WebAssembly lifter](../../../core/relinker/wasm/aot/src/Decode.cpp) accepts an address-size prefix only on a near call or jump, a multi-byte NOP (`0F 1F`), 8-bit `TEST`, `MOVZX`/`MOVSX` from a byte, `CDQ`/`CQO`, 32-bit `IDIV`, and scalar 128-bit VEX moves, arithmetic, compare, and integer conversions. A 256-bit VEX prefix, EVEX, any other VEX opcode, and 64-bit `IDIV` throw. A RIP-relative `LEA` whose result is in an executable segment is a discovery root. `SYSCALL` passes the guest registers to `env.anyps5_host_call`; number `0x259` appends the guest C string in `rsi` to the host log and returns 0, and any other number faults. Closes when: the lifter covers the VEX and syscall forms a title executes.
- The [WebAssembly host](../../../core/relinker/wasm/host/guest.mjs) answers the VideoOut, direct-memory and libc wrappers used by a homebrew flip. Direct memory is a bump allocator of at most 1 GiB placed above the image window; memory type and protection are not distinguished, and a map flag other than 0 faults. `sceSystemServiceHideSplashScreen` returns 0. `sceKernelWaitEqueue` returns 0 without blocking. `fmodf` returns the remainder of the low 32 bits of `xmm0` and `xmm1`. `snprintf` and `vsnprintf` accept `%s`, `%d`, `%i`, `%u`, `%x`, `%X`, `%p`, `%c`, `%%` and an `l` length, and any other conversion faults. The dispatch loop stops after sixteen million steps. Closes when: these calls match the console for every argument a title passes.
- The [WebGPU host](../../../core/relinker/wasm/host/webgpu.mjs) accepts opcode 3, a linear RGBA8 present of at most 3840 by 3840. The command's fourth and fifth words are the wasm offset and the byte stride; a stride of 0 means four bytes per pixel. `sceVideoOutSubmitFlip` builds that command for tiling 0 and pixel format `0x8000000022000000`. Any other tiling or pixel format faults. This is not a PlayStation GPU. Closes when: a guest can present the buffer forms a title flips.

# TechnicalDebt

## add Build

- A Windows relinker ctest run can fail `tools/tests/test_debt_split.py` `test_split_rewrites_angle_bracket_destinations_inside_their_brackets` in 0.07s with no assertion in the log.
  Job 135082548702 of run 44383682932 printed the ctest `***Failed` line and one blank line, then started the next test. The log has no `AssertionError`, traceback, or Python fatal error for that test. The same test passed on the previous Windows job of this branch and passes on Linux. Closes when: that Windows job's log contains the assertion, or the test passes there. Labels: windows, tested
