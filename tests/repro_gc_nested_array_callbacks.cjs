// Reproduce the native stack shape from the Yet crash without app dependencies:
// timer -> Array.prototype.flatMap -> Array.prototype.map -> JIT array literal -> minor GC.
const rounds = Number(process.argv[2] || 1000);
const retained = new Array(31);
const source = ['alpha', 'beta', 'gamma', 'delta'];
const expectedLength = source.reduce((total, word) => total + word.length, 0);
let round = 0;
let checksum = 0;

function forceMinor(seed) {
  let last = null;
  for (let i = 0; i < 50000; i++) {
    last = {
      seed,
      i,
      row: [seed, i, i + 1, { value: i + 2 }],
    };
  }
  return last.i;
}

function render(seed) {
  return source.flatMap((text, outerIndex) =>
    Array.from(text).map((character, innerIndex) => {
      if (outerIndex === 1 && innerIndex === 1) forceMinor(seed);
      return [
        { text: character, style: seed & 1 ? 'dim' : 'bright' },
        { outerIndex, innerIndex, seed },
      ];
    }),
  );
}

function tick() {
  const end = Math.min(round + 10, rounds);
  for (; round < end; round++) {
    const output = render(round);
    if (output.length !== expectedLength) throw new Error('bad length at ' + round + ': ' + output.length);
    for (let i = 0; i < output.length; i++) {
      const [segment, metadata] = output[i];
      if (!segment || !metadata || metadata.seed !== round || segment.text.length === 0)
        throw new Error('corrupt output at round ' + round + ', index ' + i);
      checksum += metadata.outerIndex + metadata.innerIndex + metadata.seed;
    }
    retained[round % retained.length] = output;
  }
  if (round % 100 === 0) console.log('round=' + round + ' checksum=' + checksum);
  if (round < rounds) setTimeout(tick, 0);
  else console.log('PASS: ' + rounds + ' rounds');
}

setTimeout(tick, 0);
