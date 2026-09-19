// Native errors consumed by promise APIs must not also escape the event loop.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-async-errors-'));
let index = 0;
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
  invalidDateInspection: `
    const inspect = require('node:util').inspect;
    for (const value of [NaN, Infinity, -Infinity]) {
      assert(inspect(new Date(value)) === 'Invalid Date', 'invalid date inspection failed');
    }
    console.log(new Date(NaN));
  `,
  cronRegistrationFailures: `
    const missing = require('node:path').join(__dirname, 'missing-worker.mjs');
    function registration(index) {
      const promise = Ant.cron(missing, '@daily', 'missing_' + index);
      assert(promise instanceof Promise, 'cron registration must return a Promise');
      return promise.then(
        () => { throw new Error('missing cron script should reject'); },
        error => {
          assert(error instanceof TypeError, 'wrong cron rejection type');
          assert(error.message.includes('cannot resolve cron script'), 'lost cron error message');
        }
      );
    }
    await Promise.all([registration(0), registration(1), registration(2)]);
    await registration(3);
  `,
  fetchRefused: `
    const server = require('node:net').createServer();
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const port = server.address().port;
    await new Promise(resolve => server.close(resolve));
    await rejected(fetch('http://127.0.0.1:' + port + '/'), TypeError);
  `,
  fetchValidation: `
    await rejected(fetch('file:///unused'), TypeError);
    await rejected(fetch('data:application/octet-stream;base64,%%%%'), TypeError);
    const url = URL.createObjectURL(new Blob(['x']));
    URL.revokeObjectURL(url);
    await rejected(fetch(url), TypeError);
  `,
  lockedBodies: `
    const request = new Request('http://localhost/', {
      method: 'POST', body: new ReadableStream(), duplex: 'half'
    });
    const reader = request.body.getReader();
    await rejected(request.text(), TypeError);
    reader.releaseLock();
    const response = new Response(new ReadableStream());
    const responseReader = response.body.getReader();
    await rejected(response.text(), TypeError);
    responseReader.releaseLock();
  `,
  promiseSelfResolution: `
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    const handled = rejected(promise, TypeError);
    setTimeout(() => resolve(promise), 0);
    await handled;
  `,
  promiseChainCycle: `
    let promise;
    promise = Promise.resolve().then(() => promise);
    await rejected(promise, TypeError);
  `,
  iteratorCallbacks: `
    const reason = new Error('iterator callback');
    for (const method of ['map', 'filter', 'flatMap']) {
      const iter = (async function* () { yield 1; })();
      await rejected(iter[method](() => { throw reason; }).next(), reason);
    }
    await rejected((async function* () {})().reduce((a, b) => a + b), TypeError);
    await rejected((async function* () { yield 1; })().forEach(() => { throw undefined; }), undefined);
  `,
  iteratorGetters: `
    const reason = new Error('iterator getter');
    for (const key of ['done', 'value']) {
      const source = { next() { return {
        get [key]() { throw reason; }
      }; } };
      await rejected(AsyncIterator.from(source).map(x => x).next(), reason);
      await rejected(AsyncIterator.from(source).toArray(), reason);
    }
  `,
  eventsOnce: `
    const { once } = require('node:events');
    await rejected(once({}, 'x'), TypeError);
    const reason = new Error('event registration');
    await rejected(once({ on() {}, once() { throw reason; } }, 'x'), reason);
    await rejected(once({ get on() { throw reason; } }, 'x'), reason);
    await rejected(once({ on() {}, get once() { throw reason; } }, 'x'), reason);
  `,
  assertionRejections: `
    const a = require('node:assert');
    await rejected(a.rejects(Promise.resolve()), Error);
    await rejected(a.doesNotReject(Promise.reject(new Error('failure'))), Error);
    const reason = new Error('synchronous assertion callback');
    await rejected(a.rejects(() => { throw reason; }), reason);
    await rejected(a.doesNotReject(() => { throw reason; }), reason);
  `,
  wasmValidation: `
    await rejected(WebAssembly.compile(), TypeError);
    await rejected(WebAssembly.instantiate(), TypeError);
  `,
  promiseControls: `
    const reason = new Error('ordinary rejection');
    await rejected(Promise.resolve().then(() => { throw reason; }), reason);
    await rejected(Promise.resolve({ get then() { throw reason; } }), reason);
    assert(await Promise.resolve(42) === 42, 'successful resolution changed');
  `,
};

try {
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
          console.error(error.stack || error);
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
