function add(a, b) {
  return a + b;
}

const iterations = 200000;
console.log(`Running ${iterations} iterations...`);

let start = Date.now();
let result = 0;

for (let i = 0; i < iterations; i++) {
  result += add(i, i + 1);
}

console.log(`simple add: ${Date.now() - start}ms (result: ${result})`);
console.log('Done!');
