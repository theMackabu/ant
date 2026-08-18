const assert = require('node:assert');
const isTypeError = error => error instanceof TypeError;

assert.strictEqual(typeof Ant.cron, 'function');
assert.strictEqual(Ant.cron.name, 'cron');
assert.strictEqual(Ant.cron.length, 3);
assert.strictEqual(typeof Ant.cron.parse, 'function');
assert.strictEqual(Ant.cron.parse.length, 1);
assert.strictEqual(typeof Ant.cron.remove, 'function');
assert.strictEqual(Ant.cron.remove.length, 1);
assert.strictEqual(Object.getOwnPropertyDescriptor(Ant, 'cron').writable, true);

const utc = { tz: 'UTC' };
const at = value => new Date(value);
const next = (expression, relative) => Ant.cron.parse(expression, at(relative), utc);
const iso = (expression, relative) => next(expression, relative)?.toISOString() ?? null;

assert.strictEqual(iso('*/15 * * * *', '2026-01-01T00:00:01Z'), '2026-01-01T00:15:00.000Z');
assert.strictEqual(iso('1,15 * * * *', '2026-01-01T00:00:01Z'), '2026-01-01T00:01:00.000Z');
assert.strictEqual(iso('9-17 * * * *', '2026-01-01T00:00:01Z'), '2026-01-01T00:09:00.000Z');
assert.strictEqual(iso('5/10 * * * *', '2026-01-01T00:00:01Z'), '2026-01-01T00:05:00.000Z');
assert.strictEqual(iso('0 9 * * Monday-Friday', '2026-01-01T00:00:01Z'), '2026-01-01T09:00:00.000Z');
assert.strictEqual(iso('0 0 1 JAN,JUN *', '2026-01-01T00:00:01Z'), '2026-06-01T00:00:00.000Z');
assert.strictEqual(iso('0 0 * * 7', '2026-01-01T00:00:01Z'), '2026-01-04T00:00:00.000Z');
assert.strictEqual(iso('0 0 15 * FRI', '2026-01-01T00:00:01Z'), '2026-01-02T00:00:00.000Z');

const nicknames = new Map([
  ['@yearly', '2027-01-01T00:00:00.000Z'],
  ['@annually', '2027-01-01T00:00:00.000Z'],
  ['@monthly', '2026-02-01T00:00:00.000Z'],
  ['@weekly', '2026-01-04T00:00:00.000Z'],
  ['@daily', '2026-01-02T00:00:00.000Z'],
  ['@midnight', '2026-01-02T00:00:00.000Z'],
  ['@hourly', '2026-01-01T01:00:00.000Z'],
]);
for (const [expression, expected] of nicknames) {
  assert.strictEqual(iso(expression, '2026-01-01T00:00:01Z'), expected);
}

let cursor = at('2026-01-01T00:00:01Z');
const hours = [];
for (let i = 0; i < 3; i++) {
  cursor = Ant.cron.parse('0 * * * *', cursor, utc);
  hours.push(cursor.toISOString());
}
assert.deepStrictEqual(hours, [
  '2026-01-01T01:00:00.000Z',
  '2026-01-01T02:00:00.000Z',
  '2026-01-01T03:00:00.000Z',
]);

assert.strictEqual(next('0 0 30 2 *', '2026-01-01T00:00:00Z'), null);
assert.throws(() => Ant.cron.parse('60 * * * *'), isTypeError);
assert.throws(() => Ant.cron.parse('* * * *'), isTypeError);
assert.throws(() => Ant.cron.parse('0 0 * FOO *'), isTypeError);
assert.throws(() => Ant.cron.parse('1, * * * *'), isTypeError);
assert.strictEqual(
  Ant.cron.parse(' @daily ', at('2026-01-01T00:00:01Z'), utc).toISOString(),
  '2026-01-02T00:00:00.000Z'
);
assert.strictEqual(
  Ant.cron.parse('*/61 * * * *', at('2026-01-01T00:00:01Z'), utc).toISOString(),
  '2026-01-01T01:00:00.000Z'
);
assert.strictEqual(
  Ant.cron.parse('+1 * * * *', at('2026-01-01T00:00:01Z'), utc).toISOString(),
  '2026-01-01T00:01:00.000Z'
);
assert.throws(() => Ant.cron.parse('* * * * *', {}), isTypeError);
assert.throws(() => Ant.cron.parse('* * * * *', 0, { tz: 'Not/A_Zone' }), isTypeError);
assert.throws(() => Ant.cron.parse('* * * * *', 0, { tz: '+05:30' }), isTypeError);
assert.doesNotThrow(() => Ant.cron.parse('* * * * *', null));
assert.strictEqual(
  Ant.cron.parse('0 9 * * *', at('2026-01-01T00:00:00Z'), { tz: null }).getTime(),
  Ant.cron.parse('0 9 * * *', at('2026-01-01T00:00:00Z')).getTime()
);
assert.strictEqual(Ant.cron.parse('* * * * *', 8_640_000_000_000_000, utc), null);
assert.strictEqual(Ant.cron.parse('0 0 1 1 *', -1, utc).getTime(), 0);
assert.strictEqual(Ant.cron.parse('0 0 1 1 *', 8_639_999_999_999_999, utc), null);
assert.throws(
  () => Ant.cron.parse('* * * * *', 8_640_000_000_000_001, utc),
  isTypeError
);
assert.throws(() => Ant.cron.parse('*/128 * * * *'), isTypeError);
assert.throws(
  () => Ant.cron.parse('* * * * *', 0, {
    get tz() { throw new RangeError('getter boom'); },
  }),
  error => error instanceof RangeError && error.message === 'getter boom'
);
assert.throws(() => Ant.cron(), isTypeError);
assert.throws(() => Ant.cron('* * * * *'), isTypeError);
assert.throws(() => Ant.cron('* * * * *', 1), isTypeError);
assert.throws(() => Ant.cron.remove(), isTypeError);
assert.throws(() => Ant.cron.remove('bad title'), isTypeError);

assert.strictEqual(
  Ant.cron.parse('30 2 * * *', at('2026-03-08T09:00:00Z'), {
    tz: 'America/Los_Angeles',
  }).toISOString(),
  '2026-03-08T10:30:00.000Z'
);
for (const relative of ['2026-03-08T10:00:00Z', '2026-03-08T10:30:00Z']) {
  assert.strictEqual(
    Ant.cron.parse('*/15 2 * * *', at(relative), {
      tz: 'America/Los_Angeles',
    }).toISOString(),
    '2026-03-09T09:00:00.000Z'
  );
}
assert.strictEqual(
  Ant.cron.parse('30 1 * * *', at('2026-11-01T08:30:00Z'), {
    tz: 'America/Los_Angeles',
  }).toISOString(),
  '2026-11-02T09:30:00.000Z'
);
assert.strictEqual(
  Ant.cron.parse('* * * * *', at('2026-11-01T08:59:00Z'), {
    tz: 'America/Los_Angeles',
  }).toISOString(),
  '2026-11-01T09:00:00.000Z'
);
assert.strictEqual(
  Ant.cron.parse('30 1 * * *', at('2026-11-01T09:00:00Z'), {
    tz: 'America/Los_Angeles',
  }).toISOString(),
  '2026-11-02T09:30:00.000Z'
);
assert.strictEqual(
  Ant.cron.parse('0 */1 * * *', at('2026-11-01T08:29:00Z'), {
    tz: 'America/Los_Angeles',
  }).toISOString(),
  '2026-11-01T09:00:00.000Z'
);

assert.throws(() => Ant.cron('0 0 30 2 *', () => {}), isTypeError);
const job = Ant.cron('@yearly', function () {});
assert.strictEqual(job.cron, '@yearly');
assert.strictEqual(Object.prototype.toString.call(job), '[object CronJob]');
assert.strictEqual(job.unref(), job);
assert.strictEqual(job.ref(), job);

const prototype = Object.getPrototypeOf(job);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(prototype, Symbol.toStringTag), {
  value: 'CronJob',
  writable: false,
  enumerable: false,
  configurable: true,
});
assert.strictEqual(Object.getOwnPropertyDescriptor(prototype, 'cron').get.name, 'get cron');
assert.strictEqual(prototype[Symbol.dispose].name, 'dispose');
assert.strictEqual(prototype[Symbol.dispose].length, 1);
for (const method of ['ref', 'stop', 'unref']) {
  assert.throws(() => prototype[method].call({}), isTypeError);
  const descriptor = Object.getOwnPropertyDescriptor(prototype, method);
  assert.strictEqual(descriptor.writable, true);
  assert.strictEqual(descriptor.enumerable, true);
  assert.strictEqual(descriptor.configurable, false);
}
assert.throws(() => Object.getOwnPropertyDescriptor(prototype, 'cron').get.call({}), isTypeError);
assert.strictEqual(job.stop(), job);
assert.strictEqual(job.stop(), job);
assert.strictEqual(job[Symbol.dispose](), undefined);
assert.throws(() => prototype[Symbol.dispose].call({}), isTypeError);

const OriginalDate = Date;
globalThis.Date = function FakeDate() { return { fake: true }; };
const intrinsicDate = Ant.cron.parse('@yearly', 0);
globalThis.Date = OriginalDate;
assert.strictEqual(intrinsicDate instanceof OriginalDate, true);

console.log('Ant.cron parse and CronJob ok');
