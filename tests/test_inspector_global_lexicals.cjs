const assert = require('node:assert');
const { spawn } = require('node:child_process');
const net = require('node:net');

async function reservePort() {
  const server = net.createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  await new Promise(resolve => server.close(resolve));
  return port;
}

async function connect(port, child, diagnostics) {
  let url;
  const deadline = Date.now() + 3000;
  while (!url && Date.now() < deadline && child.exitCode === null) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      url = targets[0]?.webSocketDebuggerUrl;
    } catch {}
    if (!url) await new Promise(resolve => setTimeout(resolve, 10));
  }
  assert.ok(url, diagnostics());
  const socket = new WebSocket(url);
  const pending = new Map();
  let nextId = 0;
  socket.onmessage = event => {
    const message = JSON.parse(String(event.data));
    const request = pending.get(message.id);
    if (!request) return;
    pending.delete(message.id);
    if (message.error) request.reject(new Error(message.error.message));
    else request.resolve(message.result);
  };
  socket.onclose = () => {
    for (const request of pending.values()) request.reject(new Error(diagnostics()));
    pending.clear();
  };
  await new Promise((resolve, reject) => {
    socket.onopen = resolve;
    socket.onerror = reject;
  });
  return {
    socket,
    send(method, params = {}) {
      const id = ++nextId;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        socket.send(JSON.stringify({ id, method, params }));
      });
    },
  };
}

async function main() {
  const port = await reservePort();
  const child = spawn(process.execPath, [
    `--inspect-wait=127.0.0.1:${port}`, '-e', 'setInterval(() => {}, 1000)',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', chunk => { output += chunk; });
  child.stderr.on('data', chunk => { output += chunk; });
  const timeout = setTimeout(() => child.kill('SIGKILL'), 10000);
  let cdp;
  try {
    cdp = await connect(port, child, () => output);
    const evaluate = (expression, safe = false) => cdp.send('Runtime.evaluate', {
      expression, throwOnSideEffect: safe,
    });
    const value = async (expression, safe = false) => {
      const response = await evaluate(expression, safe);
      assert.ok(!response.exceptionDetails, `${expression}: ${JSON.stringify(response)}`);
      return response.result.value;
    };

    assert.equal(await value('Math.PI', true), Math.PI);
    await value('let lexicalNumber = 41; const lexicalObject = { answer: 42 }; let lexicalUndefined;');
    assert.equal(await value('lexicalNumber', true), 41);
    assert.equal(await value('lexicalNumber + 1', true), 42);
    assert.equal(await value('lexicalObject.answer', true), 42);
    const undefinedRead = await evaluate('lexicalUndefined', true);
    assert.equal(undefinedRead.result.type, 'undefined');
    assert.ok(!undefinedRead.exceptionDetails);
    assert.ok((await evaluate('missingInspectorLexical', true)).exceptionDetails);

    await value('globalThis.shadowedLexical = 4; let shadowedLexical = 9;');
    assert.equal(await value('shadowedLexical', true), 9);
    assert.equal(await value('this.shadowedLexical', true), 4);
    const names = (await cdp.send('Runtime.globalLexicalScopeNames')).names;
    assert.ok(names.includes('lexicalNumber'));
    assert.equal(names.filter(name => name === 'shadowedLexical').length, 1);

    await value(`
      globalThis.inspectorGetterCalls = 0;
      Object.defineProperty(globalThis, 'shadowedGetter', {
        configurable: true, get() { inspectorGetterCalls++; return -1; }
      });
      const shadowedGetter = 17;
      const lexicalAccessor = { get value() { inspectorGetterCalls++; return 99; } };
      const lexicalProxy = new Proxy({}, { get() { inspectorGetterCalls++; return 99; } });
    `);
    assert.equal(await value('shadowedGetter', true), 17);
    assert.ok((await evaluate('lexicalAccessor.value', true)).exceptionDetails);
    assert.ok((await evaluate('lexicalProxy.value', true)).exceptionDetails);
    assert.equal(await value('inspectorGetterCalls'), 0);

    await value('globalThis.uninitializedLexical = 88;');
    assert.ok((await evaluate('throw 1; let uninitializedLexical;')).exceptionDetails);
    assert.ok((await evaluate('uninitializedLexical', true)).exceptionDetails);
    assert.equal(await value('this.uninitializedLexical', true), 88);

    await value('let annexLexical = 7;');
    await value('{ function annexLexical() {} }');
    await value('(0, eval)("{ function annexLexical() {} }")');
    assert.equal(await value('annexLexical'), 7);
    assert.equal(await value('typeof this.annexLexical'), 'undefined');

    for (let batch = 0; batch < 5; batch++) {
      const declarations = Array.from({ length: 32 }, (_, offset) => {
        const index = batch * 32 + offset;
        return `let indexedLexical${index} = ${index};`;
      }).join('');
      await value(declarations);
    }
    await cdp.send('HeapProfiler.collectGarbage');
    const identifiers = Array.from({ length: 160 }, (_, i) => `indexedLexical${i}`);
    assert.equal(await value(`[${identifiers}].every((value, i) => value === i)`), true);
    assert.equal(await value('indexedLexical0 = 100; indexedLexical159 = 200; indexedLexical0 + indexedLexical159'), 300);
    assert.ok((await evaluate('let indexedLexical0 = 5;')).exceptionDetails);
    assert.ok((await evaluate('lexicalObject = {};')).exceptionDetails);
    assert.equal(await value('lexicalObject.answer', true), 42);
    await value("import { sep as lexicalSeparator } from 'node:path';");
    assert.equal(await value('lexicalSeparator', true), require('node:path').sep);
    console.log('inspector global lexical resolution passed');
  } finally {
    clearTimeout(timeout);
    if (cdp) cdp.socket.close();
    if (child.exitCode === null) {
      const exited = new Promise(resolve => child.once('exit', resolve));
      child.kill('SIGTERM');
      await exited;
    }
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
