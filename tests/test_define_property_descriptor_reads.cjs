const assert = require('node:assert');

// Ordinary descriptor objects are read in one pass over their shape; fields
// that are accessors, or that an object on the prototype chain also holds,
// take the per-field reads so getter order stays observable. A proxy reads
// only the fields its has trap reports, and arrays and functions are valid
// descriptors. Expected values come from Node.

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

// proxy, array, function and builtin descriptors
{
  const r = [];
  const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name); } };
  const d = (o, k) => Object.getOwnPropertyDescriptor(o, k);
  t('proxy has/get order', () => { const log = []; const p = new Proxy({ value: 2, enumerable: true }, { get(t, k) { log.push('get ' + k); return t[k]; }, has(t, k) { log.push('has ' + k); return k in t; } }); const o = {}; Object.defineProperty(o, 'x', p); return [d(o, 'x'), log]; });
  t('proxy without has trap', () => { const log = []; const p = new Proxy({ value: 3 }, { get(t, k) { log.push(k); return t[k]; } }); const o = {}; Object.defineProperty(o, 'x', p); return [o.x, log]; });
  t('proxy has throws', () => Object.defineProperty({}, 'x', new Proxy({}, { has() { throw new RangeError('h'); } })));
  t('proxy bad getter', () => Object.defineProperty({}, 'x', new Proxy({ get: 1 }, {})));
  t('proxy getter undefined', () => { const o = {}; Object.defineProperty(o, 'x', new Proxy({ get: undefined }, {})); return d(o, 'x'); });
  t('array desc', () => { const a = []; a.value = 3; a.enumerable = true; const o = {}; Object.defineProperty(o, 'x', a); return d(o, 'x'); });
  t('function desc', () => { const f = function () {}; f.value = 8; const o = {}; Object.defineProperty(o, 'x', f); return d(o, 'x'); });
  t('builtin desc', () => { const o = {}; Object.defineProperty(o, 'x', Math.max); return d(o, 'x'); });
  t('defineProperties array desc', () => { const a = []; a.value = 5; const o = Object.defineProperties({}, { x: a }); return o.x; });
  t('Object.create function desc', () => { const f = function () {}; f.value = 6; return Object.create(null, { x: f }).x; });
  t('Reflect array desc', () => { const a = []; a.value = 1; const o = {}; return [Reflect.defineProperty(o, 'x', a), o.x]; });
  t('primitive desc', () => Object.defineProperty({}, 'x', 1));
  t('null desc', () => Object.defineProperty({}, 'x', null));
  assert.deepStrictEqual(r, [
    "proxy has/get order: [{\"value\":2,\"writable\":false,\"enumerable\":true,\"configurable\":false},[\"has enumerable\",\"get enumerable\",\"has configurable\",\"has value\",\"get value\",\"has writable\",\"has get\",\"has set\"]]",
    "proxy without has trap: [3,[\"value\"]]",
    "proxy has throws: RangeError",
    "proxy bad getter: TypeError",
    "proxy getter undefined: {\"enumerable\":false,\"configurable\":false}",
    "array desc: {\"value\":3,\"writable\":false,\"enumerable\":true,\"configurable\":false}",
    "function desc: {\"value\":8,\"writable\":false,\"enumerable\":false,\"configurable\":false}",
    "builtin desc: {\"writable\":false,\"enumerable\":false,\"configurable\":false}",
    "defineProperties array desc: 5",
    "Object.create function desc: 6",
    "Reflect array desc: [true,1]",
    "primitive desc: TypeError",
    "null desc: TypeError"
  ]);
}
