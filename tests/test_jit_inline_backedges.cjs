const assert = require('node:assert');

function lookup(key, list) {
  while (list !== null) {
    if (list.entry.key === key) return list.entry;
    list = list.next;
  }
  return false;
}

function lastEntry(list) {
  do { list = list.next; } while (list.next !== null);
  return list.entry;
}

const tail = { entry: { key: 'tail', value: 3 }, next: null };
const middle = { entry: { key: 'middle', value: 2 }, next: tail };
const head = { entry: { key: 'head', value: 1 }, next: middle };

// Warm the callees before compiling callers that could try to inline them.
for (let i = 0; i < 500; i++) {
  assert.strictEqual(lookup('tail', head), tail.entry);
  assert.strictEqual(lastEntry(head), tail.entry);
}

function lookupCaller(key, list) {
  const entry = lookup(key, list);
  return entry === false ? -1 : entry.value;
}
function lastCaller(list) { return lastEntry(list).value; }

for (let i = 0; i < 1000; i++) {
  assert.strictEqual(lookupCaller('head', head), 1);
  assert.strictEqual(lookupCaller('middle', head), 2);
  assert.strictEqual(lookupCaller('tail', head), 3);
  assert.strictEqual(lookupCaller('absent', head), -1);
  assert.strictEqual(lookupCaller('absent', null), -1);
  assert.strictEqual(lastCaller(head), 3);
}
console.log('PASS inline backward branches');
