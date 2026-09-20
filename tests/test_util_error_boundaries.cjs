// Exercise native conversions at top level so stale throws cannot be hidden by await.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const cases = {
  promisifyCustomGetter: `
    const { promisify } = require('node:util');
    for (const reason of [new Error('custom getter'), undefined, null]) {
      const original = () => {};
      Object.defineProperty(original, Symbol.for('nodejs.util.promisify.custom'), {
        get() { throw reason; }
      });
      let caught = false;
      try { promisify(original); }
      catch (error) { caught = true; assert(error === reason, 'lost custom getter throw'); }
      assert(caught, 'custom getter throw was swallowed');
    }
  `,
  parseArgsGetters: `
    const { parseArgs } = require('node:util');
    for (const reason of [new Error('option getter'), undefined, null]) {
      for (const property of ['type', 'short', 'multiple', 'default']) {
        const option = {};
        Object.defineProperty(option, property, { get() { throw reason; } });
        let caught = false;
        try { parseArgs({ args: [], options: { x: option } }); }
        catch (error) { caught = true; assert(error === reason, 'lost option getter throw'); }
        assert(caught, 'option getter throw was swallowed');
      }
      for (const property of ['strict', 'allowPositionals']) {
        const config = { args: [], options: {} };
        Object.defineProperty(config, property, { get() { throw reason; } });
        let caught = false;
        try { parseArgs(config); }
        catch (error) { caught = true; assert(error === reason, 'lost config getter throw'); }
        assert(caught, 'config getter throw was swallowed');
      }
    }
  `,
  settledPromisify: `
    const { promisify } = require('node:util');
    const reason = new Error('throw after settlement');
    for (const value of [reason, undefined, null]) {
      for (const reject of [false, true]) {
        const promise = promisify(callback => {
          callback(reject ? reason : null, 42);
          throw value;
        })();
        assert(promise instanceof Promise, 'promisify must return a Promise');
        promise.then(
          result => { assert(!reject && result === 42, 'settled result changed'); completed++; },
          error => { assert(reject && error === reason, 'settled reason changed'); completed++; }
        );
      }
    }
    expected = 6;
  `,
  unsettledPromisify: `
    const { promisify } = require('node:util');
    for (const reason of [new Error('original'), undefined, null]) {
      const promise = promisify(() => { throw reason; })();
      assert(promise instanceof Promise, 'promisify must return a Promise');
      promise.then(
        () => { throw new Error('expected rejection'); },
        error => { assert(error === reason, 'lost original throw'); completed++; }
      );
    }
    expected = 3;
  `,
  callbackThrows: `
    const { callbackify } = require('node:util');
    for (const reason of [new Error('callback'), undefined, null]) {
      for (const original of [() => 42, () => { throw new Error('original'); }]) {
        let caught = false;
        try { callbackify(original)(() => { throw reason; }); }
        catch (error) { caught = true; assert(error === reason, 'lost callback throw'); }
        assert(caught, 'callback throw was swallowed');
      }
    }
    const reason = new Error('original');
    const result = callbackify(() => { throw reason; })(error => {
      assert(error === reason, 'lost caught error');
      return 99;
    });
    assert(result === undefined, 'callback return leaked');
  `,
  callbackifySpecies: `
    const { callbackify } = require('node:util');
    for (const reason of [new Error('species'), undefined, null]) {
      const promise = Promise.resolve(42);
      Object.defineProperty(promise, 'constructor', { get() { throw reason; } });
      let caught = false;
      try { callbackify(() => promise)(() => { throw new Error('unexpected callback'); }); }
      catch (error) { caught = true; assert(error === reason, 'lost species throw'); }
      assert(caught, 'species throw was swallowed');
    }
  `,
  abortedValidation: `
    const { aborted } = require('node:util');
    for (const args of [[], [{}], [null]]) {
      const promise = aborted(...args);
      assert(promise instanceof Promise, 'aborted must return a Promise');
      promise.then(
        () => { throw new Error('invalid signal should reject'); },
        error => { assert(error instanceof TypeError, 'wrong signal error'); completed++; }
      );
    }
    expected = 3;
  `,
};

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-util-errors-'));
try {
  let index = 0;
  for (const [name, body] of Object.entries(cases)) {
    for (const observe of [false, true]) {
      const file = path.join(dir, `case${index++}.cjs`);
      fs.writeFileSync(file, `
        function assert(condition, message) { if (!condition) throw new Error(message); }
        ${observe ? "process.on('uncaughtException', () => console.log('UNCAUGHT'));" : ''}
        process.on('unhandledRejection', () => console.log('UNHANDLED'));
        let completed = 0, expected = 0;
        let before = false, after = false;
        queueMicrotask(() => { before = true; });
        ${body}
        queueMicrotask(() => { after = true; });
        setTimeout(() => {
          assert(before && after, 'queued work was dropped');
          assert(completed === expected, 'promise handlers did not finish');
          console.log('PASS');
        }, 10);
      `);
      const result = spawnSync(process.execPath, [file], { encoding: 'utf8', timeout: 5000 });
      const label = name + ' (observer: ' + observe + ')';
      assert(result.status === 0, label + ': ' + JSON.stringify(result));
      assert(result.stdout.includes('PASS'), label + ': incomplete: ' + result.stdout);
      assert(!result.stdout.includes('UNCAUGHT'), label + ': ' + result.stdout);
      assert(!result.stdout.includes('UNHANDLED'), label + ': ' + result.stdout);
    }
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log('PASS');
