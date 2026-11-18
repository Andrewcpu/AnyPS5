import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { access, mkdtemp, readFile, realpath, rm } from 'node:fs/promises';
import { constants } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { kClearOpcode, kCommandBytes, kConstantColor, kDrawOpcode, kMaxExtent, kPresentMax, kPresentOpcode, parseCommand, translateShader } from './webgpu.mjs';

const hostPath = fileURLToPath(new URL('./webgpu.mjs', import.meta.url));

function fail(message) {
    const error = new Error(message);
    error.reported = true;
    console.error(message);
    throw error;
}

function note(chunks, text) {
    chunks.push(text);
    while (chunks.length > 1 && chunks.join('').length > 4000) chunks.shift();
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
    const present = parseCommand(command(kPresentOpcode, kPresentMax, 2, 128, 0));
    if (present.opcode !== kPresentOpcode || present.width !== kPresentMax || present.offset !== 128 || present.stride !== kPresentMax * 4) fail('present command');
    expectThrow('webgpu extent', () => parseCommand(command(kPresentOpcode, kPresentMax + 1, 1, 0, 0)));
    expectThrow('webgpu stride', () => parseCommand(command(kPresentOpcode, 2, 2, 0, 4)));
    console.log('ok');
}

async function chromeCommand(command) {
    try {
        const path = await realpath(command);
        const direct = join(dirname(path), 'chrome');
        await access(direct, constants.X_OK);
        return direct;
    } catch {
        return command;
    }
}

async function chromeProfile() {
    const roots = ['/tmp', '/private/tmp', '/var/tmp', tmpdir()];
    const seen = new Set();
    for (const root of roots) {
        if (!root || seen.has(root)) continue;
        seen.add(root);
        const prefix = join(root, 'aps5-');
        if ((prefix + 'XXXXXX/SingletonSocket').length >= 100) continue;
        try {
            await access(root, constants.W_OK);
        } catch {
            continue;
        }
        return { profile: await mkdtemp(prefix), temp: root };
    }
    fail('chrome profile path is too long for a unix socket');
}

async function openChrome(binary) {
    let reported = 'chrome did not open a debugging port';
    for (let launch = 0; launch < 2; launch += 1) {
        const { profile, temp } = await chromeProfile();
        const log = [];
        const child = spawn(binary, [
            '--headless=new',
            '--enable-unsafe-webgpu',
            '--no-sandbox',
            '--disable-gpu-sandbox',
            '--no-first-run',
            '--disable-sync',
            '--disable-breakpad',
            '--disable-dev-shm-usage',
            '--user-data-dir=' + profile,
            '--remote-debugging-port=0',
            '--remote-allow-origins=*',
            'about:blank',
        ], {
            stdio: ['ignore', 'ignore', 'pipe'],
            detached: true,
            env: {
                ...process.env,
                TMPDIR: temp,
                TEMP: temp,
                TMP: temp,
                DBUS_SESSION_BUS_ADDRESS: 'unix:path=/tmp/aps5-no-bus',
                DBUS_SYSTEM_BUS_ADDRESS: 'unix:path=/tmp/aps5-no-bus',
            },
        });
        child.stderr.on('data', (chunk) => note(log, chunk.toString()));
        let exited = null;
        child.once('exit', (code, signal) => { exited = code ?? signal; });
        const stop = () => {
            try { process.kill(-child.pid, 'SIGKILL'); } catch { child.kill('SIGKILL'); }
        };
        let version = null;
        let port = 0;
        try {
            for (let attempt = 0; attempt < 100 && version == null && exited == null; attempt += 1) {
                try {
                    port = Number((await readFile(join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]);
                    if (port > 0) {
                        const response = await fetch('http://127.0.0.1:' + port + '/json/version');
                        if (response.ok) version = await response.json();
                    }
                } catch {
                }
                if (version == null) await delay(100);
            }
        } catch (error) {
            stop();
            await rm(profile, { recursive: true, force: true });
            throw error;
        }
        if (version) return { port, stop, profile, log };
        stop();
        await rm(profile, { recursive: true, force: true });
        reported = 'chrome did not open a debugging port' + (exited == null ? '' : ' exit ' + exited) + '\n' + log.join('');
    }
    fail(reported);
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
    let opened = null;
    try {
        opened = await openChrome(await chromeCommand(chrome));
        const created = await fetch('http://127.0.0.1:' + opened.port + '/json/new?http://127.0.0.1:' + httpPort + '/', { method: 'PUT' });
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
        if (!text) fail('webgpu page produced no result\n' + logs.join('\n') + '\n' + opened.log.join(''));
        console.log(text);
    } finally {
        if (opened) {
            opened.stop();
            await rm(opened.profile, { recursive: true, force: true });
        }
        server.close();
    }
}

try {
    const mode = process.argv[2];
    if (mode === 'check') check();
    else if (mode === 'browser') await browser(process.argv[3], process.argv[4]);
    else fail('usage: run.mjs check | run.mjs browser <chrome> <guest.wasm>');
} catch (error) {
    if (!error.reported) console.error(error && error.stack ? error.stack : error);
    process.exit(1);
}
