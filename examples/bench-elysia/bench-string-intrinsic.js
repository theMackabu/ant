const iterations = Number(process.argv[2] || 2_000_000);
const selected = process.argv[3];
let sink;

function bench(name, fn) {
  if (selected && selected !== name) return;
  for (let i = 0; i < 10_000; i++) sink = fn();
  const start = performance.now();
  for (let i = 0; i < iterations; i++) sink = fn();
  const elapsed = performance.now() - start;
  console.log(name + ": " + elapsed.toFixed(2) + " ms");
}

const url = "http://localhost/";
const pathStart = "http://localhost".length;
bench("url path parse", () => {
  const start = url.indexOf("/", pathStart);
  const query = url.indexOf("?", start);
  return url.substring(start, query === -1 ? url.length : query);
});

const custom = {
  indexOf(value) {
    return value + 1;
  }
};
bench("custom indexOf fallback", () => custom.indexOf(2));

const array = [1, 2, 3, 4];
bench("array indexOf fallback", () => array.indexOf(3));

if (sink === undefined) throw new Error("benchmark result was not observed");
