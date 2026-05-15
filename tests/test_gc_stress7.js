const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

// Minimal crash: pad() + string concat in a nested loop
function pad(s, n) {
  while (s.length < n) s += ' ';
  return s;
}

let sink = [];
for (let i = 0; i < 10000; i++) {
  for (let j = 0; j < 5; j++) {
    sink.push(pad('INFO', 5) + ' ' + pad('hello', 15));
  }
  if (sink.length > 100) sink = [];
}

console.log('OK');
__gcPerfLog();
