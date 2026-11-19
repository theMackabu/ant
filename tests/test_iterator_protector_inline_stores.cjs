const assert = require('node:assert');

const arrayIteratorProto = Object.getPrototypeOf([][Symbol.iterator]());
const stringIteratorProto = Object.getPrototypeOf(''[Symbol.iterator]());
const mapIteratorProto = Object.getPrototypeOf(new Map()[Symbol.iterator]());
const iteratorProto = Object.getPrototypeOf(mapIteratorProto);

function storeNext(target, value, ...rest) { target.next = value; return rest.length; }
function storeReturn(target, value, ...rest) { target.return = value; return rest.length; }

const stringNext = stringIteratorProto.next;
for (let i = 0; i < 200000; i++) storeNext(stringIteratorProto, stringNext);

const list = [1, 2, 3];
const count = () => { let n = 0; for (const item of list) n++; return n; };
const firstTwo = () => { const [a, b] = list; return [a, b]; };
const spread = () => [...list].length;
for (let i = 0; i < 20000; i++) { count(); firstTwo(); spread(); }
assert.deepStrictEqual([count(), firstTwo(), spread()], [3, [1, 2], 3]);

const arrayNext = arrayIteratorProto.next;
storeNext(arrayIteratorProto, function () { return { done: true, value: undefined }; });
assert.deepStrictEqual([count(), firstTwo(), spread(), Array.from(list).length, new Set(list).size], [0, [undefined, undefined], 0, 0, 0]);
storeNext(arrayIteratorProto, arrayNext);
assert.deepStrictEqual([count(), firstTwo(), spread()], [3, [1, 2], 3]);

const mapNext = mapIteratorProto.next;
const twin = () => {
  const object = Object.create(iteratorProto);
  Object.defineProperty(object, 'next', { value: mapNext, writable: true, enumerable: false, configurable: true });
  Object.defineProperty(object, Symbol.toStringTag, { value: 'Map Iterator', writable: false, enumerable: false, configurable: true });
  return object;
};
let returnCalls = 0;
const countingReturn = function () { returnCalls++; return {}; };
for (let i = 0; i < 200000; i++) storeReturn(twin(), countingReturn);

const map = new Map([[1, 2], [3, 4]]);
const breakEarly = () => { for (const entry of map) break; };
const takeFirst = () => { const [first] = map; return first; };
for (let i = 0; i < 20000; i++) { breakEarly(); takeFirst(); }
assert.strictEqual(returnCalls, 0);

storeReturn(mapIteratorProto, countingReturn);
breakEarly();
takeFirst();
assert.strictEqual(returnCalls, 2);
delete mapIteratorProto.return;

for (let i = 0; i < 200000; i++) storeNext(twin(), mapNext);
const mapCount = () => { let n = 0; for (const entry of map) n++; return n; };
for (let i = 0; i < 20000; i++) mapCount();
storeNext(mapIteratorProto, function () { return { done: true, value: undefined }; });
assert.deepStrictEqual([mapCount(), [...map].length], [0, 0]);
storeNext(mapIteratorProto, mapNext);
assert.deepStrictEqual([mapCount(), [...map].length], [2, 2]);

console.log('ok');
