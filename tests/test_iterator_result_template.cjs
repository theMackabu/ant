const assert = require('node:assert');

for (const iterable of [[7, 8], 'ab', new Uint8Array([7, 8])]) {
  const iterator = iterable[Symbol.iterator]();
  const first = iterator.next();
  const savedValue = first.value;
  const originalPrototype = Object.getPrototypeOf(first);
  assert.deepStrictEqual(Object.keys(first), ['done', 'value']);
  for (const key of ['done', 'value']) {
    const descriptor = Object.getOwnPropertyDescriptor(first, key);
    assert.strictEqual(descriptor.writable, true);
    assert.strictEqual(descriptor.enumerable, true);
    assert.strictEqual(descriptor.configurable, true);
  }
  Object.defineProperty(first, 'value', { get() { return 'changed'; } });
  Object.setPrototypeOf(first, { changed: true });
  delete first.done;
  const second = iterator.next();
  assert.notStrictEqual(first, second);
  assert.strictEqual(Object.getPrototypeOf(second), originalPrototype);
  assert.strictEqual(second.done, false);
  assert.notStrictEqual(second.value, savedValue);
  const end = iterator.next();
  assert.strictEqual(end.done, true);
  assert.strictEqual(end.value, undefined);
  assert.deepStrictEqual(Object.keys(end), ['done', 'value']);
  assert.notStrictEqual(end, iterator.next());
}

const held = [];
for (let i = 0; i < 20000; i++) {
  const value = { index: i };
  held.push([value][Symbol.iterator]().next());
}
for (let i = 0; i < held.length; i++) {
  assert.strictEqual(held[i].done, false);
  assert.strictEqual(held[i].value.index, i);
}
console.log('iterator:result-template:ok');
