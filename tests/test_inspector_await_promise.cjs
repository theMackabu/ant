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
  const weakChainLength = Number(process.env.ANT_WEAK_CHAIN_LENGTH || 512);
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

    await cdp.send('Runtime.evaluate', {
      expression: 'globalThis.__safeAwaitPromise = Promise.resolve(46)',
    });
    const safeAwaited = await cdp.send('Runtime.evaluate', {
      expression: 'globalThis.__safeAwaitPromise',
      throwOnSideEffect: true,
      awaitPromise: true,
    });
    assert.equal(safeAwaited.result.value, 46);

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__weakCollectionState = (() => {
        const map = new WeakMap();
        const liveKey = {};
        const liveValue = {};
        map.set(liveKey, liveValue);

        const cycleMap = new WeakMap();
        let cycleKey = {};
        let cycleValue = { key: cycleKey };
        cycleMap.set(cycleKey, cycleValue);

        const set = new WeakSet();
        let setKey = {};
        set.add(setKey);

        const state = {
          map,
          liveKey,
          liveValueRef: new WeakRef(liveValue),
          cycleMap,
          cycleKeyRef: new WeakRef(cycleKey),
          cycleValueRef: new WeakRef(cycleValue),
          set,
          setKeyRef: new WeakRef(setKey),
        };
        cycleKey = cycleValue = setKey = null;
        return state;
      })(); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const liveEphemeron = await cdp.send('Runtime.evaluate', {
      expression: 'Boolean(__weakCollectionState.liveValueRef.deref())',
    });
    assert.equal(liveEphemeron.result.value, true);
    await cdp.send('Runtime.evaluate', {
      expression: '__weakCollectionState.liveKey = null; 1',
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('HeapProfiler.collectGarbage');
    const deadWeakEntries = await cdp.send('Runtime.evaluate', {
      expression: `[
        __weakCollectionState.liveValueRef.deref(),
        __weakCollectionState.cycleKeyRef.deref(),
        __weakCollectionState.cycleValueRef.deref(),
        __weakCollectionState.setKeyRef.deref(),
      ].map(value => value === undefined).join(',')`,
    });
    assert.equal(
      deadWeakEntries.result.value,
      'true,true,true,true',
      String(deadWeakEntries.result.value)
    );

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__multiValueEphemeronState = (() => {
        const firstMap = new WeakMap();
        const secondMap = new WeakMap();
        const triggerMap = new WeakMap();
        const root = {};
        const key = {};
        const first = {};
        const second = {};
        firstMap.set(key, first);
        secondMap.set(key, second);
        triggerMap.set(root, key);
        return {
          firstMap,
          secondMap,
          triggerMap,
          root,
          firstRef: new WeakRef(first),
          secondRef: new WeakRef(second),
        };
      })(); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const liveMultiValueEphemeron = await cdp.send('Runtime.evaluate', {
      expression: `[
        __multiValueEphemeronState.firstRef.deref(),
        __multiValueEphemeronState.secondRef.deref(),
      ].every(value => value !== undefined)`,
    });
    assert.equal(liveMultiValueEphemeron.result.value, true);
    await cdp.send('Runtime.evaluate', {
      expression: '__multiValueEphemeronState.root = null; 1',
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('HeapProfiler.collectGarbage');
    const deadMultiValueEphemeron = await cdp.send('Runtime.evaluate', {
      expression: `[
        __multiValueEphemeronState.firstRef.deref(),
        __multiValueEphemeronState.secondRef.deref(),
      ].every(value => value === undefined)`,
    });
    assert.equal(deadMultiValueEphemeron.result.value, true);

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__typedWeakKeyState = (() => {
        const makeKey = [
          () => [],
          () => Promise.resolve(),
          () => (function* () {})(),
          () => function () {},
        ];
        return makeKey.map(createKey => {
          const map = new WeakMap();
          const root = {};
          const key = createKey();
          const value = {};
          map.set(key, value);
          map.set(root, key);
          return {
            map,
            set: new WeakSet([key]),
            root,
            valueRef: new WeakRef(value),
          };
        });
      })(); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const liveTypedKeyEphemerons = await cdp.send('Runtime.evaluate', {
      expression: `__typedWeakKeyState
        .map(state => {
          const key = state.map.get(state.root);
          return Boolean(state.valueRef.deref()) &&
            state.map.has(key) && state.set.has(key);
        })
        .join(',')`,
    });
    assert.equal(
      liveTypedKeyEphemerons.result.value,
      'true,true,true,true',
      String(liveTypedKeyEphemerons.result.value)
    );
    await cdp.send('Runtime.evaluate', {
      expression: '__typedWeakKeyState.forEach(state => { state.root = null }); 1',
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('HeapProfiler.collectGarbage');
    const deadTypedKeyEphemerons = await cdp.send('Runtime.evaluate', {
      expression: `__typedWeakKeyState
        .map(state => state.valueRef.deref() === undefined)
        .join(',')`,
    });
    assert.equal(
      deadTypedKeyEphemerons.result.value,
      'true,true,true,true',
      String(deadTypedKeyEphemerons.result.value)
    );

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__minorWeakState = Array.from(
        { length: 4 },
        () => ({ map: new WeakMap(), root: {}, valueRef: null })
      ); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('Runtime.evaluate', {
      expression: `(() => {
        const keys = [
          [],
          Promise.resolve(),
          (function* () {})(),
          function () {},
        ];
        __minorWeakState.forEach((state, index) => {
          const value = {};
          const key = keys[index];
          state.map.set(key, value);
          state.map.set(state.root, key);
          state.valueRef = new WeakRef(value);
        });
      })(); 1`,
    });
    await cdp.send('Runtime.evaluate', {
      expression: 'for (let i = 0; i < 250000; i++) ({ i }); 1',
    });
    const youngEphemeron = await cdp.send('Runtime.evaluate', {
      expression: `__minorWeakState
        .map(state => Boolean(state.valueRef.deref()))
        .join(',')`,
    });
    assert.equal(youngEphemeron.result.value, 'true,true,true,true');
    await cdp.send('Runtime.evaluate', {
      expression: `__minorWeakState.forEach(state => { state.root = null });
        for (let i = 0; i < 250000; i++) ({ i }); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const deadYoungEphemeron = await cdp.send('Runtime.evaluate', {
      expression: `__minorWeakState
        .map(state => state.valueRef.deref() === undefined)
        .join(',')`,
    });
    assert.equal(deadYoungEphemeron.result.value, 'true,true,true,true');

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__weakSymbolState = (() => {
        const map = new WeakMap();
        const owner = {};
        let key = Symbol('weak-key');
        const value = {};
        owner[key] = true;
        map.set(key, value);
        const set = new WeakSet([key]);
        const state = {
          map,
          set,
          owner,
          keyRef: new WeakRef(key),
          valueRef: new WeakRef(value),
        };
        key = null;
        return state;
      })(); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const liveSymbolEphemeron = await cdp.send('Runtime.evaluate', {
      expression: `Boolean(__weakSymbolState.valueRef.deref()) &&
        __weakSymbolState.set.has(Reflect.ownKeys(__weakSymbolState.owner)[0])`,
    });
    assert.equal(liveSymbolEphemeron.result.value, true);
    await cdp.send('Runtime.evaluate', {
      expression: '__weakSymbolState.owner = null; 1',
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('HeapProfiler.collectGarbage');
    const deadSymbolEphemeron = await cdp.send('Runtime.evaluate', {
      expression: `__weakSymbolState.keyRef.deref() === undefined &&
        __weakSymbolState.valueRef.deref() === undefined`,
    });
    assert.equal(deadSymbolEphemeron.result.value, true);

    const sameJobWeakRef = await cdp.send('Runtime.evaluate', {
      expression: `(() => {
        let target = {};
        const ref = new WeakRef(target);
        target = null;
        for (let i = 0; i < 250000; i++) ({ i });
        return Boolean(ref.deref());
      })()`,
    });
    assert.equal(sameJobWeakRef.result.value, true);

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__ephemeronChainState = (() => {
        const map = new WeakMap();
        const keys = Array.from({ length: ${weakChainLength} }, () => ({}));
        for (let i = keys.length - 2; i >= 0; i--)
          map.set(keys[i], keys[i + 1]);
        const state = {
          map,
          root: keys[0],
          tailRef: new WeakRef(keys[keys.length - 1]),
        };
        return state;
      })(); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const liveEphemeronChain = await cdp.send('Runtime.evaluate', {
      expression: 'Boolean(__ephemeronChainState.tailRef.deref())',
    });
    assert.equal(liveEphemeronChain.result.value, true);
    await cdp.send('Runtime.evaluate', {
      expression: '__ephemeronChainState.root = null; 1',
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('HeapProfiler.collectGarbage');
    const deadEphemeronChain = await cdp.send('Runtime.evaluate', {
      expression: '__ephemeronChainState.tailRef.deref() === undefined',
    });
    assert.equal(deadEphemeronChain.result.value, true);

    await cdp.send('Runtime.evaluate', {
      expression: `globalThis.__nestedEphemeronState = (() => {
        const outer = new WeakMap();
        const inner = new WeakMap();
        const root = {};
        const innerKey = {};
        const tail = {};
        inner.set(innerKey, tail);
        outer.set(root, inner);
        return {
          outer,
          root,
          innerKey,
          tailRef: new WeakRef(tail),
        };
      })(); 1`,
    });
    await cdp.send('HeapProfiler.collectGarbage');
    const liveNestedEphemeron = await cdp.send('Runtime.evaluate', {
      expression: 'Boolean(__nestedEphemeronState.tailRef.deref())',
    });
    assert.equal(liveNestedEphemeron.result.value, true);
    await cdp.send('Runtime.evaluate', {
      expression: '__nestedEphemeronState.root = __nestedEphemeronState.innerKey = null; 1',
    });
    await cdp.send('HeapProfiler.collectGarbage');
    await cdp.send('HeapProfiler.collectGarbage');
    const deadNestedEphemeron = await cdp.send('Runtime.evaluate', {
      expression: '__nestedEphemeronState.tailRef.deref() === undefined',
    });
    assert.equal(deadNestedEphemeron.result.value, true);

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
