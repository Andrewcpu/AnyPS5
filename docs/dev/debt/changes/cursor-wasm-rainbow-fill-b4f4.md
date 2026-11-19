# relinker

## add Functional

- The [WebAssembly lifter](../../../core/relinker/wasm/aot/src/Lift.cpp) counts a step on a dispatch switch, a return to a recovered block, and a direct edge whose target is at an earlier or equal address. A direct edge to a later address does not count, so a fill loop counts the branch back to its compare and not the fallthrough into the store. A 1920 by 1088 fill is one step per pixel. Sixteen million steps still stop the guest with status 3 during a later frame. Closes when: the fill is lowered without a per-pixel step and an infinite direct loop still stops.
