// A generator abandoned by breaking out of its loop finished, but its
// coroutine and captured activation were only retired, never freed, while
// the script kept running: destruction waited for the next event loop turn,
// so a synchronous loop kept every one of them (~375 bytes each, 757 MiB for
// two million). They are now freed as soon as no resume is in progress.
function* g() { yield 1; yield 2; yield 3; }
let s = 0;
for (let i = 0; i < 500000; i++) { for (const v of g()) { s += v; break; } }
if (s !== 500000) throw new Error('checksum mismatch: ' + s);
const rss = process.memoryUsage().rss / 1048576;
if (rss > 120) throw new Error('rss too high after abandoned generators: ' + rss.toFixed(0) + ' MiB');
console.log('generator-abandon-memory: ok');
