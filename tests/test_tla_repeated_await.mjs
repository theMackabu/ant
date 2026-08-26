const fulfilled = Promise.resolve(1);

let directSum = 0;
for (let i = 0; i < 32; i++) directSum += await fulfilled;
if (directSum !== 32) throw new Error("repeated top-level await produced the wrong value");

async function sumRepeatedAwaits(iterations) {
  let sum = 0;
  for (let i = 0; i < iterations; i++) sum += await fulfilled;
  return sum;
}

const nestedSum = await sumRepeatedAwaits(32);
if (nestedSum !== 32) throw new Error("nested repeated await produced the wrong value");

const order = [];
await new Promise(resolve => {
  fulfilled.then(() => {
    order.push("first");
    fulfilled.then(() => {
      order.push("second");
      resolve();
    });
  });
});

if (order.join(",") !== "first,second") {
  throw new Error("handler appended during Promise processing was lost");
}

console.log("repeated top-level await: ok");
