// MVP: "continue" inside switch inside for...in loop

const obj = { a: 1, b: 2, c: 3 };
const results = [];

for (let key in obj) {
  switch (key) {
    case "b":
      continue; // should skip to next for...in iteration
    default:
      results.push(key);
  }
}

console.log("expected: ['a','c']");
console.log("got:", JSON.stringify(results));
