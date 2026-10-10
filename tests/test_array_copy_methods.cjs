const assert = require('node:assert');

// with, toSorted, toReversed and toSpliced build an exactly sized copy in one
// pass: arr[0, start), the inserted items, then arr[start + skip, len).
// toSpliced converts its arguments before reading any element and never reads
// skipped ones. Expected values come from Node.

{
  const r = [];
  const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name); } };
  const a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
  t('basic', () => a.toSpliced(2, 3, 'x', 'y'));
  t('no args', () => a.toSpliced());
  t('start only', () => a.toSpliced(4));
  t('negative start', () => a.toSpliced(-3, 1));
  t('huge start', () => a.toSpliced(99, 1, 'z'));
  t('-Infinity start', () => a.toSpliced(-Infinity, 2));
  t('Infinity skip', () => a.toSpliced(1, Infinity));
  t('undefined skip', () => a.toSpliced(0, undefined));
  t('string args', () => a.toSpliced('2', '2'));
  t('NaN args', () => a.toSpliced(NaN, NaN, 'n'));
  t('fraction', () => a.toSpliced(1.9, 1.9));
  t('insert only', () => a.toSpliced(3, 0, 'a', 'b', 'c'));
  t('empty source', () => [].toSpliced(0, 0, 1, 2));
  t('to empty', () => [1, 2].toSpliced(0));
  t('holes read as undefined', () => [1, , 3].toSpliced(0, 0));
  t('skipped not read', () => { const log = []; const o = { length: 4, get 0() { log.push(0); return 'a'; }, get 1() { log.push(1); return 'b'; }, get 2() { log.push(2); return 'c'; }, get 3() { log.push(3); return 'd'; } }; return [Array.prototype.toSpliced.call(o, 1, 2), log]; });
  t('arg order', () => { const log = []; const s = { valueOf() { log.push('start'); return 1; } }; const k = { valueOf() { log.push('skip'); return 1; } }; const p = new Proxy([1, 2, 3], { get(t, key) { if (key !== 'length' && typeof key === 'string') log.push('get ' + key); return t[key]; } }); return [Array.prototype.toSpliced.call(p, s, k), log]; });
  t('symbol start', () => a.toSpliced(Symbol()));
  t('then push', () => { const b = a.slice(0, 5).toSpliced(1, 1, 'q', 'r'); b.push('p'); b.length = 9; return [b, 7 in b, 8 in b]; });
  t('with then push', () => { const b = [1, 2, 3, 4, 5].with(0, 9); b.push(6); b.length = 8; return [b, 6 in b]; });
  t('toSorted spare', () => { const b = [5, 3, 1, 4, 2].toSorted(); b.length = 6; return [b, 5 in b]; });
  t('toReversed', () => [1, , 3].toReversed());
  t('array-like', () => Array.prototype.toSpliced.call({ length: 3, 0: 'a', 2: 'c' }, 1, 1));
  assert.deepStrictEqual(r, [
    "basic: [0,1,\"x\",\"y\",5,6,7,8,9]",
    "no args: [0,1,2,3,4,5,6,7,8,9]",
    "start only: [0,1,2,3]",
    "negative start: [0,1,2,3,4,5,6,8,9]",
    "huge start: [0,1,2,3,4,5,6,7,8,9,\"z\"]",
    "-Infinity start: [2,3,4,5,6,7,8,9]",
    "Infinity skip: [0]",
    "undefined skip: [0,1,2,3,4,5,6,7,8,9]",
    "string args: [0,1,4,5,6,7,8,9]",
    "NaN args: [\"n\",0,1,2,3,4,5,6,7,8,9]",
    "fraction: [0,2,3,4,5,6,7,8,9]",
    "insert only: [0,1,2,\"a\",\"b\",\"c\",3,4,5,6,7,8,9]",
    "empty source: [1,2]",
    "to empty: []",
    "holes read as undefined: [1,null,3]",
    "skipped not read: [[\"a\",\"d\"],[0,3]]",
    "arg order: [[1,3],[\"start\",\"skip\",\"get 0\",\"get 2\"]]",
    "symbol start: TypeError",
    "then push: [[0,\"q\",\"r\",2,3,4,\"p\",null,null],false,false]",
    "with then push: [[9,2,3,4,5,6,null,null],false]",
    "toSorted spare: [[1,2,3,4,5,null],false]",
    "toReversed: [3,null,1]",
    "array-like: [\"a\",\"c\"]"
  ]);
}

console.log('array copy methods: ok');
