// Native modules must consume handled throws before calling observers or settling promises.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-module-errors-'));
const helpers = `
  function assert(condition, message) { if (!condition) throw new Error(message); }
  function rejected(promise, expected) {
    assert(promise instanceof Promise, 'expected a Promise');
    return promise.then(
      () => { throw new Error('expected rejection'); },
      error => {
        if (typeof expected === 'function') assert(error instanceof expected, 'wrong error type');
        else assert(error === expected, 'lost original rejection reason');
      }
    );
  }
`;

const cases = {
  timerAbort: `
    const timers = require('node:timers/promises');
    for (const immediate of [false, true]) {
      for (const preAborted of [false, true]) {
        const controller = new AbortController();
        if (preAborted) controller.abort(null);
        const promise = immediate
          ? timers.setImmediate(42, { signal: controller.signal })
          : timers.setTimeout(100, 42, { signal: controller.signal });
        const handled = rejected(promise, TypeError);
        if (!preAborted) controller.abort(null);
        await handled;
      }
    }
    assert(await timers.setTimeout(0, 42) === 42, 'timer success changed');
  `,
  cryptoCallbacks: `
    const crypto = require('node:crypto');
    for (const invoke of [
      cb => crypto.pbkdf2('x', 'y', 0, 16, 'sha256', cb),
      cb => crypto.scrypt('x', 'y', -1, cb)
    ]) {
      let calls = 0;
      invoke((error, value) => {
        assert(error instanceof Error, 'callback received a throw marker');
        assert(value === undefined, 'failed crypto operation returned a value');
        calls++;
      });
      assert(calls === 1, 'crypto callback was skipped');
      const reason = new Error('callback throw');
      let caught = false;
      try { invoke(() => { throw reason; }); }
      catch (error) { caught = true; assert(error === reason, 'callback throw was replaced'); }
      assert(caught, 'callback throw was swallowed');
    }
    for (const reason of [new Error('crypto getter'), undefined, null]) {
      let called = false;
      crypto.scrypt('x', 'y', 16, { get N() { throw reason; } }, (error, value) => {
        assert(error === reason && value === undefined, 'crypto getter throw lost its reason');
        called = true;
      });
      assert(called, 'crypto getter failure skipped callback');
    }
  `,
  lockThrows: `
    for (const reason of [new Error('lock callback'), undefined, null]) {
      await rejected(navigator.locks.request('errors', () => { throw reason; }), reason);
      assert(await navigator.locks.request('errors', () => 42) === 42, 'lock was not released');
    }
    let release;
    const held = navigator.locks.request('queued', () => new Promise(resolve => { release = resolve; }));
    const queuedReason = new Error('queued callback');
    const queued = rejected(navigator.locks.request('queued', () => { throw queuedReason; }), queuedReason);
    release();
    await held;
    await queued;
  `,
  unavailableLockThrows: `
    let release;
    const held = navigator.locks.request('busy', () => new Promise(resolve => { release = resolve; }));
    for (const reason of [new Error('unavailable lock'), undefined, null]) {
      await rejected(navigator.locks.request('busy', { ifAvailable: true }, lock => {
        assert(lock === null, 'unavailable lock must be null');
        throw reason;
      }), reason);
    }
    const reason = new Error('unavailable async lock');
    await rejected(navigator.locks.request('busy', { ifAvailable: true }, () => Promise.reject(reason)), reason);
    release();
    await held;
  `,
  sandboxValidation: `
    const { Sandbox } = require('ant:sandbox');
    for (const method of ['run', 'eval', 'receive', 'once']) {
      await rejected(Sandbox.prototype[method].call({}), TypeError);
    }
  `,
  workerMessageFallback: `
    const fs = require('node:fs');
    const { Worker } = require('node:worker_threads');
    const filename = require('node:path').join(__dirname, 'worker-fallback.cjs');
    fs.writeFileSync(filename, "process.stdout.write('ANT_WT_MSG:not-json\\n');");
    const worker = new Worker(filename);
    let message;
    worker.on('message', value => { message = value; });
    await new Promise(resolve => worker.on('exit', resolve));
    assert(message === 'not-json', 'invalid worker JSON should fall back to text');
  `,
  observableReports: `
    const reason = new Error('observer callback');
    const values = [];
    let completed = false;
    new Observable(observer => {
      observer.next(1);
      observer.next(2);
      observer.complete();
      return () => { throw reason; };
    }).subscribe({
      start() { throw reason; },
      next(value) { values.push(value); if (value === 1) throw reason; },
      error() { throw new Error('observer throw incorrectly reached error callback'); },
      complete() { completed = true; throw reason; }
    });
    assert(values.join(',') === '1,2' && completed, 'observer throw interrupted delivery');
    let errorDelivered = false;
    new Observable(() => { throw reason; }).subscribe({
      error(error) { errorDelivered = error === reason; throw reason; }
    });
    assert(errorDelivered, 'subscriber throw lost its reason');
    let cleanupError;
    new Observable(() => ({ get unsubscribe() { throw reason; } })).subscribe({
      error(error) { cleanupError = error; }
    });
    assert(cleanupError === reason, 'cleanup getter throw lost its reason');
  `,
  observerDiagnosticGetters: `
    let completed = 0;
    let queued = 0;
    for (const property of ['stack', 'name', 'message']) {
      for (const reason of [undefined, null, new Error('diagnostic getter')]) {
        const error = new Error('observer failure');
        Object.defineProperty(error, 'stack', { value: undefined, configurable: true });
        Object.defineProperty(error, property, {
          get() { queueMicrotask(() => { queued++; }); throw reason; },
          configurable: true
        });
        new Observable(observer => {
          observer.next(1);
          observer.complete();
        }).subscribe({
          next() { throw error; },
          complete() { completed++; }
        });
      }
    }
    await new Promise(resolve => setTimeout(resolve, 0));
    assert(completed === 9, 'diagnostic getter throw interrupted observer completion');
    assert(queued >= 9, 'diagnostic getter queued work did not run');
  `,
};

try {
  let index = 0;
  for (const [name, body] of Object.entries(cases)) {
    for (const observeUncaught of [false, true]) {
      const file = path.join(dir, `case${index++}.cjs`);
      fs.writeFileSync(file, helpers + `
        ${observeUncaught ? "process.on('uncaughtException', () => console.log('UNCAUGHT'));" : ''}
        process.on('unhandledRejection', () => console.log('UNHANDLED'));
        const deadline = setTimeout(() => process.exit(2), 3000);
        (async () => {
          ${body}
          await new Promise(resolve => setTimeout(resolve, 10));
          console.log('PASS');
        })().catch(error => {
          console.error(error && error.stack || error);
          process.exitCode = 1;
        }).finally(() => clearTimeout(deadline));
      `);
      const result = spawnSync(process.execPath, [file], { encoding: 'utf8', timeout: 5000 });
      const label = name + ' (observer: ' + observeUncaught + ')';
      assert(result.status === 0, label + ': ' + result.stderr);
      assert(result.stdout.includes('PASS'), label + ': incomplete: ' + result.stdout);
      assert(!result.stdout.includes('UNCAUGHT'), label + ': ' + result.stdout);
      assert(!result.stdout.includes('UNHANDLED'), label + ': ' + result.stdout);
    }
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log('PASS');
