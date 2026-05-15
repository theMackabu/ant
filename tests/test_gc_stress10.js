import { Screen, colors, codes, pad, truncate } from '../examples/tui/tuey.js';

const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

const screen = new Screen({ fullscreen: false, hideCursor: false });

const logs = [
  { time: '10:23:45', level: 'INFO', message: 'Application started' },
  { time: '10:23:48', level: 'WARN', message: 'Cache directory not found, creating...' },
  { time: '10:24:05', level: 'ERROR', message: 'Failed to load plugin: missing-plugin' },
  { time: '10:24:06', level: 'WARN', message: 'Running with reduced functionality' },
  { time: '10:24:10', level: 'INFO', message: 'Ready to accept connections' }
];

function render() {
  screen.write(2, 14, colors.bold + colors.magenta + '┌─ Recent Activity ───────────────────────┐' + codes.reset);
  for (let i = 0; i < 5; i++) {
    const log = logs[i];
    const levelColor = log.level === 'ERROR' ? colors.red : log.level === 'WARN' ? colors.yellow : colors.green;
    screen.write(2, 15 + i, colors.magenta + '│' + codes.reset);
    screen.write(4, 15 + i, `${colors.dim}${log.time}${codes.reset} ${levelColor}${log.level}${codes.reset} ${truncate(log.message, 30)}`);
    screen.write(45, 15 + i, colors.magenta + '│' + codes.reset);
  }
  screen.write(2, 20, colors.magenta + '└──────────────────────────────────────────┘' + codes.reset);
}

const t0 = Date.now();
for (let frame = 0; frame < 50000; frame++) {
  render();
  const stats = Ant.stats();
  const elapsed = ((Date.now() - t0) / 1000).toFixed(2);
  const a = stats.alloc, M = 1024 * 1024;
  const total = (stats.pools.totalUsed + a.total) / M;
  console.log(
    `frame ${frame}: pools ${(stats.pools.totalUsed/M).toFixed(1)}MB obj ${(a.objects/M).toFixed(1)}MB shp ${(a.shapes/M).toFixed(1)}MB arr ${(a.arrays/M).toFixed(1)}MB refs ${(a.propRefs/M).toFixed(1)}MB cls ${(a.closures/M).toFixed(1)}MB uv ${(a.upvalues/M).toFixed(1)}MB ov ${(a.overflow/M).toFixed(1)}MB | ${total.toFixed(1)}MB rss ${(stats.rss/M).toFixed(1)}MB ${elapsed}s`
  );
}

const total = ((Date.now() - t0) / 1000).toFixed(2);
const f = Ant.stats();
const fmem = (f.pools.totalUsed + f.alloc.total) / 1024 / 1024;
console.log(`Done: ${total}s, total ${fmem.toFixed(1)}MB, rss ${(f.rss / 1024 / 1024).toFixed(1)}MB`);
console.log('OK');
__gcPerfLog();
