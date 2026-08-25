function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function addOne(value) {
  return value + 1;
}

let total = 0;
for (let i = 0; i < 1000; i++) total = addOne(total);
assert(total === 1000, 'closure reference');

function negativeNaN(value) {
  return -Math.sqrt(value);
}

let nan;
for (let i = 0; i < 1000; i++) nan = negativeNaN(-1);
assert(typeof nan === 'number' && Number.isNaN(nan), 'JIT NaN boxing');

let rope = '';
for (let i = 0; i < 5000; i++) rope += String(i % 10);
assert(rope.length === 5000, 'rope reference');
assert(rope.slice(0, 10) === '0123456789', 'rope contents');

const symbol = Symbol('cage');
const bigint = (1n << 200n) + 17n;
const object = {[symbol]: bigint};
for (let i = 0; i < 100000; i++) ({index: i, text: `${i}`});
assert(object[symbol] === bigint, 'pooled reference after GC pressure');

class Box {}
const box = new Box();
function isBox(value) {
  return value instanceof Box;
}
for (let i = 0; i < 1000; i++) assert(isBox(box), 'instanceof cache');

const native = Math.min;
for (let i = 0; i < 1000; i++) assert(native(i, 500) === Math.min(i, 500), 'native handle');
assert(native === Math.min, 'native metadata handle');

console.log('pointer compression cage: ok');
