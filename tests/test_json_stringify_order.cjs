function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

same(JSON.stringify(), undefined, 'no arguments');

same(
  JSON.stringify({ b: 1, 10: 'ten', a: 2, 2: 'two', 4294967295: 'big', 4294967294: 'max', '01': 'lead', 0: 'zero' }),
  '{"0":"zero","2":"two","10":"ten","4294967294":"max","b":1,"a":2,"4294967295":"big","01":"lead"}',
  'index keys first, then insertion order'
);

same(JSON.stringify({ a: 1, get b() { delete this.c; return 2; }, c: 3, d: 4 }), '{"a":1,"b":2,"d":4}', 'deleted during serialization');
same(JSON.stringify({ a: 1, get b() { this.z = 9; delete this.a; return 2; }, c: 3 }), '{"a":1,"b":2,"c":3}', 'keys are snapshotted');

const P = {};
const a = Object.create(P);
Object.defineProperty(a, 'x', { get() { P.toJSON = () => 'P'; return 1; }, enumerable: true });
same(JSON.stringify([a, Object.create(P)]), '[{"x":1},"P"]', 'toJSON added to a prototype mid-serialization');

same(
  JSON.stringify([[], { get a() { Array.prototype.toJSON = () => 'A'; return 1; } }, [1], {}]),
  '[[],{"a":1},"A",{}]',
  'toJSON added to Array.prototype mid-serialization'
);
delete Array.prototype.toJSON;

const d = new Date(0);
const saved = Date.prototype.toJSON;
same(JSON.stringify([d, { get a() { Date.prototype.toJSON = () => 'D'; return 1; } }, d]), '["1970-01-01T00:00:00.000Z",{"a":1},"D"]', 'toJSON replaced mid-serialization');
Date.prototype.toJSON = saved;
same(JSON.stringify([d, { get a() { delete Date.prototype.toJSON; return 1; } }, d]), '["1970-01-01T00:00:00.000Z",{"a":1},{}]', 'toJSON deleted mid-serialization');
Date.prototype.toJSON = saved;

const escaped = ['\u0000\u0001\b\t\n\v\f\r\u001f"\\', 'é日本\ud83d\ude00', '\ud800', 'x\udc00', '\ud800\ud800\udc00', '\ud7ff\ue000'];
same(
  JSON.stringify(escaped),
  '["\\u0000\\u0001\\b\\t\\n\\u000b\\f\\r\\u001f\\"\\\\","é日本😀","\\ud800","x\\udc00","\\ud800\ud800\udc00","\ud7ff\ue000"]',
  'escapes and surrogates'
);

console.log('json stringify order ok');
