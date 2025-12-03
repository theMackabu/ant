async function test() {
  await new Promise(resolve => setTimeout(resolve, 50));
  return "result";
}

console.log("Calling async function");
const p = test();
console.log("Promise returned:", typeof p);

p.then(r => console.log("Then callback:", r));

setTimeout(() => console.log("Done"), 100);
