async function test() {
  console.log("1: Before await");
  const x = await new Promise(resolve => setTimeout(() => {
    console.log("2: Timer callback");
    resolve("VALUE");
  }, 10));
  console.log("3: x assigned, x =", x);
  console.log("4: About to return");
  return x;
}

console.log("A: Calling test");
test().then(r => console.log("Final:", r));
console.log("B: After test call");
