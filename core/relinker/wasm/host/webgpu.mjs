export const kCommandBytes = 20;
export const kClearOpcode = 1;
export const kDrawOpcode = 2;
export const kPresentOpcode = 3;
export const kConstantColor = 1;
export const kMaxExtent = 256;
export const kPresentMax = 3840;

const kConstantColorWgsl = [
    'struct Anyps5Color { value: vec4f }',
    '@group(0) @binding(0) var<uniform> anyps5_color: Anyps5Color;',
    '@vertex',
    'fn vs(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {',
    '  var positions = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));',
    '  return vec4f(positions[index], 0.0, 1.0);',
    '}',
    '@fragment',
    'fn fs() -> @location(0) vec4f {',
    '  return anyps5_color.value;',
    '}',
].join('\n');

const kPresentWgsl = [
    '@group(0) @binding(0) var anyps5_frame: texture_2d<f32>;',
    '@vertex',
    'fn vs(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {',
    '  var positions = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));',
    '  return vec4f(positions[index], 0.0, 1.0);',
    '}',
    '@fragment',
    'fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {',
    '  return textureLoad(anyps5_frame, vec2i(pos.xy), 0);',
    '}',
].join('\n');

export function translateShader(form) {
    if (form !== kConstantColor) throw new Error('wasm: unknown webgpu shader form ' + form);
    return kConstantColorWgsl;
}

export function parseCommand(bytes) {
    const view = bytes instanceof DataView ? bytes : new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.byteLength !== kCommandBytes) throw new Error('wasm: webgpu command size ' + view.byteLength);
    const opcode = view.getUint32(0, true);
    const width = view.getUint32(4, true);
    const height = view.getUint32(8, true);
    const fourth = view.getUint32(12, true);
    const fifth = view.getUint32(16, true);
    if (opcode === kPresentOpcode) {
        if (width < 1 || width > kPresentMax || height < 1 || height > kPresentMax) throw new Error('wasm: webgpu extent ' + width + 'x' + height);
        const stride = fifth === 0 ? width * 4 : fifth;
        if (stride < width * 4) throw new Error('wasm: webgpu stride ' + stride);
        return { opcode, width, height, offset: fourth, stride, rgba: 0, shader: 0 };
    }
    if (width < 1 || width > kMaxExtent || height < 1 || height > kMaxExtent) throw new Error('wasm: webgpu extent ' + width + 'x' + height);
    if (opcode === kClearOpcode) {
        if (fifth !== 0) throw new Error('wasm: unknown webgpu shader form ' + fifth);
        return { opcode, width, height, rgba: fourth, shader: fifth };
    }
    if (opcode === kDrawOpcode) {
        translateShader(fifth);
        return { opcode, width, height, rgba: fourth, shader: fifth };
    }
    throw new Error('wasm: unknown webgpu opcode ' + opcode);
}

function unpack(rgba) {
    return [
        (rgba & 255) / 255,
        ((rgba >>> 8) & 255) / 255,
        ((rgba >>> 16) & 255) / 255,
        ((rgba >>> 24) & 255) / 255,
    ];
}

export async function createWebGpuService(gpu, canvas) {
    if (!gpu) throw new Error('wasm: webgpu is unavailable');
    const adapter = await gpu.requestAdapter();
    if (!adapter) throw new Error('wasm: webgpu adapter is unavailable');
    const device = await adapter.requestDevice();
    const errors = [];
    device.addEventListener('uncapturederror', (event) => errors.push(event.error.message));
    const context = canvas ? canvas.getContext('webgpu') : null;
    if (canvas && !context) throw new Error('wasm: webgpu canvas is unavailable');
    const canvasFormat = canvas ? gpu.getPreferredCanvasFormat() : '';
    let texture = null;
    let textureWidth = 0;
    let textureHeight = 0;
    let uniform = null;
    let pending = null;
    let rejection = '';
    const pipelines = new Map();
    const blits = new Map();
    const binds = new WeakMap();
    const staging = [];
    function uniformBuffer() {
        if (!uniform) uniform = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        return uniform;
    }
    function pipelineFor(format) {
        const found = pipelines.get(format);
        if (found) return found;
        const module = device.createShaderModule({ code: translateShader(kConstantColor) });
        const pipeline = device.createRenderPipeline({
            layout: 'auto',
            vertex: { module, entryPoint: 'vs' },
            fragment: { module, entryPoint: 'fs', targets: [{ format }] },
            primitive: { topology: 'triangle-list' },
        });
        pipelines.set(format, pipeline);
        return pipeline;
    }
    function blitFor(format) {
        const found = blits.get(format);
        if (found) return found;
        const module = device.createShaderModule({ code: kPresentWgsl });
        const pipeline = device.createRenderPipeline({
            layout: 'auto',
            vertex: { module, entryPoint: 'vs' },
            fragment: { module, entryPoint: 'fs', targets: [{ format }] },
            primitive: { topology: 'triangle-list' },
        });
        blits.set(format, pipeline);
        return pipeline;
    }
    function bindFor(pipeline) {
        const found = binds.get(pipeline);
        if (found) return found;
        const group = device.createBindGroup({
            layout: pipeline.getBindGroupLayout(0),
            entries: [{ binding: 0, resource: { buffer: uniformBuffer() } }],
        });
        binds.set(pipeline, group);
        return group;
    }
    function target(width, height) {
        if (texture && textureWidth === width && textureHeight === height) return texture;
        if (texture) texture.destroy();
        textureWidth = width;
        textureHeight = height;
        texture = device.createTexture({
            size: [width, height, 1],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING,
        });
        return texture;
    }
    function draw(encoder, view, format, color, shaded) {
        const pass = encoder.beginRenderPass({
            colorAttachments: [{
                view,
                clearValue: shaded ? { r: 0, g: 0, b: 0, a: 0 } : { r: color[0], g: color[1], b: color[2], a: color[3] },
                loadOp: 'clear',
                storeOp: 'store',
            }],
        });
        if (shaded) {
            const pipeline = pipelineFor(format);
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, bindFor(pipeline));
            pass.draw(3);
        }
        pass.end();
    }
    function present() {
        if (!context || !pending) return false;
        canvas.width = pending.width;
        canvas.height = pending.height;
        context.configure({ device, format: canvasFormat, alphaMode: 'opaque' });
        const encoder = device.createCommandEncoder();
        const canvasView = context.getCurrentTexture().createView();
        if (pending.copied) {
            const pipeline = blitFor(canvasFormat);
            const group = device.createBindGroup({
                layout: pipeline.getBindGroupLayout(0),
                entries: [{ binding: 0, resource: texture.createView() }],
            });
            const pass = encoder.beginRenderPass({
                colorAttachments: [{ view: canvasView, clearValue: { r: 0, g: 0, b: 0, a: 1 }, loadOp: 'clear', storeOp: 'store' }],
            });
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, group);
            pass.draw(3);
            pass.end();
        } else {
            draw(encoder, canvasView, canvasFormat, pending.color, pending.shaded);
        }
        device.queue.submit([encoder.finish()]);
        return true;
    }
    async function mapBuffer(buffer) {
        await buffer.mapAsync(GPUMapMode.READ);
        const bytes = [...new Uint8Array(buffer.getMappedRange()).slice(0, 4)];
        buffer.unmap();
        return bytes;
    }
    async function readTexture(source, x, y) {
        const buffer = device.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        const encoder = device.createCommandEncoder();
        encoder.copyTextureToBuffer({ texture: source, origin: { x, y, z: 0 } }, { buffer, bytesPerRow: 256 }, { width: 1, height: 1 });
        device.queue.submit([encoder.finish()]);
        const bytes = await mapBuffer(buffer);
        buffer.destroy();
        return bytes;
    }
    return {
        rejection() { return rejection; },
        submit(memory, ptr, len) {
            try {
                const command = parseCommand(new DataView(memory.buffer, ptr, len));
                if (command.opcode === kPresentOpcode) {
                    const bytes = command.stride * command.height;
                    if (command.offset > memory.buffer.byteLength || bytes > memory.buffer.byteLength - command.offset) throw new Error('wasm: webgpu present range');
                    const aligned = Math.ceil((command.width * 4) / 256) * 256;
                    const padded = new Uint8Array(aligned * command.height);
                    const source = new Uint8Array(memory.buffer, command.offset, bytes);
                    const row = command.width * 4;
                    for (let y = 0; y < command.height; y++) padded.set(source.subarray(y * command.stride, y * command.stride + row), y * aligned);
                    const buffer = device.createBuffer({ size: padded.byteLength, usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
                    device.queue.writeBuffer(buffer, 0, padded);
                    staging.push(buffer);
                    if (staging.length > 2) staging.shift().destroy();
                    const encoder = device.createCommandEncoder();
                    encoder.copyBufferToTexture({ buffer, bytesPerRow: aligned, rowsPerImage: command.height }, { texture: target(command.width, command.height) }, { width: command.width, height: command.height });
                    device.queue.submit([encoder.finish()]);
                    pending = { width: command.width, height: command.height, color: [0, 0, 0, 0], shaded: false, copied: true };
                    rejection = '';
                    return 0;
                }
                const color = unpack(command.rgba);
                const shaded = command.opcode === kDrawOpcode;
                if (shaded) device.queue.writeBuffer(uniformBuffer(), 0, new Float32Array(color));
                const encoder = device.createCommandEncoder();
                draw(encoder, target(command.width, command.height).createView(), 'rgba8unorm', color, shaded);
                device.queue.submit([encoder.finish()]);
                pending = { width: command.width, height: command.height, color, shaded, copied: false };
                rejection = '';
                return 0;
            } catch (error) {
                rejection = error && error.message ? error.message : String(error);
                return -1;
            }
        },
        async pixels() {
            if (!texture) throw new Error('wasm: webgpu target is empty');
            const origin = await readTexture(texture, 0, 0);
            const corner = await readTexture(texture, textureWidth - 1, textureHeight - 1);
            const presented = present();
            return { origin, corner, presented, errors: errors.slice() };
        },
    };
}
