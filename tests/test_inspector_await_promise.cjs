const assert = require('node:assert');
const { spawn } = require('node:child_process');
const net = require('node:net');

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
    let nextId = 0;

    const fail = error => {
      for (const { reject: rejectRequest } of pending.values()) rejectRequest(error);
      pending.clear();
      reject(error);
    };

    socket.onerror = event => fail(new Error(`inspector websocket error: ${event.type}`));
    socket.onmessage = event => {
      const message = JSON.parse(String(event.data));
      if (!message.id) return;
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
    });
  });
}

async function waitForExit(child) {
  if (child.exitCode !== null) return child.exitCode;
  return await new Promise(resolve => child.once('exit', resolve));
}

async function main() {
  const port = await reservePort();
  const child = spawn(process.execPath, [
    `--inspect-wait=127.0.0.1:${port}`,
    '-e',
    'setInterval(() => {}, 1000)',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', chunk => { stdout += String(chunk); });
  child.stderr.on('data', chunk => { stderr += String(chunk); });
  const diagnostics = () => `stdout:\n${stdout}\nstderr:\n${stderr}`;

  let cdp;
  try {
    const url = await inspectorTarget(port, child, diagnostics);
    cdp = await connectCDP(url);

    const whileWaiting = await cdp.send('Runtime.evaluate', {
      expression: 'Promise.resolve(40)',
      awaitPromise: true,
    });
    assert.equal(whileWaiting.result.value, 40);
    await cdp.send('Runtime.runIfWaitingForDebugger');

    const plainPromise = await cdp.send('Runtime.evaluate', {
      expression: 'Promise.resolve(41)',
    });
    assert.equal(plainPromise.result.subtype, 'promise');
    assert.match(plainPromise.result.objectId, /^ant:\d+$/);

    const awaitedObject = await cdp.send('Runtime.awaitPromise', {
      promiseObjectId: plainPromise.result.objectId,
    });
    assert.equal(awaitedObject.result.value, 41);

    const topLevelAwait = await cdp.send('Runtime.evaluate', {
      expression: 'await Promise.resolve(43)',
    });
    assert.equal(topLevelAwait.result.subtype, 'promise');

    const awaitedEvaluation = await cdp.send('Runtime.evaluate', {
      expression: 'new Promise(resolve => setTimeout(() => resolve(44), 10))',
      awaitPromise: true,
    });
    assert.equal(awaitedEvaluation.result.value, 44);

    const awaitedThenable = await cdp.send('Runtime.evaluate', {
      expression: '({ then(resolve) { setTimeout(() => resolve(45), 10) } })',
      awaitPromise: true,
    });
    assert.equal(awaitedThenable.result.value, 45);

    const rejected = await cdp.send('Runtime.evaluate', {
      expression: "Promise.reject(new Error('inspector-await-boom'))",
      awaitPromise: true,
    });
    assert.ok(rejected.exceptionDetails);
    assert.match(JSON.stringify(rejected.exceptionDetails), /inspector-await-boom/);

    const pendingEvaluation = cdp.send('Runtime.evaluate', {
      expression: 'new Promise(resolve => { globalThis.__inspectorResolve = resolve })',
      awaitPromise: true,
    });
    const responsiveEvaluation = cdp.send('Runtime.evaluate', {
      expression: '6 * 7',
    });
    assert.equal(await Promise.race([
      pendingEvaluation.then(() => 'pending'),
      responsiveEvaluation.then(() => 'responsive'),
    ]), 'responsive');
    assert.equal((await responsiveEvaluation).result.value, 42);

    await cdp.send('Runtime.evaluate', {
      expression: 'globalThis.__inspectorResolve(47)',
    });
    assert.equal((await pendingEvaluation).result.value, 47);

    const compiled = await cdp.send('Runtime.compileScript', {
      expression: 'new Promise(resolve => setTimeout(() => resolve(48), 10))',
      sourceURL: 'ant://inspector/await-test',
      persistScript: true,
    });
    const run = await cdp.send('Runtime.runScript', {
      scriptId: compiled.scriptId,
      awaitPromise: true,
    });
    assert.equal(run.result.value, 48);

    cdp.send('Runtime.evaluate', {
      expression: 'new Promise(resolve => { globalThis.__abandonedInspectorResolve = resolve })',
      awaitPromise: true,
    });
    await cdp.send('Runtime.evaluate', { expression: '1' });
    cdp.socket.close();
    cdp = null;
    await delay(20);

    cdp = await connectCDP(url);
    await cdp.send('Runtime.evaluate', {
      expression: 'globalThis.__abandonedInspectorResolve(49)',
    });
    const afterReconnect = await cdp.send('Runtime.evaluate', {
      expression: '7 * 7',
    });
    assert.equal(afterReconnect.result.value, 49);

    console.log('inspector:await-promise:ok');
  } finally {
    if (cdp) cdp.socket.close();
    if (child.exitCode === null) child.kill('SIGTERM');
    await waitForExit(child);
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
