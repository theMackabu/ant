function now() {
  return typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now();
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[sorted.length >> 1];
}

function timeSearchPass(subject, patterns) {
  const start = now();
  let checksum = 0;
  for (const pattern of patterns) checksum += subject.search(pattern);
  const elapsed = now() - start;
  if (checksum !== -patterns.length) throw new Error(`unexpected search checksum: ${checksum}`);
  return elapsed;
}

const patternCount = 1024;
const original = Array.from({ length: patternCount }, (_, i) => `original_${i}_z`);
const shifted = Array.from({ length: patternCount }, (_, i) => `shifted_${i}_z`);
const subject = 'none of the generated cache patterns match this subject';

timeSearchPass(subject, original);
const originalWarmMs = timeSearchPass(subject, original);
const shiftedColdMs = timeSearchPass(subject, shifted);
const shiftedWarmMs = median([
  timeSearchPass(subject, shifted),
  timeSearchPass(subject, shifted),
  timeSearchPass(subject, shifted),
]);
const warmRatio = shiftedWarmMs / shiftedColdMs;

console.log(
  `regexp cache: original=${originalWarmMs.toFixed(3)}ms ` +
  `shifted-cold=${shiftedColdMs.toFixed(3)}ms ` +
  `shifted-warm=${shiftedWarmMs.toFixed(3)}ms ratio=${warmRatio.toFixed(3)}`
);

if (warmRatio >= 0.5) throw new Error(
  `shifted RegExp working set was not cached: warm/cold ratio ${warmRatio.toFixed(3)}`
);

const retained = /retained_([0-9]+)/g;
const initialRetainedMatch = retained.exec('retained_42');
if (!initialRetainedMatch || initialRetainedMatch[1] !== '42') {
  throw new Error('retained RegExp warmup failed');
}

const evictionPatterns = Array.from(
  { length: patternCount * 2 + 1 },
  (_, i) => `eviction_${i}_z`,
);
timeSearchPass(subject, evictionPatterns);

retained.lastIndex = 0;
const retainedMatch = retained.exec('retained_42');
if (!retainedMatch || retainedMatch[1] !== '42') {
  throw new Error('live RegExp lost its compiled entry during cache rotation');
}
