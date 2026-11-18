# relinker

## add Functional

- The [WebGPU host](../../../core/relinker/wasm/host/webgpu.mjs) serves `anyps5_webgpu_submit`. The guest passes 20 bytes: opcode, width, height, packed RGBA with red in the low byte, and a shader form. Opcode 1 with shader form 0 clears an rgba8unorm target. Opcode 2 with shader form 1 draws one fullscreen triangle; that form translates to a WGSL vertex shader and a fragment shader that returns one uniform color. Width and height must be from 1 to 256. Any other size, extent, opcode or shader form throws in the translator and faults the guest, and a negative host result faults the guest. Replacing the target does not wait for an earlier submit. A page canvas is presented in the canvas format. This is not a PlayStation GPU. Closes when: a guest can submit the shader forms a title draws.
