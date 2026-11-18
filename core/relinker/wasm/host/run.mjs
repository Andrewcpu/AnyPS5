import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer as createNet } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { kClearOpcode, kCommandBytes, kConstantColor, kDrawOpcode, kMaxExtent, parseCommand, translateShader } from './webgpu.mjs';

const hostPath = fileURLToPath(new URL('./webgpu.mjs', import.meta.url));

function fail(message) {
    console.error(message);
    process.exit(1);
}

function command(opcode, width, height, rgba, shader) {
    const bytes = new Uint8Array(kCommandBytes);
    const view = new DataView(bytes.buffer);
    view.setUint32(0, opcode, true);
    view.setUint32(4, width, true);
    view.setUint32(8, height, true);
    view.setUint32(12, rgba, true);
    view.setUint32(16, shader, true);
    return bytes;
}

function expectThrow(text, fn) {
    let thrown = '';
    try {
        fn();
    } catch (error) {
        thrown = error && error.message ? error.message : String(error);
    }
    if (!thrown.includes(text)) fail(thrown || 'accepted ' + text);
}

function check() {
    const wgsl = translateShader(kConstantColor);
    if (!wgsl.includes('@vertex') || !wgsl.includes('@fragment') || !wgsl.includes('anyps5_color.value') || !wgsl.includes('vec2f(-1.0, -1.0)')) fail('constant-color shader');
    expectThrow('unknown webgpu shader', () => translateShader(0));
    expectThrow('unknown webgpu shader', () => translateShader(2));
    const clear = parseCommand(command(kClearOpcode, 2, 2, 0xff0000ff, 0));
    if (clear.opcode !== kClearOpcode || clear.width !== 2 || clear.height !== 2 || clear.rgba !== 0xff0000ff || clear.shader !== 0) fail('clear command');
    const draw = parseCommand(command(kDrawOpcode, kMaxExtent, 1, 0xffff0000, kConstantColor));
    if (draw.opcode !== kDrawOpcode || draw.width !== kMaxExtent || draw.shader !== kConstantColor || draw.rgba !== 0xffff0000) fail('draw command');
    expectThrow('unknown webgpu opcode', () => parseCommand(command(9, 2, 2, 0xff0000ff, 0)));
    expectThrow('unknown webgpu shader', () => parseCommand(command(kDrawOpcode, 2, 2, 0xff0000ff, 4)));
    expectThrow('unknown webgpu shader', () => parseCommand(command(kClearOpcode, 2, 2, 0xff0000ff, kConstantColor)));
    expectThrow('webgpu extent', () => parseCommand(command(kClearOpcode, 0, 2, 0xff0000ff, 0)));
    expectThrow('webgpu extent', () => parseCommand(command(kDrawOpcode, 1, kMaxExtent + 1, 0xffff0000, kConstantColor)));
    expectThrow('webgpu command size', () => parseCommand(command(kClearOpcode, 2, 2, 0xff0000ff, 0).slice(0, 19)));
    console.log('ok');
}

function freePort() {
    return new Promise((resolve, reject) => {
        const server = createNet();
        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            const { port } = server.address();
            server.close(() => resolve(port));
        });
    });
}

async function browser(chrome, wasmPath) {
    if (!chrome || !wasmPath) fail('browser requires chrome and a wasm module');
    const wasm = await readFile(wasmPath);
    const host = await readFile(hostPath);
    const page = `<!DOCTYPE html>
<canvas id="screen"></canvas>
<pre id="out">pending</pre>
<script type="module">
import { createWebGpuService } from '/webgpu.mjs';
try {
  const service = await createWebGpuService(navigator.gpu, document.getElementById('screen'));
  let memory;
  const compiled = await WebAssembly.instantiate(await (await fetch('/guest.wasm')).arrayBuffer(), {
    env: { anyps5_webgpu_submit(ptr, len) { return service.submit(memory, ptr, len); } },
  });
  memory = compiled.instance.exports.memory;
  const status = compiled.instance.exports.anyps5_run();
  if (status !== 0) {
    window.result = 'fault ' + status + ' ' + service.rejection();
  } else {
    const pixels = await service.pixels();
    const err = pixels.errors.length ? pixels.errors.join(' | ') : 'none';
    window.result = 'status ' + status + ' off ' + pixels.origin.join(',') + ' corner ' + pixels.corner.join(',') + ' canvas ' + (pixels.presented ? 'presented' : 'missing') + ' err ' + err;
  }
} catch (error) {
  window.result = 'throw ' + (error && error.message ? error.message : error);
}
document.getElementById('out').textContent = window.result;
</script>`;
    const server = createServer((request, response) => {
        if (request.url === '/webgpu.mjs') {
            response.setHeader('content-type', 'text/javascript');
            response.end(host);
            return;
        }
        if (request.url === '/guest.wasm') {
            response.setHeader('content-type', 'application/wasm');
            response.end(wasm);
            return;
        }
        response.setHeader('content-type', 'text/html');
        response.end(page);
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const httpPort = server.address().port;
    const debugPort = await freePort();
    const profile = await mkdtemp(join(tmpdir(), 'anyps5-webgpu-'));
    const chromeLog = [];
    const child = spawn(chrome, [
        '--headless=new',
        '--enable-unsafe-webgpu',
        '--no-sandbox',
        '--disable-gpu-sandbox',
        '--no-first-run',
        '--disable-sync',
        '--disable-dev-shm-usage',
        '--user-data-dir=' + profile,
        '--remote-debugging-port=' + debugPort,
        '--remote-allow-origins=*',
        'about:blank',
    ], { stdio: ['ignore', 'ignore', 'pipe'], detached: true });
    child.stderr.on('data', (chunk) => {
        chromeLog.push(chunk.toString());
        if (chromeLog.join('').length > 4000) chromeLog.splice(0, chromeLog.length - 4);
    });
    const stop = () => {
        try { process.kill(-child.pid, 'SIGKILL'); } catch { child.kill('SIGKILL'); }
        server.close();
    };
    try {
        let version = null;
        for (let attempt = 0; attempt < 50; attempt += 1) {
            try {
                const response = await fetch('http://127.0.0.1:' + debugPort + '/json/version');
                if (response.ok) {
                    version = await response.json();
                    break;
                }
            } catch {
            }
            await delay(200);
        }
        if (!version) fail('chrome did not open a debugging port\n' + chromeLog.join(''));
        const created = await fetch('http://127.0.0.1:' + debugPort + '/json/new?http://127.0.0.1:' + httpPort + '/', { method: 'PUT' });
        const target = await created.json();
        const socket = new WebSocket(target.webSocketDebuggerUrl);
        await new Promise((resolve, reject) => {
            socket.addEventListener('open', resolve);
            socket.addEventListener('error', reject);
        });
        let id = 0;
        const waiters = new Map();
        const logs = [];
        socket.addEventListener('message', (event) => {
            const message = JSON.parse(event.data);
            if (message.id && waiters.has(message.id)) waiters.get(message.id)(message);
            if (message.method === 'Runtime.exceptionThrown') logs.push(message.params.exceptionDetails.exception?.description || 'exception');
        });
        const call = (method, params = {}) => new Promise((resolve) => {
            const seq = ++id;
            waiters.set(seq, resolve);
            socket.send(JSON.stringify({ id: seq, method, params }));
        });
        await call('Runtime.enable');
        let text = '';
        for (let attempt = 0; attempt < 80; attempt += 1) {
            await delay(250);
            const evaluated = await call('Runtime.evaluate', { expression: 'window.result || ""', returnByValue: true });
            text = evaluated.result?.result?.value || '';
            if (text) break;
        }
        socket.close();
        if (!text) fail('webgpu page produced no result\n' + logs.join('\n') + '\n' + chromeLog.join(''));
        console.log(text);
    } finally {
        stop();
        await rm(profile, { recursive: true, force: true });
    }
}

const mode = process.argv[2];
if (mode === 'check') check();
else if (mode === 'browser') await browser(process.argv[3], process.argv[4]);
else fail('usage: run.mjs check | run.mjs browser <chrome> <guest.wasm>');
