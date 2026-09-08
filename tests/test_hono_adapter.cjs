// Regression test for packages/hono (@ant/hono): serve, upgradeWebSocket,
// getConnInfo, serveStatic, getAntServer. Runs the built package from
// packages/hono/dist against hono from packages/hono/node_modules.
const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..');
const packageDir = path.join(repoRoot, 'packages', 'hono');
const honoDir = path.join(packageDir, 'node_modules', 'hono');

function waitForLine(child) {
  return new Promise((resolve, reject) => {
    let stdout = '';
    let stderr = '';
    const timeout = setTimeout(() => {
      child.kill('SIGTERM');
      reject(new Error(`timed out waiting for server metadata\nstdout:\n${stdout}\nstderr:\n${stderr}`));
    }, 5000);

    child.stdout.on('data', chunk => {
      stdout += String(chunk);
      const newline = stdout.indexOf('\n');
      if (newline === -1) return;
      clearTimeout(timeout);
      resolve(stdout.slice(0, newline));
    });

    child.stderr.on('data', chunk => {
      stderr += String(chunk);
    });

    child.on('exit', code => {
      clearTimeout(timeout);
      if (code !== null && stdout.indexOf('\n') === -1) {
        reject(new Error(`server exited before metadata; code=${code}\nstderr:\n${stderr}`));
      }
    });
  });
}

async function waitForExit(child) {
  if (child.exitCode !== null) return child.exitCode;
  return await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      child.kill('SIGTERM');
      reject(new Error('timed out waiting for server process to exit'));
    }, 3000);

    child.once('exit', code => {
      clearTimeout(timeout);
      resolve(code);
    });
  });
}

// Ant's WebSocket client does not send Sec-WebSocket-Protocol, so subprotocol
// negotiation is checked on the wire with a raw handshake.
function rawHandshake(port, pathname, extraHeaders) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(port, '127.0.0.1');
    let data = '';
    const timer = setTimeout(() => { socket.destroy(); reject(new Error('raw handshake timed out')); }, 3000);
    socket.on('connect', () => {
      socket.write(
        `GET ${pathname} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        `Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n${extraHeaders}\r\n`
      );
    });
    socket.on('data', chunk => {
      data += String(chunk);
      const end = data.indexOf('\r\n\r\n');
      if (end === -1) return;
      clearTimeout(timer);
      socket.destroy();
      const [statusLine, ...lines] = data.slice(0, end).split('\r\n');
      const headers = {};
      for (const line of lines) {
        const idx = line.indexOf(':');
        headers[line.slice(0, idx).trim().toLowerCase()] = line.slice(idx + 1).trim();
      }
      resolve({ status: Number(statusLine.split(' ')[1]), headers });
    });
    socket.on('error', error => { clearTimeout(timer); reject(error); });
  });
}

// Sends "close" over a raw WebSocket connection and returns the close frame the
// server replies with. Ant's WebSocket client does not surface the server's
// close code, so the frame is decoded by hand.
function rawCloseFrame(port, pathname) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(port, '127.0.0.1');
    const chunks = [];
    let handshakeDone = false;
    const timer = setTimeout(() => { socket.destroy(); reject(new Error('raw close timed out')); }, 3000);
    socket.on('connect', () => {
      socket.write(
        `GET ${pathname} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n'
      );
    });
    socket.on('data', chunk => {
      chunks.push(Buffer.from(chunk));
      let buffer = Buffer.concat(chunks);
      if (!handshakeDone) {
        const end = buffer.indexOf('\r\n\r\n');
        if (end === -1) return;
        if (!String(buffer.slice(0, end)).startsWith('HTTP/1.1 101')) {
          clearTimeout(timer);
          socket.destroy();
          return reject(new Error(`unexpected handshake: ${String(buffer.slice(0, end))}`));
        }
        handshakeDone = true;
        chunks.length = 0;
        buffer = buffer.slice(end + 4);
        if (buffer.length) chunks.push(buffer);
        const mask = Buffer.from([1, 2, 3, 4]);
        const payload = Buffer.from('close');
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
        socket.write(Buffer.concat([Buffer.from([0x81, 0x80 | payload.length]), mask, payload]));
      }
      // Skip any non-close frames (e.g. the onOpen greeting) and wait for opcode 8.
      let offset = 0;
      while (offset + 2 <= buffer.length) {
        const opcode = buffer[offset] & 0x0f;
        const len = buffer[offset + 1] & 0x7f;
        if (len > 125) return; // not expected in this test
        if (offset + 2 + len > buffer.length) return;
        if (opcode === 8) {
          clearTimeout(timer);
          socket.destroy();
          const body = buffer.slice(offset + 2, offset + 2 + len);
          return resolve({ code: body.readUInt16BE(0), reason: String(body.slice(2)) });
        }
        offset += 2 + len;
      }
    });
    socket.on('error', error => { clearTimeout(timer); reject(error); });
  });
}

function openWebSocket(url, protocols) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, protocols);
    const timer = setTimeout(() => reject(new Error('websocket open timed out')), 3000);
    ws.onopen = () => { clearTimeout(timer); resolve(ws); };
    ws.onerror = event => { clearTimeout(timer); reject(new Error(`websocket error: ${event && event.message}`)); };
  });
}

function nextMessage(ws) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('websocket message timed out')), 3000);
    ws.onmessage = event => { clearTimeout(timer); resolve(event.data); };
  });
}

function nextClose(ws) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('websocket close timed out')), 3000);
    ws.onclose = event => { clearTimeout(timer); resolve(event); };
  });
}

async function main() {
  if (!fs.existsSync(path.join(packageDir, 'dist', 'index.js'))) {
    throw new Error('packages/hono/dist is missing; run `ant install && npm run build` in packages/hono');
  }
  if (!fs.existsSync(path.join(honoDir, 'package.json'))) {
    throw new Error('packages/hono/node_modules/hono is missing; run `ant install` in packages/hono');
  }

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-hono-adapter-'));
  const serverPath = path.join(tmpDir, 'server.mjs');
  const publicDir = path.join(tmpDir, 'public');

  fs.mkdirSync(path.join(tmpDir, 'node_modules', '@ant'), { recursive: true });
  fs.symlinkSync(honoDir, path.join(tmpDir, 'node_modules', 'hono'), 'dir');
  fs.symlinkSync(packageDir, path.join(tmpDir, 'node_modules', '@ant', 'hono'), 'dir');
  fs.mkdirSync(path.join(publicDir, 'nested'), { recursive: true });
  fs.writeFileSync(path.join(publicDir, 'hello.txt'), 'static hello');
  fs.writeFileSync(path.join(publicDir, 'nested', 'index.html'), '<h1>nested</h1>');

  fs.writeFileSync(
    serverPath,
    `
import { Hono } from 'hono';
import { serve, getAntServer, getConnInfo, serveStatic, upgradeWebSocket } from '@ant/hono';

const decoder = new TextDecoder();
const app = new Hono();
let server;
let listened = null;

app.get('/', c => c.text('hello hono'));

app.get('/server', c => {
  const s = getAntServer(c);
  return c.json({
    same: s === server,
    envIsServer: c.env === server,
    port: s.port,
    hostname: s.hostname,
    url: s.url
  });
});

app.get('/ip', c => c.json(getConnInfo(c)));

app.use('/static/*', serveStatic({ root: ${JSON.stringify(publicDir)}, rewriteRequestPath: p => p.replace(/^\\/static/, '') }));

app.get('/ws', upgradeWebSocket(c => ({
  onOpen(event, ws) {
    ws.send('open:' + ws.protocol + ':' + ws.url.pathname + ':' + ws.readyState);
  },
  onMessage(event, ws) {
    if (typeof event.data === 'string') {
      if (event.data === 'close') return ws.close(4000, 'bye');
      return ws.send('text:' + event.data);
    }
    const bytes = new Uint8Array(event.data);
    ws.send('binary:' + (event.data instanceof ArrayBuffer) + ':' + decoder.decode(bytes));
  }
})));

app.get('/ws-open-only', upgradeWebSocket(() => ({})));
app.get('/ws-explicit', upgradeWebSocket(() => ({}), { protocol: 'other' }));
app.get('/ws-not-offered', upgradeWebSocket(() => ({}), { protocol: 'nope' }));

app.get('/stop', c => {
  queueMicrotask(() => server.stop());
  return c.text('stopping');
});

let badPort = null;
try { serve({ fetch: app.fetch, port: 'nope' }); } catch (e) { badPort = e.constructor.name; }
let badFetch = null;
try { serve({ port: 0 }); } catch (e) { badFetch = e.constructor.name; }

server = serve({ fetch: app.fetch, hostname: '127.0.0.1', port: '0' }, s => { listened = s; });

console.log(JSON.stringify({
  port: server.port,
  hostname: server.hostname,
  url: server.url,
  listened: listened === server,
  badPort,
  badFetch
}));
`
  );

  const child = spawn(process.execPath, [serverPath], { stdio: ['ignore', 'pipe', 'pipe'] });

  try {
    const metadata = JSON.parse(await waitForLine(child));
    assert.equal(metadata.hostname, '127.0.0.1');
    assert.equal(typeof metadata.port, 'number');
    assert(metadata.port > 0);
    assert.equal(metadata.url, `http://127.0.0.1:${metadata.port}`);
    assert.equal(metadata.listened, true);
    assert.equal(metadata.badPort, 'TypeError');
    assert.equal(metadata.badFetch, 'TypeError');

    const base = `http://127.0.0.1:${metadata.port}`;

    const root = await fetch(`${base}/`);
    assert.equal(root.status, 200);
    assert.equal(await root.text(), 'hello hono');

    const serverInfo = await (await fetch(`${base}/server`)).json();
    assert.deepEqual(serverInfo, {
      same: true,
      envIsServer: true,
      port: metadata.port,
      hostname: '127.0.0.1',
      url: metadata.url
    });

    const conn = await (await fetch(`${base}/ip`)).json();
    assert.equal(conn.remote.address, '127.0.0.1');
    assert.equal(conn.remote.addressType, 'IPv4');
    assert.equal(conn.remote.transport, 'tcp');
    assert.equal(typeof conn.remote.port, 'number');
    assert(conn.remote.port > 0);

    const file = await fetch(`${base}/static/hello.txt`);
    assert.equal(file.status, 200);
    assert.equal(file.headers.get('content-type'), 'text/plain; charset=utf-8');
    assert.equal(await file.text(), 'static hello');

    const dirIndex = await fetch(`${base}/static/nested`);
    assert.equal(dirIndex.status, 200);
    assert.equal(dirIndex.headers.get('content-type'), 'text/html; charset=utf-8');
    assert.equal(await dirIndex.text(), '<h1>nested</h1>');

    const missing = await fetch(`${base}/static/missing.txt`);
    assert.equal(missing.status, 404);

    const notUpgrade = await fetch(`${base}/ws`);
    assert.equal(notUpgrade.status, 404);

    const negotiated = await rawHandshake(metadata.port, '/ws', 'Sec-WebSocket-Protocol: chat, other\r\n');
    assert.equal(negotiated.status, 101);
    assert.equal(negotiated.headers['upgrade'], 'websocket');
    assert.equal(negotiated.headers['sec-websocket-protocol'], 'chat');

    const explicit = await rawHandshake(metadata.port, '/ws-explicit', 'Sec-WebSocket-Protocol: chat, other\r\n');
    assert.equal(explicit.status, 101);
    assert.equal(explicit.headers['sec-websocket-protocol'], 'other');

    const none = await rawHandshake(metadata.port, '/ws-open-only', '');
    assert.equal(none.status, 101);
    assert.equal(none.headers['sec-websocket-protocol'], undefined);

    const notOffered = await rawHandshake(metadata.port, '/ws-not-offered', 'Sec-WebSocket-Protocol: chat\r\n');
    assert.equal(notOffered.status, 500);

    const wsUrl = `ws://127.0.0.1:${metadata.port}/ws`;
    const ws = await openWebSocket(wsUrl);
    assert.equal(await nextMessage(ws), 'open:null:/ws:1');

    const text = nextMessage(ws);
    ws.send('ping');
    assert.equal(await text, 'text:ping');

    const binary = nextMessage(ws);
    ws.send(new TextEncoder().encode('bytes'));
    assert.equal(await binary, 'binary:true:bytes');

    const closed = nextClose(ws);
    ws.send('close');
    await closed;

    const closeFrame = await rawCloseFrame(metadata.port, '/ws');
    assert.equal(closeFrame.code, 4000);
    assert.equal(closeFrame.reason, 'bye');

    const plain = await openWebSocket(`ws://127.0.0.1:${metadata.port}/ws-open-only`);
    const plainClosed = nextClose(plain);
    plain.close();
    await plainClosed;

    const stop = await fetch(`${base}/stop`);
    assert.equal(await stop.text(), 'stopping');
    assert.equal(await waitForExit(child), 0);

    console.log('ant:hono-adapter:ok');
  } finally {
    if (child.exitCode === null) child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
