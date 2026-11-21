# relinker

## add Functional

- A [WebAssembly control transfer](../../../core/relinker/wasm/aot/src/Lift.cpp) whose destination is outside the guest window ends the process with lifter status 0. That covers a direct or conditional branch, a call, an indirect jump or call, and a return, including a return whose 8-byte slot is not in the guest window or the direct heap. The stopped RIP is the destination when that address was read, and the return instruction when a native return's slot was outside the window. An import return with that slot stops at the import entry. An import entry just past the window stays a call. A load or store outside the window still faults. A destination inside the window that is not a recovered block stops with status 1, and a direct branch into non-executable bytes in the window still fails the lift. The lifter status is not the guest's exit code. Closes when: measured on a PS5.
