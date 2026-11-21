# relinker

## add Functional

- The [WebAssembly lifter](../../../core/relinker/wasm/aot/src/Lift.cpp) counts a step on a dispatch switch, a return to a recovered block, and a direct edge whose target is at an earlier or equal address. A direct edge to a later address does not count. Sixteen million steps still stop the guest with status 3.
