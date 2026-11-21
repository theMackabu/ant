const assert = require('node:assert');

assert.throws(() => FinalizationRegistry(() => {}), TypeError);
assert.throws(() => new FinalizationRegistry(1), TypeError);
const probe = new FinalizationRegistry(() => {});
assert.throws(() => probe.register(1, 'x'), TypeError);
const same = {};
assert.throws(() => probe.register(same, same), TypeError);
assert.throws(() => probe.register({}, 'x', 1), TypeError);
assert.throws(() => probe.unregister(1), TypeError);
assert.throws(() => FinalizationRegistry.prototype.register.call({}, {}, 1), TypeError);
assert.strictEqual(probe.unregister({}), false);
assert.strictEqual(Object.prototype.toString.call(probe), '[object FinalizationRegistry]');

const seen = [];
let inCallback = 0;
const registry = new FinalizationRegistry((held) => { inCallback++; seen.push(held); });
const kept = [];
const tokens = [];

(function fill() {
  for (let i = 0; i < 600; i++) registry.register({ i }, 'dead' + i);
  for (let i = 0; i < 50; i++) { const target = { i }; kept.push(target); registry.register(target, 'kept' + i); }
  for (let i = 0; i < 50; i++) { const token = { i }; tokens.push(token); registry.register({ i }, 'unregistered' + i, token); }
  const selfToken = { self: true };
  registry.register(selfToken, 'self-token', selfToken);
  assert.strictEqual(registry.unregister(selfToken), true);
  assert.strictEqual(registry.unregister(selfToken), false);
})();

for (const token of tokens) assert.strictEqual(registry.unregister(token), true);
assert.strictEqual(inCallback, 0);

let rounds = 0;
function churn() {
  const junk = [];
  for (let i = 0; i < 150000; i++) junk.push({ i });
  if (seen.length === 0 && ++rounds < 60) return setTimeout(churn, 2);
  setTimeout(finish, 20);
}

function finish() {
  assert.ok(seen.length > 0, 'no finalization callback ran');
  assert.ok(seen.length <= 600);
  assert.strictEqual(new Set(seen).size, seen.length);
  for (const held of seen) assert.ok(/^dead\d+$/.test(held), held);
  assert.strictEqual(kept.length, 50);
  console.log('ok');
}

churn();
