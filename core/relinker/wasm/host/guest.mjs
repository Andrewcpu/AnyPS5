import { kCommandBytes, kPresentOpcode, parseCommand } from './webgpu.mjs';

export const kScratchAddress = 0x200000;
export const kHostResult = 8;
export const kHostDirectEnd = 16;
export const kHostErrno = 24;
export const kHostStrerror = 32;
export const kHostQueue = 160;
export const kDirectBudget = 0x40000000n;
export const kPage = 0x4000n;
export const kStateAddress = 0xf0000n;
export const kStateBytes = 400n;

const kEnomem = 0x8002000c;
const kEfault = 0x8002000e;
const kEinval = 0x80020016;
const kInvalidHandle = -2144796661;
const kInvalidValue = -2144796671;
const kInvalidQueue = -2144796660;
const kBusy = -2144796663;
const kLinearFormat = 0x8000000022000000n;

function f32(bits) {
    const buffer = new ArrayBuffer(4);
    new DataView(buffer).setUint32(0, Number(u64(bits) & 0xffffffffn), true);
    return new Float32Array(buffer)[0];
}

function f32Bits(value) {
    const buffer = new ArrayBuffer(4);
    new Float32Array(buffer)[0] = value;
    return BigInt(new DataView(buffer).getUint32(0, true));
}

function fmodf(left, right) {
    const x = Math.fround(left);
    const y = Math.fround(right);
    if (!Number.isFinite(x) || !Number.isFinite(y) || y === 0) return NaN;
    return Math.fround(x - Math.fround(Math.trunc(x / y) * y));
}

function fail(message) {
    const error = new Error(message);
    error.fault = true;
    throw error;
}

function u64(value) {
    return BigInt.asUintN(64, BigInt(value));
}

function i32(value) {
    return Number(BigInt.asIntN(32, u64(value)));
}

function u32(value) {
    return Number(u64(value) & 0xffffffffn);
}

function se32(value) {
    return BigInt.asUintN(64, BigInt.asIntN(32, BigInt(value)));
}

export function createGuestHost(slot) {
    slot.log = slot.log || '';
    slot.flips = slot.flips || [];
    const allocs = [];
    const queues = [];
    const outputs = [];
    let physCursor = 0n;
    function base() {
        return u64(slot.base ?? 0);
    }
    function view() {
        return new DataView(slot.memory.buffer);
    }
    function wasmOf(guest, size) {
        const address = u64(guest);
        const start = base();
        if (address < start) fail('wasm: guest address');
        const off = address - start;
        const bytes = BigInt(slot.memory.buffer.byteLength);
        if (off + BigInt(size) > bytes) fail('wasm: guest address');
        const state = kStateAddress;
        if (off < state + kStateBytes && off + BigInt(size) > state) fail('wasm: guest address');
        return Number(off);
    }
    function readable(guest, size) {
        try {
            wasmOf(guest, size);
            return true;
        } catch (error) {
            if (error && error.fault) return false;
            throw error;
        }
    }
    function read64(guest) {
        return view().getBigUint64(wasmOf(guest, 8), true);
    }
    function read32(guest) {
        return view().getUint32(wasmOf(guest, 4), true);
    }
    function write64(guest, value) {
        view().setBigUint64(wasmOf(guest, 8), u64(value), true);
    }
    function write32(guest, value) {
        view().setUint32(wasmOf(guest, 4), u32(value), true);
    }
    function writeBytes(guest, bytes) {
        const off = wasmOf(guest, bytes.length);
        new Uint8Array(slot.memory.buffer).set(bytes, off);
    }
    function cstring(guest, limit = 4096) {
        const start = wasmOf(guest, 1);
        const bytes = new Uint8Array(slot.memory.buffer);
        let end = start;
        while (end < bytes.length && end - start < limit && bytes[end] !== 0) end++;
        if (end >= bytes.length || bytes[end] !== 0) fail('wasm: unterminated string');
        return new TextDecoder().decode(bytes.subarray(start, end));
    }
    function result(value) {
        view().setBigUint64(kScratchAddress + kHostResult, u64(value), true);
    }
    function errnoAddress() {
        return base() + BigInt(kScratchAddress + kHostErrno);
    }
    function setErrno(value) {
        view().setInt32(kScratchAddress + kHostErrno, value | 0, true);
    }
    function ok(value) {
        setErrno(0);
        result(value);
        return 0;
    }
    function kernel(code) {
        setErrno(code & 255);
        result(se32(code));
        return 0;
    }
    function video(code) {
        result(se32(code));
        return 0;
    }
    function directEnd() {
        return view().getBigUint64(kScratchAddress + kHostDirectEnd, true);
    }
    function setDirectEnd(value) {
        const next = u64(value);
        if (next > directEnd()) view().setBigUint64(kScratchAddress + kHostDirectEnd, next, true);
    }
    function grow(guestEnd) {
        const wasmEnd = guestEnd - base();
        const bytes = slot.memory.buffer.byteLength;
        if (wasmEnd <= BigInt(bytes)) return true;
        const pages = Number((wasmEnd - BigInt(bytes) + 65535n) / 65536n);
        return slot.memory.grow(pages) >= 0;
    }
    function alignUp(value, alignment) {
        const mask = alignment - 1n;
        return (value + mask) & ~mask;
    }
    function power(value) {
        return value > 0n && (value & (value - 1n)) === 0n;
    }
    function output(handle) {
        return outputs.find((item) => item.handle === handle) || null;
    }
    function gpCursor(list) {
        let gp = read32(list);
        let overflow = read64(list + 8n);
        const save = read64(list + 16n);
        return {
            gp() {
                if (gp < 48) {
                    const value = read64(save + BigInt(gp));
                    gp += 8;
                    return value;
                }
                const value = read64(overflow);
                overflow += 8n;
                return value;
            },
        };
    }
    function regCursor(rcx, r8, r9, rsp) {
        const regs = [u64(rcx), u64(r8), u64(r9)];
        let index = 0;
        let stack = u64(rsp) + 8n;
        return {
            gp() {
                if (index < regs.length) return regs[index++];
                const value = read64(stack);
                stack += 8n;
                return value;
            },
        };
    }
    function format(pattern, cursor) {
        let out = '';
        for (let index = 0; index < pattern.length; index++) {
            if (pattern[index] !== '%') {
                out += pattern[index];
                continue;
            }
            index++;
            if (index >= pattern.length) fail('wasm: format');
            let wide = 0;
            while (pattern[index] === 'l') {
                wide++;
                index++;
                if (index >= pattern.length) fail('wasm: format');
            }
            const spec = pattern[index];
            if (spec === '%') {
                out += '%';
                continue;
            }
            const value = spec === 's' || spec === 'c' || spec === 'd' || spec === 'i' || spec === 'u' || spec === 'x' || spec === 'X' || spec === 'p' ? cursor.gp() : 0n;
            if (spec === 's') out += cstring(value);
            else if (spec === 'c') out += String.fromCharCode(Number(value & 0xffn));
            else if (spec === 'd' || spec === 'i') out += (wide ? BigInt.asIntN(64, value) : BigInt.asIntN(32, value)).toString(10);
            else if (spec === 'u') out += (wide ? value : value & 0xffffffffn).toString(10);
            else if (spec === 'x' || spec === 'X') {
                const text = (wide ? value : value & 0xffffffffn).toString(16);
                out += spec === 'X' ? text.toUpperCase() : text;
            } else if (spec === 'p') out += '0x' + value.toString(16);
            else fail('wasm: format ' + spec);
        }
        return out;
    }
    function print(buffer, size, text) {
        const length = text.length;
        if (length > 0x7fffffff) {
            setErrno(34);
            result(se32(-1));
            return 0;
        }
        const limit = u64(size);
        if (limit > 0n) {
            if (!readable(buffer, 1)) return kernel(kEfault);
            const count = limit - 1n < BigInt(length) ? Number(limit - 1n) : length;
            const bytes = new Uint8Array(count + 1);
            const encoded = new TextEncoder().encode(text);
            bytes.set(encoded.subarray(0, count));
            writeBytes(buffer, bytes);
        }
        return ok(BigInt(length));
    }
    function allocate(rdi, rsi, rcx) {
        const len = u64(rdi);
        const align = u64(rsi);
        if (len === 0n || (len & (kPage - 1n)) !== 0n) return kernel(kEinval);
        if (align !== 0n && (align & (kPage - 1n)) !== 0n) return kernel(kEinval);
        if (u64(rcx) === 0n) return kernel(kEinval);
        if (!readable(rcx, 8)) return kernel(kEfault);
        if (physCursor + len > kDirectBudget) return kernel(kEnomem);
        const phys = physCursor;
        physCursor += len;
        allocs.push({ phys, len });
        write64(rcx, phys);
        return ok(0n);
    }
    function map(rdi, rsi, rcx, r8, r9) {
        const len = u64(rsi);
        const flags = u64(rcx);
        const phys = u64(r8);
        const align = u64(r9) === 0n ? kPage : u64(r9);
        if (flags !== 0n) fail('wasm: direct map flags');
        if (u64(rdi) === 0n) return kernel(kEfault);
        if (!readable(rdi, 8)) return kernel(kEfault);
        if (len === 0n || (len & (kPage - 1n)) !== 0n) return kernel(kEinval);
        if ((phys & (kPage - 1n)) !== 0n || phys + len > kDirectBudget) return kernel(kEinval);
        if (!power(align) || align < kPage) return kernel(kEinval);
        const hint = read64(rdi);
        const heap = base() + BigInt(kScratchAddress);
        let place = alignUp(directEnd(), align);
        if (hint !== 0n && hint >= heap && (hint & (align - 1n)) === 0n) place = hint;
        const end = place + len;
        if (end > heap + kDirectBudget) return kernel(kEnomem);
        if (!grow(end)) return kernel(kEnomem);
        write64(rdi, place);
        setDirectEnd(end);
        return ok(0n);
    }
    function attribute(attr, format, tiling, width, height, option, dcc, clear) {
        if (u64(attr) === 0n || !readable(attr, 80)) fail('wasm: video attribute');
        const bytes = new Uint8Array(80);
        const data = new DataView(bytes.buffer);
        data.setUint32(4, u32(tiling), true);
        data.setUint32(12, u32(width), true);
        data.setUint32(16, u32(height), true);
        data.setBigUint64(24, u64(option), true);
        data.setBigUint64(32, u64(format), true);
        data.setBigUint64(40, u64(clear), true);
        data.setUint32(48, u32(dcc), true);
        writeBytes(attr, bytes);
        return ok(0n);
    }
    function register(handle, setIndex, start, buffers, count, attributePointer, category, option) {
        if (handle <= 0 || handle >= 4) return video(kInvalidHandle);
        const cfg = output(handle);
        if (!cfg) fail('wasm: video handle');
        if (u64(buffers) === 0n || u64(attributePointer) === 0n || u64(option) !== 0n) fail('wasm: video buffers');
        if (setIndex < 0 || setIndex >= 4 || start < 0 || count < 1 || start + count > 16) fail('wasm: video buffers');
        if (category !== 0) fail('wasm: video category');
        if (!readable(attributePointer, 80)) fail('wasm: video buffers');
        const attr = view();
        const at = wasmOf(attributePointer, 80);
        const tiling = attr.getUint32(at + 4, true);
        const width = attr.getUint32(at + 12, true);
        const height = attr.getUint32(at + 16, true);
        const pitch = attr.getUint32(at + 20, true);
        const format = attr.getBigUint64(at + 32, true);
        if (tiling !== 0 || format !== kLinearFormat) fail('wasm: video buffer form');
        if (width < 1 || height < 1 || width > 3840 || height > 3840) fail('wasm: video extent');
        const stride = pitch === 0 ? width * 4 : pitch * 4;
        for (let index = 0; index < count; index++) {
            const entry = u64(buffers) + BigInt(index * 32);
            if (!readable(entry, 8)) fail('wasm: video buffers');
            cfg.buffers[start + index] = { data: read64(entry), width, height, stride };
        }
        return ok(0n);
    }
    function submit(handle, index, mode) {
        const cfg = output(handle);
        if (!cfg) fail('wasm: video handle');
        if (mode < 1 || mode > 6 || index < -2 || index >= 16) fail('wasm: video flip');
        if (index < 0) {
            slot.flips.push({ width: 0, height: 0, offset: 0, stride: 0, pixel: null });
            return ok(0n);
        }
        const buffer = cfg.buffers[index];
        if (!buffer) fail('wasm: video flip');
        const offset = buffer.data - base();
        if (offset < 0n || offset > 0xffffffffn) fail('wasm: video flip');
        const command = new DataView(new ArrayBuffer(kCommandBytes));
        command.setUint32(0, kPresentOpcode, true);
        command.setUint32(4, buffer.width, true);
        command.setUint32(8, buffer.height, true);
        command.setUint32(12, Number(offset), true);
        command.setUint32(16, buffer.stride, true);
        parseCommand(new DataView(command.buffer));
        const pixel = readable(buffer.data, 4) ? [view().getUint8(wasmOf(buffer.data, 4)), view().getUint8(wasmOf(buffer.data, 4) + 1), view().getUint8(wasmOf(buffer.data, 4) + 2), view().getUint8(wasmOf(buffer.data, 4) + 3)] : null;
        slot.flips.push({ width: buffer.width, height: buffer.height, offset: Number(offset), stride: buffer.stride, pixel });
        return ok(0n);
    }
    function dispatch(op, rdi, rsi, rdx, rcx, r8, r9, rsp, rax) {
        if (op === 7) return ok(errnoAddress());
        if (op === 8) {
            const names = { 0: 'Success', 12: 'Cannot allocate memory', 14: 'Bad address', 22: 'Invalid argument', 34: 'Result too large' };
            const text = names[i32(rdi)] || 'Unknown error';
            const bytes = new Uint8Array(text.length + 1);
            bytes.set(new TextEncoder().encode(text));
            writeBytes(base() + BigInt(kScratchAddress + kHostStrerror), bytes);
            return ok(base() + BigInt(kScratchAddress + kHostStrerror));
        }
        if (op === 9) return print(rdi, rsi, format(cstring(rdx), regCursor(rcx, r8, r9, rsp)));
        if (op === 10) return print(rdi, rsi, format(cstring(rdx), gpCursor(rcx)));
        if (op === 11) return ok(f32Bits(fmodf(f32(rdi), f32(rsi))));
        if (op === 12) return ok(0n);
        if (op === 13) return allocate(rdi, rsi, rcx);
        if (op === 14) return map(rdi, rsi, rcx, r8, r9);
        if (op === 15) {
            if (u64(rdi) === 0n || !readable(rdi, 8)) return kernel(kEfault);
            if (u64(rsi) !== 0n) cstring(rsi);
            if (queues.length >= 8) fail('wasm: equeue table');
            const token = base() + BigInt(kScratchAddress + kHostQueue + queues.length * 8);
            queues.push(token);
            write64(rdi, token);
            return ok(0n);
        }
        if (op === 16) {
            if (!queues.includes(u64(rdi))) return kernel(kEinval);
            if (u64(rcx) !== 0n && readable(rcx, 4)) write32(rcx, 0);
            return ok(0n);
        }
        if (op === 17) {
            const user = i32(rdi);
            const bus = i32(rsi);
            const index = i32(rdx);
            if (bus === 0x20) return video(kInvalidValue);
            if ((user !== 0 && user !== 255) || (bus !== 0 && bus !== 1 && bus !== 2) || index !== 0 || u64(rcx) !== 0n) fail('wasm: video open');
            if (outputs.length >= 3) return video(kBusy);
            const handle = outputs.length + 1;
            outputs.push({ handle, buffers: new Array(16) });
            return ok(BigInt(handle));
        }
        if (op === 18) {
            if (!output(i32(rdi))) fail('wasm: video handle');
            if (u64(rsi) === 0n || !readable(rsi, 48)) fail('wasm: video status');
            const bytes = new Uint8Array(48);
            new DataView(bytes.buffer).setUint32(0, 1, true);
            writeBytes(rsi, bytes);
            return ok(0n);
        }
        if (op === 19) {
            if (!output(i32(rsi))) return video(kInvalidHandle);
            if (u64(rdi) === 0n || !queues.includes(u64(rdi))) return video(kInvalidQueue);
            return ok(0n);
        }
        if (op === 20) {
            if (!output(i32(rdi))) fail('wasm: video handle');
            const rate = i32(rsi);
            if (rate < 0 || rate > 2) return video(kInvalidValue);
            return ok(0n);
        }
        if (op === 21) return attribute(rdi, rsi, rdx, rcx, r8, r9, read64(u64(rsp) + 8n), read64(u64(rsp) + 16n));
        if (op === 22) return register(i32(rdi), i32(rsi), i32(rdx), rcx, i32(r8), r9, i32(read64(u64(rsp) + 8n)), read64(u64(rsp) + 16n));
        if (op === 23) return submit(i32(rdi), i32(rsi), i32(rdx));
        if (op === 24) {
            if (u64(rax) !== 0x259n) fail('wasm: syscall ' + u64(rax).toString(16));
            slot.log += cstring(rsi);
            return ok(0n);
        }
        fail('wasm: host op ' + op);
    }
    return {
        anyps5_host_call(op, rdi, rsi, rdx, rcx, r8, r9, rsp, rax) {
            try {
                return dispatch(Number(op), rdi, rsi, rdx, rcx, r8, r9, rsp, rax);
            } catch (error) {
                if (error && typeof error.message === 'string' && error.message.startsWith('wasm:')) return 1;
                throw error;
            }
        },
    };
}
