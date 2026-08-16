function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function write(array, index, value) {
  array[index] = value;
}

function read(array, index) {
  return array[index];
}

const hot = [0, 1, 2];
for (let i = 0; i < 500; i++) write(hot, 1, i);
assertEq(hot[1], 499, "hot dense write");

const growing = [];
for (let i = 0; i < 500; i++) write(growing, i, i + 1);
assertEq(growing.length, 500, "dense growth length");
assertEq(growing[499], 500, "dense growth value");

const hole = [0, , 2];
write(hole, 1, 3);
assertEq(hole[1], 3, "hole write");

const sparse = [];
write(sparse, 1000, 4);
assertEq(sparse.length, 1001, "sparse growth length");
assertEq(sparse[1000], 4, "sparse growth value");

const keyed = [];
write(keyed, -1, 5);
write(keyed, 1.5, 6);
assertEq(keyed[-1], 5, "negative numeric property");
assertEq(keyed[1.5], 6, "fractional numeric property");
assertEq(keyed.length, 0, "non-index writes preserve length");

const stringIndexed = [];
write(stringIndexed, "0", 12);
write(stringIndexed, "01", 13);
write(stringIndexed, "-0", 14);
assertEq(read(stringIndexed, "0"), 12, "canonical string index");
assertEq(read(stringIndexed, "01"), 13, "leading-zero property");
assertEq(read(stringIndexed, "-0"), 14, "negative-zero string property");
assertEq(stringIndexed.length, 1, "noncanonical string keys preserve length");

const sealed = Object.seal([1]);
write(sealed, 0, 8);
write(sealed, 1, 9);
assertEq(sealed[0], 8, "sealed existing index remains writable");
assertEq(sealed.length, 1, "sealed array does not grow");

let trapCount = 0;
const target = [1];
const proxy = new Proxy(target, {
  set(object, key, value, receiver) {
    trapCount++;
    return Reflect.set(object, key, value, receiver);
  },
});
write(proxy, 0, 10);
assertEq(trapCount, 1, "proxy trap count");

function mappedArgument(value) {
  for (let i = 0; i < 500; i++) write(arguments, 0, i);
  return value;
}
assertEq(mappedArgument(1), 499, "mapped arguments write");

const retained = { marker: 11 };
for (let i = 0; i < 500; i++) write(hot, 2, retained);
assertEq(hot[2], retained, "object write retained");
assertEq(hot[2].marker, 11, "object payload retained");

console.log("OK: test_jit_put_elem_fast");
