const assert = require('node:assert');

const cases = [
  [0, '1970-01-01T00:00:00Z'], [1, '1970-01-01T00:00:00.001Z'], [-1, '1969-12-31T23:59:59.999Z'], [1500, '1970-01-01T00:00:01.5Z'],
  [1700000000123, '2023-11-14T22:13:20.123Z'], [8.64e15, '+275760-09-13T00:00:00Z'], [-8.64e15, '-271821-04-20T00:00:00Z'],
];
for (const [milliseconds, text] of cases) {
  const instant = new Date(milliseconds).toTemporalInstant();
  assert.ok(instant instanceof Temporal.Instant);
  assert.strictEqual(String(instant), text);
  assert.strictEqual(instant.epochMilliseconds, milliseconds);
  assert.strictEqual(instant.epochNanoseconds, BigInt(milliseconds) * 1000000n);
}

assert.throws(() => new Date(NaN).toTemporalInstant(), RangeError);
for (const receiver of [{}, 5, undefined, '2020', Temporal.Instant.fromEpochMilliseconds(0), Date.prototype, Object.create(Date.prototype), new Proxy(new Date(0), {})])
  assert.throws(() => Date.prototype.toTemporalInstant.call(receiver), TypeError);

assert.deepStrictEqual(Object.getOwnPropertyDescriptor(Date.prototype, 'toTemporalInstant').enumerable, false);
assert.deepStrictEqual([Date.prototype.toTemporalInstant.name, Date.prototype.toTemporalInstant.length], ['toTemporalInstant', 0]);

{
  class Stamp extends Date {}
  assert.strictEqual(String(new Stamp(5000).toTemporalInstant()), '1970-01-01T00:00:05Z');
  const date = new Date(0);
  assert.notStrictEqual(date.toTemporalInstant(), date.toTemporalInstant());
  date.setTime(86400000);
  assert.strictEqual(String(date.toTemporalInstant()), '1970-01-02T00:00:00Z');
  assert.ok(new Date(1234).toTemporalInstant().equals(Temporal.Instant.fromEpochMilliseconds(1234)));
}

console.log('ok');
