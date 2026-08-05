const iterations = Number(process.argv[2] || 50_000);
const rounds = Number(process.argv[3] || 7);
const filter = process.argv[4];

function median(values) {
  const sorted = [];
  for (const value of values) {
    let index = sorted.length;
    while (index > 0 && sorted[index - 1] > value) index--;
    sorted.splice(index, 0, value);
  }
  return sorted[Math.floor(sorted.length / 2)];
}

let sink;

function bench(name, fn, check) {
  if (filter && name !== filter) return;
  for (let i = 0; i < 2_000; i++) sink = fn();

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = performance.now();
    for (let i = 0; i < iterations; i++) sink = fn();
    samples.push(performance.now() - start);
  }

  if (!check(sink)) throw new Error(name + " result mismatch");
  console.log(name + ": median=" + median(samples).toFixed(2) + "ms samples=" + samples.map(x => x.toFixed(2)).join(","));
}

const textHeaders = new Headers({ "content-type": "text/plain" });

console.log("Response construction benchmark: " + iterations + " iterations x " + rounds + " rounds");
bench("plain object", () => ({}), value => value !== null);
bench("empty Headers", () => new Headers(), value => value instanceof Headers);
bench(
  "object-initialized Headers",
  () => new Headers({ "content-type": "text/plain" }),
  value => value.get("content-type") === "text/plain",
);
bench("empty Response", () => new Response(), value => value instanceof Response && value.body === null);
bench("text Response", () => new Response("hello"), value => value instanceof Response);
bench("text Response with empty init", () => new Response("hello", {}), value => value.status === 200);
bench(
  "text Response with header object",
  () => new Response("hello", { headers: { "content-type": "text/plain" } }),
  value => value.headers.get("content-type")?.startsWith("text/plain") === true,
);
bench(
  "text Response with Headers",
  () => new Response("hello", { headers: textHeaders }),
  value => value.headers.get("content-type")?.startsWith("text/plain") === true,
);
