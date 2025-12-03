async function test() {
  console.log("1: Before await");
  const x = await new Promise(resolve => setTimeout(() => {
    console.log("2: Timer fired");
    resolve("VALUE");
  }, 50));
  console.log("3: After await, x =", x);
  console.log("4: About to return");
  return "FINAL_" + x;
}

console.log("A: Calling test()");
const p = test();
console.log("B: test() returned");
p.then(result => console.log("C: Promise resolved:", result));
console.log("D: Script end");
