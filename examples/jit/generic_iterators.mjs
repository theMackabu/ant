const ROUNDS = 200;
const DATA = Array.from({ length: 64 }, (_, i) => i);

function arrayIter(values) {
  let total = 0;
  for (const v of values) total += v;
  return total;
}

function* range(n) {
  for (let i = 0; i < n; i++) yield i;
}

function generatorIter(n) {
  let total = 0;
  for (const v of range(n)) total += v;
  return total;
}

const iterable = {
  [Symbol.iterator]() {
    let i = 0;
    return {
      next: () => (i < 64 ? { value: i++, done: false } : { value: undefined, done: true })
    };
  }
};

function userIter(source) {
  let total = 0;
  for (const v of source) total += v;
  return total;
}

let sink = 0;
for (let round = 0; round < ROUNDS; round++) {
  sink += arrayIter(DATA);
  sink += generatorIter(64);
  sink += userIter(iterable);
}

if (sink !== ROUNDS * 3 * 2016) throw new Error(`unexpected total ${sink}`);
console.log(`ok ${sink}`);
