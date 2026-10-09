const assert = require('node:assert');

// Ordinary descriptor objects are read in one pass over their shape; fields
// that are accessors, or that an object on the prototype chain also holds,
// take the per-field reads so getter order stays observable. Expected values
// come from Node.

{
  const r = [];
  const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name); } };
  const d = (o, k) => Object.getOwnPropertyDescriptor(o, k);
  t('basic', () => { const o = {}; Object.defineProperty(o, 'x', { value: 1 }); return d(o, 'x'); });
  t('all', () => { const o = {}; Object.defineProperty(o, 'x', { value: 1, writable: 1, enumerable: '', configurable: 0 }); return d(o, 'x'); });
  t('inherited field', () => { const o = {}; Object.defineProperty(o, 'x', Object.create({ value: 7, enumerable: true })); return d(o, 'x'); });
  t('getter field order', () => { const log = []; const desc = {}; for (const k of ['set', 'get', 'writable', 'value', 'configurable', 'enumerable']) Object.defineProperty(desc, k, { get() { log.push(k); return undefined; }, enumerable: true }); Object.defineProperty({}, 'x', desc); return log; });
  t('Object.prototype field', () => { Object.prototype.enumerable = true; const o = {}; Object.defineProperty(o, 'x', { value: 1 }); delete Object.prototype.enumerable; return d(o, 'x'); });
  t('Object.prototype get', () => { Object.prototype.get = function () { return 5; }; const o = {}; try { Object.defineProperty(o, 'x', {}); return d(o, 'x').get !== undefined; } finally { delete Object.prototype.get; } });
  t('bad getter', () => Object.defineProperty({}, 'x', { get: 1 }));
  t('bad setter', () => Object.defineProperty({}, 'x', { set: 1 }));
  t('bad getter before set read', () => { const log = []; const desc = { get: 1 }; Object.defineProperty(desc, 'set', { get() { log.push('set'); } }); try { Object.defineProperty({}, 'x', desc); } catch (e) { return [e.constructor.name, log]; } });
  t('mixed', () => Object.defineProperty({}, 'x', { get() {}, value: 1 }));
  t('accessor', () => { const o = {}; Object.defineProperty(o, 'x', { get() { return 3; }, enumerable: true }); return [o.x, Object.keys(o)]; });
  t('null proto desc', () => { const desc = Object.create(null); desc.value = 4; const o = {}; Object.defineProperty(o, 'y', desc); return o.y; });
  t('defineProperties', () => { const o = Object.defineProperties({}, { a: { value: 1, enumerable: true }, b: { get() { return 2; }, enumerable: true } }); return [o.a, o.b, Object.keys(o)]; });
  t('Object.create props', () => { const o = Object.create({}, { a: { value: 1, writable: true } }); return d(o, 'a'); });
  t('Reflect', () => [Reflect.defineProperty({}, 'x', { value: 1 }), Reflect.defineProperty(Object.freeze({}), 'x', { value: 1 })]);
  t('class instance desc', () => { class D { constructor() { this.value = 9; this.enumerable = true; } } const o = {}; Object.defineProperty(o, 'x', new D()); return d(o, 'x'); });
  assert.deepStrictEqual(r, [
    "basic: {\"value\":1,\"writable\":false,\"enumerable\":false,\"configurable\":false}",
    "all: {\"value\":1,\"writable\":true,\"enumerable\":false,\"configurable\":false}",
    "inherited field: {\"value\":7,\"writable\":false,\"enumerable\":true,\"configurable\":false}",
    "getter field order: TypeError",
    "Object.prototype field: {\"value\":1,\"writable\":false,\"enumerable\":true,\"configurable\":false}",
    "Object.prototype get: true",
    "bad getter: TypeError",
    "bad setter: TypeError",
    "bad getter before set read: [\"TypeError\",[]]",
    "mixed: TypeError",
    "accessor: [3,[\"x\"]]",
    "null proto desc: 4",
    "defineProperties: [1,2,[\"a\",\"b\"]]",
    "Object.create props: {\"value\":1,\"writable\":true,\"enumerable\":false,\"configurable\":false}",
    "Reflect: [true,false]",
    "class instance desc: {\"value\":9,\"writable\":false,\"enumerable\":true,\"configurable\":false}"
  ]);
}
