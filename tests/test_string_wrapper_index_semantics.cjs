const assert = require('node:assert');

// A String wrapper's characters and length are own read-only properties held
// outside its shape: writes to them are ignored (or throw in strict code),
// defines may only restate them, and they cannot be deleted. Indexes past the
// end are ordinary own or inherited properties. Expected values come from Node.

{
  const r = [];
  const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name + ' ' + e.message); } };
  const hot = (f) => { let v; for (let i = 0; i < 300; i++) v = f(); return v; };
  t('read chars', () => { const s = new String('ab'); return [s[0], s[1], s[2]]; });
  t('write past end', () => { const s = new String('ab'); s[5] = 'y'; return [s[5], s.length, Object.keys(s), 5 in s]; });
  t('write past end hot', () => hot(() => { const s = new String('ab'); s[3] = 'z'; return [s[3], s[2]]; }));
  t('write char sloppy', () => { const s = new String('ab'); s[0] = 'x'; return [s[0], Object.keys(s), Object.getOwnPropertyDescriptor(s, '0')]; });
  t('write char strict', () => { 'use strict'; const s = new String('ab'); s[0] = 'x'; });
  t('write char strict hot', () => hot(() => { 'use strict'; const s = new String('ab'); try { s[1] = 'x'; return 'no'; } catch (e) { return e.message; } }));
  t('write length sloppy', () => { const s = new String('ab'); s.length = 9; return s.length; });
  t('write length strict', () => { 'use strict'; const s = new String('ab'); s.length = 9; });
  t('string key write', () => { const s = new String('ab'); s['1'] = 'q'; s['7'] = 'w'; return [s[1], s[7]]; });
  t('string key strict', () => { 'use strict'; const s = new String('ab'); s['1'] = 'q'; });
  t('Object() wrapper', () => { const s = Object('ab'); s[0] = 'z'; s.length = 7; s[4] = 'f'; return [s[0], s.length, s[4], Object.keys(s)]; });
  t('Object() wrapper strict', () => { 'use strict'; const s = Object('ab'); s[1] = 7; });
  t('sloppy this', () => { String.prototype.__w = function () { this[0] = 'q'; this[9] = 'n'; return [this[0], this[9], Object.keys(this)]; }; const v = 'ab'.__w(); delete String.prototype.__w; return v; });
  t('subclass', () => { class S extends String { constructor() { super('hey'); this[1] = 'k'; } } const s = new S(); s[5] = 1; return [s[0], s[1], s[5], s.length]; });
  t('surrogates', () => { const s = new String('a\u{1F600}'); s[2] = 'x'; s[3] = 'y'; return [s.length, s[1].charCodeAt(0), s[2].charCodeAt(0), s[3]]; });
  t('in / hasOwn', () => { const s = new String('ab'); return [0 in s, 2 in s, Object.hasOwn(s, 1), Object.hasOwn(s, 2)]; });
  t('keys with index props', () => { const s = new String('ab'); s.x = 1; s[4] = 2; return Object.keys(s); });
  t('names with index props', () => { const s = new String('ab'); s.x = 1; s[5] = 2; s[3] = 0; return Object.getOwnPropertyNames(s); });
  t('for-in', () => { const s = new String('ab'); s[4] = 1; s.y = 2; const k = []; for (const x in s) k.push(x); return k; });
  t('String.prototype index', () => { String.prototype[6] = 'P'; const s = new String('ab'); const v = [s[6], 'ab'[6], s[1]]; delete String.prototype[6]; return v; });
  t('String.prototype index hot', () => { String.prototype[6] = 'P'; const v = hot(() => ['ab'[6], new String('ab')[6], 'abcdefg'[6]]); delete String.prototype[6]; return v; });
  t('Object.prototype index', () => { Object.prototype[3] = 'O'; const v = ['ab'[3], new String('ab')[3], 'abcd'[3]]; delete Object.prototype[3]; return v; });
  t('String.prototype setter past end', () => { let got; Object.defineProperty(String.prototype, '7', { set(v) { got = v; }, configurable: true }); const s = new String('ab'); s[7] = 'v'; const own = Object.hasOwn(s, 7); delete String.prototype[7]; return [got, own]; });
  t('primitive out of range', () => ['abc'[5], 'abc'[2], hot(() => 'abc'[9])]);
  t('numeric keys', () => { const s = new String('abc'); return [s[1.5], s[-1], s['01'], s[' 1']]; });
  t('Reflect.set', () => [Reflect.set(new String('ab'), 0, 'x'), Reflect.set(new String('ab'), 4, 'x'), Reflect.set(new String('ab'), 'length', 4)]);
  t('Reflect.set receiver', () => { const o = {}; return [Reflect.set(new String('ab'), 0, 'x', o), o[0]]; });
  t('defineProperty char', () => [Reflect.defineProperty(new String('ab'), '0', { value: 'x' }), Reflect.defineProperty(new String('ab'), '1', { enumerable: false }), Reflect.defineProperty(new String('ab'), '0', { get() {} }), Reflect.defineProperty(new String('ab'), '0', { configurable: true })]);
  t('defineProperty same', () => { const s = new String('ab'); Object.defineProperty(s, '1', { value: 'b', enumerable: true, writable: false }); Object.defineProperty(s, 'length', { value: 2 }); return [Object.getOwnPropertyDescriptor(s, 1), s.length]; });
  t('defineProperty length', () => { const s = new String('ab'); Object.defineProperty(s, 'length', { value: 3 }); });
  t('defineProperty past end', () => { const s = new String('ab'); Object.defineProperty(s, '4', { value: 1, enumerable: true }); return [s[4], Object.keys(s)]; });
  t('delete', () => { const s = new String('ab'); return [delete s[0], delete s.length, delete s[5], s[0], s.length]; });
  t('delete strict', () => { 'use strict'; const s = new String('ab'); try { delete s[0]; return 'no'; } catch (e) { return e.constructor.name; } });
  t('freeze', () => { const s = Object.freeze(new String('ab')); return [Object.isFrozen(s), s[0]]; });
  t('push on wrapper', () => { const s = new String('ab'); try { Array.prototype.push.call(s, 'c'); } catch (e) { return e.constructor.name; } });
  assert.deepStrictEqual(r, [
    "read chars: [\"a\",\"b\",null]",
    "write past end: [\"y\",2,[\"0\",\"1\",\"5\"],true]",
    "write past end hot: [\"z\",null]",
    "write char sloppy: [\"a\",[\"0\",\"1\"],{\"value\":\"a\",\"writable\":false,\"enumerable\":true,\"configurable\":false}]",
    "write char strict: TypeError Cannot assign to read only property '0' of object '[object String]'",
    "write char strict hot: \"Cannot assign to read only property '1' of object '[object String]'\"",
    "write length sloppy: 2",
    "write length strict: TypeError Cannot assign to read only property 'length' of object '[object String]'",
    "string key write: [\"b\",\"w\"]",
    "string key strict: TypeError Cannot assign to read only property '1' of object '[object String]'",
    "Object() wrapper: [\"a\",2,\"f\",[\"0\",\"1\",\"4\"]]",
    "Object() wrapper strict: TypeError Cannot assign to read only property '1' of object '[object String]'",
    "sloppy this: [\"a\",\"n\",[\"0\",\"1\",\"9\"]]",
    "subclass: TypeError Cannot assign to read only property '1' of object '[object String]'",
    "surrogates: [3,55357,56832,\"y\"]",
    "in / hasOwn: [true,false,true,false]",
    "keys with index props: [\"0\",\"1\",\"4\",\"x\"]",
    "names with index props: [\"0\",\"1\",\"3\",\"5\",\"length\",\"x\"]",
    "for-in: [\"0\",\"1\",\"4\",\"y\"]",
    "String.prototype index: [\"P\",\"P\",\"b\"]",
    "String.prototype index hot: [\"P\",\"P\",\"g\"]",
    "Object.prototype index: [\"O\",\"O\",\"d\"]",
    "String.prototype setter past end: [\"v\",false]",
    "primitive out of range: [null,\"c\",null]",
    "numeric keys: [null,null,null,null]",
    "Reflect.set: [false,true,false]",
    "Reflect.set receiver: [false,null]",
    "defineProperty char: [false,false,false,false]",
    "defineProperty same: [{\"value\":\"b\",\"writable\":false,\"enumerable\":true,\"configurable\":false},2]",
    "defineProperty length: TypeError Cannot redefine property: length",
    "defineProperty past end: [1,[\"0\",\"1\",\"4\"]]",
    "delete: [false,false,true,\"a\",2]",
    "delete strict: \"TypeError\"",
    "freeze: [true,\"a\"]",
    "push on wrapper: \"TypeError\""
  ]);
}
