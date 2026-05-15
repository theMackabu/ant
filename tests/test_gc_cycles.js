const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

console.log('=== Testing Multiple GC Cycles ===\n');

let cycleData = { iteration: 0 };
for (let i = 0; i < 5; i = i + 1) {
  console.log('Cycle', i);
  cycleData.iteration = i;
  cycleData['data' + i] = { value: i * 10 };
  console.log('  Before GC - iteration:', cycleData.iteration);
  console.log('  After GC - iteration:', cycleData.iteration);
}

console.log('\nFinal check:');
console.log('  iteration:', cycleData.iteration);
console.log('  data3.value:', cycleData.data3.value);

console.log('\n=== Done ===');
__gcPerfLog();
