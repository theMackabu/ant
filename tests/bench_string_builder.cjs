const now =
  typeof performance !== "undefined" && performance && typeof performance.now === "function"
    ? () => performance.now()
    : () => Date.now();

const scale =
  typeof process !== "undefined" && process.argv && process.argv.length > 2
    ? Math.max(1, Number(process.argv[2]) || 1)
    : 1;

let sink = 0;

function mixString(value) {
  const len = value.length;
  sink = (sink + len * 33 + (len ? value.charCodeAt(len - 1) : 0)) | 0;
}

function bench(name, iterations, fn) {
  const warmup = Math.max(1, iterations >> 5);
  for (let i = 0; i < warmup; i++) mixString(fn(i));

  const start = now();
  for (let i = 0; i < iterations; i++) mixString(fn(i));
  const elapsed = now() - start;
  const opsPerSec = elapsed > 0 ? (iterations * 1000) / elapsed : 0;

  console.log(
    name +
      ": " +
      elapsed.toFixed(2) +
      "ms (" +
      iterations +
      " ops, " +
      opsPerSec.toFixed(0) +
      " ops/s)"
  );
}

function makeTemplateCase(count, literalSize) {
  const data = {};
  let template = "";

  for (let i = 0; i < count; i++) {
    const key = "p" + i;
    data[key] = i % 3 === 0 ? "value" + i : i % 3 === 1 ? i : (i & 1) === 0;
    template += "x".repeat(literalSize) + "{{" + key + "}}";
  }

  template += "tail";
  return { template, data };
}

function makeRawCase(count, literalSize) {
  const raw = [];
  const args = [{ raw }];

  for (let i = 0; i < count; i++) {
    raw[i] = "r".repeat(literalSize);
    if (i + 1 < count) args[i + 1] = i % 3 === 0 ? "sub" + i : i % 3 === 1 ? i : true;
  }

  return args;
}

const smallTemplate = "Hello {{name}} {{count}} {{ok}}.";
const smallTemplateData = { name: "Ant", count: 42, ok: true };
const largeTemplateCase = makeTemplateCase(96, 24);

const smallRawArgs = [{ raw: ["Hello ", " ", "."] }, "Ant", 42];
const largeRawArgs = makeRawCase(128, 24);

const objectToString = Object.prototype.toString;
const smallTagObject = { [Symbol.toStringTag]: "SmallTag" };
const largeTagObject = { [Symbol.toStringTag]: "T".repeat(800) };

const hugeLeft = "L".repeat(320 * 1024);
const hugeRight = "R".repeat(320 * 1024);

console.log("String builder bench scale=" + scale);

bench("String.prototype.template small/static", 250000 * scale, () => {
  return smallTemplate.template(smallTemplateData);
});

bench("String.prototype.template large/spill", 25000 * scale, () => {
  return largeTemplateCase.template.template(largeTemplateCase.data);
});

bench("String.raw small/static", 200000 * scale, () => {
  return String.raw.apply(String, smallRawArgs);
});

bench("String.raw large/realloc", 20000 * scale, () => {
  return String.raw.apply(String, largeRawArgs);
});

bench("Object.prototype.toString small/static", 300000 * scale, () => {
  return objectToString.call(smallTagObject);
});

bench("Object.prototype.toString large/spill", 150000 * scale, () => {
  return objectToString.call(largeTagObject);
});

bench("large binary concat flatten", 300 * scale, () => {
  return hugeLeft + hugeRight;
});

console.log("sink=" + sink);
