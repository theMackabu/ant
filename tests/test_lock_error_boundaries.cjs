const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const helpers = `
  function assert(condition, message) { if (!condition) throw new Error(message); }
  function rejected(promise, reason) {
    assert(promise instanceof Promise, 'lock request must return a Promise');
    return promise.then(
      () => { throw new Error('expected rejection'); },
      error => { assert(error === reason, 'original rejection changed'); }
    );
  }
  function throwingPromise(reason, species) {
    const promise = Promise.resolve(42);
    if (species) {
      promise.constructor = { get [Symbol.species]() { throw reason; } };
    } else {
      Object.defineProperty(promise, 'constructor', { get() { throw reason; } });
    }
    return promise;
  }
`;

const cases = {
  acquiredSpecies: `
    const checks = [];
    let index = 0;
    for (const reason of [new Error('constructor'), undefined, null]) {
      for (const species of [false, true]) {
        const name = 'species-' + index++;
        checks.push(rejected(navigator.locks.request(name, () => throwingPromise(reason, species)), reason));
        checks.push(navigator.locks.request(name, { ifAvailable: true }, lock => {
          assert(lock !== null, 'failed promise setup retained its lock');
        }));
      }
    }
    Promise.all(checks).then(finish);
  `,
  unavailableSpecies: `
    let release;
    const held = navigator.locks.request('unavailable-species', () => new Promise(resolve => { release = resolve; }));
    const checks = [];
    for (const reason of [new Error('constructor'), undefined, null]) {
      for (const species of [false, true]) {
        checks.push(rejected(navigator.locks.request('unavailable-species', { ifAvailable: true }, lock => {
          assert(lock === null, 'unavailable request acquired the lock');
          return throwingPromise(reason, species);
        }), reason));
      }
    }
    checks.push(navigator.locks.request('unavailable-species', { ifAvailable: true }, lock => {
      assert(lock === null, 'failed unavailable request released the owner');
    }));
    Promise.all(checks).then(() => { release(); return held; }).then(finish);
  `,
  emptyNameResolved: `
    let release;
    const held = navigator.locks.request('', () => new Promise(resolve => { release = resolve; }));
    navigator.locks.request('', { ifAvailable: true }, lock => {
      assert(lock === null, 'empty-name owner was not held');
      return Promise.resolve(42);
    }).then(value => {
      assert(value === 42, 'unavailable callback value changed');
      return navigator.locks.request('', { ifAvailable: true }, lock => {
        assert(lock === null, 'unavailable fulfillment released empty-name owner');
      });
    }).then(() => { release(); return held; }).then(finish);
  `,
  emptyNameRejected: `
    let release;
    const reason = new Error('unavailable callback');
    const held = navigator.locks.request('', () => new Promise(resolve => { release = resolve; }));
    rejected(navigator.locks.request('', { ifAvailable: true }, lock => {
      assert(lock === null, 'empty-name owner was not held');
      return Promise.reject(reason);
    }), reason).then(() => navigator.locks.request('', { ifAvailable: true }, lock => {
      assert(lock === null, 'unavailable rejection released empty-name owner');
    })).then(() => { release(); return held; }).then(finish);
  `,
};

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-lock-errors-'));
try {
  let index = 0;
  for (const [name, body] of Object.entries(cases)) {
    for (const observe of [false, true]) {
      const file = path.join(dir, `case${index++}.cjs`);
      fs.writeFileSync(file, helpers + `
        ${observe ? "process.on('uncaughtException', () => console.log('UNCAUGHT'));" : ''}
        process.on('unhandledRejection', () => console.log('UNHANDLED'));
        let queued = false;
        queueMicrotask(() => { queued = true; });
        const deadline = setTimeout(() => process.exit(2), 3000);
        function finish() {
          setTimeout(() => {
            assert(queued, 'queued work was dropped');
            clearTimeout(deadline);
            console.log('PASS');
          }, 10);
        }
        ${body}
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
