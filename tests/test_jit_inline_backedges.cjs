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

// Small pure numeric callees: unconditional and conditional backedges must
// remain safe when their already-warm callers become inline candidates.
function sumWhile(n) {
  let sum = 0;
  while (n > 0) { sum += n; n--; }
  return sum;
}
function sumDoWhile(n) {
  let sum = 0;
  do { sum += n; n--; } while (n > 0);
  return sum;
}
for (let i = 0; i < 500; i++) {
  assert.strictEqual(sumWhile(7), 28);
  assert.strictEqual(sumDoWhile(7), 28);
}
function whileCaller(n) { return sumWhile(n) * 3 + 1; }
function doWhileCaller(n) { return sumDoWhile(n) * 3 + 1; }
for (let i = 0; i < 5000; i++) {
  const n = i & 7;
  const expected = n * (n + 1) / 2 * 3 + 1;
  assert.strictEqual(whileCaller(n), expected);
  assert.strictEqual(doWhileCaller(n), expected);
}
console.log('PASS small numeric inline backward branches');
