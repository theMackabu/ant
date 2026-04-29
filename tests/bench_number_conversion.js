const now =
  typeof performance !== "undefined" && performance && typeof performance.now === "function"
    ? () => performance.now()
    : () => Date.now();

const scale =
  typeof process !== "undefined" && process.argv && process.argv.length > 2
    ? Math.max(1, Number(process.argv[2]) || 1)
    : 1;

let sink = 0;

function mixNumber(value) {
  sink = (sink + (value * 1000003) | 0) ^ (sink << 5);
}

function mixString(value) {
  sink = (sink + value.length * 33 + value.charCodeAt(value.length - 1)) | 0;
}

function bench(name, iterations, fn) {
  const warmup = Math.max(1, iterations >> 4);
  fn(warmup);

  const start = now();
  const ops = fn(iterations);
  const elapsed = now() - start;
  const nsPerOp = elapsed > 0 ? (elapsed * 1e6) / ops : 0;
  const opsPerSec = elapsed > 0 ? (ops * 1000) / elapsed : 0;

  console.log(
    name +
      ": " +
      elapsed.toFixed(2) +
      "ms (" +
      ops +
      " ops, " +
      nsPerOp.toFixed(2) +
      " ns/op, " +
      opsPerSec.toFixed(0) +
      " ops/s)"
  );
}

const decimalStrings = [
  "0.7875",
  "2.675",
  "123456789.125",
  "-0.0000033333333333333333",
  "1.7976931348623157e308",
  "5e-324",
  "   42.5   ",
  "0x10",
  "0b101010",
  "0o755",
];

const floatStrings = [
  "0.7875px",
  "2.675 and change",
  "  Infinity!",
  "-0.0000033333333333333333ms",
  "123456789.125;",
  "5e-324end",
];

const literalSources = [
  "0.7875",
  "2.675",
  "123456789.125",
  "-0.0000033333333333333333",
  "1.7976931348623157e308",
  "5e-324",
  "1_234_567.8_9",
  "1.e+1",
];

const numbers = [
  0.7875,
  0.7876,
  2.675,
  123456789.125,
  -0.0000033333333333333333,
  1.7976931348623157e308,
  5e-324,
  Math.PI,
];

console.log("Number conversion benchmark (scale " + scale + ")");

bench("Number(string)", 500000 * scale, n => {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += Number(decimalStrings[i % decimalStrings.length]);
  }
  mixNumber(sum);
  return n;
});

bench("parseFloat(prefix)", 500000 * scale, n => {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += parseFloat(floatStrings[i % floatStrings.length]);
  }
  mixNumber(sum);
  return n;
});

bench("eval(decimal literal)", 60000 * scale, n => {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += eval(literalSources[i % literalSources.length]);
  }
  mixNumber(sum);
  return n;
});

bench("String(number)", 500000 * scale, n => {
  for (let i = 0; i < n; i++) {
    mixString(String(numbers[i & 7]));
  }
  return n;
});

bench("number.toFixed(20)", 250000 * scale, n => {
  for (let i = 0; i < n; i++) {
    mixString(numbers[i & 7].toFixed(20));
  }
  return n;
});

bench("number.toPrecision(17)", 250000 * scale, n => {
  for (let i = 0; i < n; i++) {
    mixString(numbers[i & 7].toPrecision(17));
  }
  return n;
});

bench("number.toExponential(20)", 250000 * scale, n => {
  for (let i = 0; i < n; i++) {
    mixString(numbers[i & 7].toExponential(20));
  }
  return n;
});

console.log("sink:", sink);
