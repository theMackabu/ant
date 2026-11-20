const assert = require('node:assert');

const out = [];
function mk() { return [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40]; }
function mkObjs() { return [[1, 2, 3], [4, 5, 6]][0]; }
function t(name, f) { let r; for (let i = 0; i < 300; i++) { try { r = JSON.stringify(f()); } catch (e) { r = '!' + e.constructor.name; } } out.push(name + ': ' + r); }
t('len0 then index', () => { const a = mk(); a.length = 0; a[0] = 9; return [a, a.length, Object.keys(a)]; });
t('len0 then push', () => { const a = mk(); a.length = 0; a.push(7, 8); return [a, a.length]; });
t('len0 then pop', () => { const a = mk(); a.length = 0; return [a.pop(), a.length]; });
t('len0 then unshift', () => { const a = mk(); a.length = 0; a.unshift(1); return [a, a.length]; });
t('len0 then grow', () => { const a = mk(); a.length = 0; a.length = 3; return [a, 0 in a, a.length]; });
t('len0 then idx5', () => { const a = mk(); a.length = 0; a[5] = 1; return [a, a.length]; });
t('len0 then fill', () => { const a = mk(); a.length = 0; a.length = 2; a.fill(7); return a; });
t('len0 then splice', () => { const a = mk(); a.length = 0; a.splice(0, 0, 1, 2); return a; });
t('len0 then concat', () => { const a = mk(); a.length = 0; return a.concat([1], 2); });
t('len0 freeze', () => { const a = mk(); a.length = 0; Object.freeze(a); return [Object.isFrozen(a), a.length]; });
t('len0 isFrozen', () => { const a = mk(); a.length = 0; return [Object.isFrozen(a), Object.isSealed(a), Object.isExtensible(a)]; });
t('len0 defineProperty', () => { const a = mk(); a.length = 0; Object.defineProperty(a, 0, { value: 4, writable: true, enumerable: true, configurable: true }); return [a, a.length]; });
t('pop to zero', () => { const a = mkObjs(); a.pop(); a.pop(); a.pop(); a.pop(); a.push('z'); a[1] = 'y'; return [a, a.length]; });
t('shift to zero', () => { const a = mkObjs(); a.shift(); a.shift(); a.shift(); a.shift(); a.push('z'); return [a, a.length]; });
t('len1 then index', () => { const a = mk(); a.length = 1; a[1] = 9; a[0] = 8; return [a, a.length]; });
t('defineProperty length 0', () => { const a = mk(); Object.defineProperty(a, 'length', { value: 0 }); a[0] = 1; a.push(2); return [a, a.length]; });
t('nested len0', () => { const a = mkObjs(); a.length = 0; a[0] = 'q'; a.push('r'); return [a, a.length, mkObjs()]; });
t('len0 then sort reverse', () => { const a = mk(); a.length = 0; a.push(3, 1, 2); a.sort(); a.reverse(); return a; });
t('len0 then copyWithin', () => { const a = mk(); a.length = 0; a.length = 4; a[3] = 1; a.copyWithin(0, 3); return [a, 1 in a]; });
t('len0 spread', () => { const a = mk(); a.length = 0; return [...a, 1].concat(a); });
t('sibling intact', () => { const a = mk(); a.length = 0; a[0] = 9; return mk(); });

assert.deepStrictEqual(out, [
  "len0 then index: [[9],1,[\"0\"]]",
  "len0 then push: [[7,8],2]",
  "len0 then pop: [null,0]",
  "len0 then unshift: [[1],1]",
  "len0 then grow: [[null,null,null],false,3]",
  "len0 then idx5: [[null,null,null,null,null,1],6]",
  "len0 then fill: [7,7]",
  "len0 then splice: [1,2]",
  "len0 then concat: [1,2]",
  "len0 freeze: [true,0]",
  "len0 isFrozen: [false,false,true]",
  "len0 defineProperty: [[4],1]",
  "pop to zero: [[\"z\",\"y\"],2]",
  "shift to zero: [[\"z\"],1]",
  "len1 then index: [[8,9],2]",
  "defineProperty length 0: [[1,2],2]",
  "nested len0: [[\"q\",\"r\"],2,[1,2,3]]",
  "len0 then sort reverse: [3,2,1]",
  "len0 then copyWithin: [[1,null,null,1],false]",
  "len0 spread: [1]",
  "sibling intact: [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40]"
]);

console.log('ok');
