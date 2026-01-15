const obj = { a: 42 };
const iterations = 200000;

let start = Date.now();
let result = 0;
for (let i = 0; i < iterations; i++) {
  result += obj.a;
}
console.log(`loop string access: ${Date.now() - start}ms (result: ${result})`);
