const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

function reservePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(error => error ? reject(error) : resolve(port));
    });
  });
}

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function inspectorTarget(port, child, diagnostics) {
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) {
      throw new Error(`inspector child exited early (${child.exitCode})\n${diagnostics()}`);
    }
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`);
      const targets = await response.json();
      if (targets[0]?.webSocketDebuggerUrl) return targets[0].webSocketDebuggerUrl;
    } catch {}
    await delay(10);
  }
  throw new Error(`timed out waiting for inspector\n${diagnostics()}`);
}

function connectCDP(url) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url);
    const pending = new Map();
    const events = [];
    let nextId = 0;

    socket.onerror = event => reject(new Error(`inspector websocket error: ${event.type}`));
    socket.onmessage = event => {
      const message = JSON.parse(String(event.data));
      if (!message.id) {
        events.push(message);
        return;
      }
      const request = pending.get(message.id);
      if (!request) return;
      pending.delete(message.id);
      if (message.error) request.reject(new Error(message.error.message));
      else request.resolve(message.result);
    };
    socket.onopen = () => resolve({
      socket,
      send(method, params = {}) {
        const id = ++nextId;
        return new Promise((resolveRequest, rejectRequest) => {
          pending.set(id, { resolve: resolveRequest, reject: rejectRequest });
          socket.send(JSON.stringify({ id, method, params }));
        });
      },
      async waitFor(method, predicate) {
        const deadline = Date.now() + 3000;
        while (Date.now() < deadline) {
          const index = events.findIndex(message =>
            message.method === method && predicate(message.params));
          if (index !== -1) return events.splice(index, 1)[0].params;
          await delay(10);
        }
        throw new Error(`timed out waiting for ${method}`);
      },
    });
  });
}

function headerValue(headers, name) {
  const key = Object.keys(headers).find(key => key.toLowerCase() === name);
  return key === undefined ? undefined : headers[key];
}

async function main() {
  const [inspectorPort, serverPort] = await Promise.all([reservePort(), reservePort()]);
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-inspector-network-'));
  const serverPath = path.join(tmpDir, 'server.mjs');
  fs.writeFileSync(serverPath, `
export default {
  hostname: '127.0.0.1',
  port: ${serverPort},
  fetch() {
    return new Response('inspected', { headers: { 'x-inspector-test': 'yes' } });
  },
};
`);

  const child = spawn(process.execPath, [
    `--inspect=127.0.0.1:${inspectorPort}`,
    serverPath,
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', chunk => { stdout += String(chunk); });
  child.stderr.on('data', chunk => { stderr += String(chunk); });
  const diagnostics = () => `stdout:\n${stdout}\nstderr:\n${stderr}`;

  let cdp;
  try {
    const target = await inspectorTarget(inspectorPort, child, diagnostics);
    cdp = await connectCDP(target);
    await cdp.send('Network.enable');

    const requestUrl = `http://127.0.0.1:${serverPort}/observed`;
    let response;
    const deadline = Date.now() + 3000;
    while (Date.now() < deadline) {
      try {
        response = await fetch(requestUrl, {
          headers: { 'x-request-test': 'present' },
        });
        break;
      } catch {
        if (child.exitCode !== null) {
          throw new Error(`server child exited early (${child.exitCode})\n${diagnostics()}`);
        }
        await delay(10);
      }
    }
    assert(response, `timed out waiting for server\n${diagnostics()}`);
    assert.equal(await response.text(), 'inspected');

    const requestEvent = await cdp.waitFor(
      'Network.requestWillBeSent',
      params => params.request?.url === requestUrl,
    );
    assert.equal(
      headerValue(requestEvent.request.headers, 'x-request-test'),
      'present',
    );

    const responseEvent = await cdp.waitFor(
      'Network.responseReceived',
      params => params.response?.url === requestUrl,
    );
    assert.equal(responseEvent.response.status, 200);
    assert.equal(
      headerValue(responseEvent.response.headers, 'x-inspector-test'),
      'yes',
    );
    assert.equal(child.exitCode, null, diagnostics());
    console.log('inspector network server events preserve request and response headers');
  } finally {
    if (cdp) cdp.socket.close();
    child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
