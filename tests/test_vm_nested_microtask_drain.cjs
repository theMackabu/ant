const assert = require('node:assert');
const vm = require('node:vm');

const log = [];
let depth = 0;

const context = vm.createContext({
  log,
  again() {
    if (depth++ > 0) return;
    vm.runInContext("Promise.resolve().then(() => log.push('C')); log.push('eval2')", context);
    log.push('after-nested');
  },
}, { microtaskMode: 'afterEvaluate' });

vm.runInContext(`
  Promise.resolve().then(() => { log.push('A'); again(); });
  Promise.resolve().then(() => log.push('B'));
  log.push('eval1');
`, context);

assert.deepStrictEqual(log, ['eval1', 'A', 'eval2', 'after-nested', 'B', 'C']);

function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) {
    const o = { i, s: 'x' + i, a: [i] };
    if ((i & 4095) === 0) keep = o;
  }
  return keep;
}

let ran = 0, bad = 0, runs = 0;
const stress = vm.createContext({
  churn,
  note(p, i) {
    ran++;
    if (p.id !== i || p.tag !== 'p' + i || p.list[1] !== i * 2) bad++;
  },
  again() {
    if (runs++ < 3) vm.runInContext(source, stress);
  },
}, { microtaskMode: 'afterEvaluate' });

const source = `
  for (let i = 0; i < 1500; i++) {
    const p = { id: i, tag: 'p' + i, list: [i, i * 2] };
    Promise.resolve().then(() => { note(p, i); if (i === 700) { again(); churn(200000); } });
  }
`;

vm.runInContext(source, stress);
assert.strictEqual(bad, 0);
assert.strictEqual(ran, 6000);
console.log('ok');
