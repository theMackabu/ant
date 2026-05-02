function addStep(value, delta) {
  return value + delta;
}

function mix(a, b, c) {
  return (a + b) * c - a / (b + 1);
}

function readRounds() {
  const raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw | 0 : 10000;
}

const rounds = readRounds();
let checksum = 0;

for (let i = 0; i < 240; i++) {
  checksum = addStep(checksum, mix(i, i + 1, 3));
}

for (let i = 0; i < rounds; i++) {
  checksum = addStep(checksum, (i % 13) + (i % 7));
}

console.log("rounds=" + rounds);
console.log("checksum=" + checksum.toFixed(3));
console.log("fallback=" + addStep("type", "script"));
