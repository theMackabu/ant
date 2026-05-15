const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

// Simulate TUI-like behavior: async handler with lots of string allocations

function stripAnsi(str) {
  return str.replace(/\x1b\[[0-9;]*m/g, '');
}

function pad(str, len) {
  const visible = stripAnsi(str).length;
  const diff = len - visible;
  if (diff <= 0) return str;
  return str + ' '.repeat(diff);
}

function render() {
  // Simulate TUI render - matches what tui.js does
  const width = 120;
  const height = 40;
  const lines = [];

  // Clear buffer
  for (let y = 0; y < height; y++) {
    lines.push(' '.repeat(width));
  }

  // Draw styled content (like the TUI does)
  for (let y = 0; y < height; y++) {
    let text = `\x1b[38;5;196mRow ${y}\x1b[0m: `;
    text += `\x1b[38;5;82m${'█'.repeat(20)}\x1b[0m`;
    text += `\x1b[2m${'░'.repeat(20)}\x1b[0m`;
    text = pad(text, width);
    lines[y] = text;
  }

  return lines.join('\n');
}

async function handleEvent(n) {
  // Simulate multiple renders per event (like scrolling does)
  for (let i = 0; i < 10; i++) {
    render();
  }
}

let count = 0;
const interval = setInterval(() => {
  count++;
  handleEvent(count);

  const s = Ant.stats(), a = s.alloc, M = 1024 * 1024;
  const total = (s.pools.totalUsed + a.total) / M;
  console.log(
    `tick ${count}: pools ${(s.pools.totalUsed/M).toFixed(1)}MB obj ${(a.objects/M).toFixed(1)}MB shp ${(a.shapes/M).toFixed(1)}MB arr ${(a.arrays/M).toFixed(1)}MB refs ${(a.propRefs/M).toFixed(1)}MB cls ${(a.closures/M).toFixed(1)}MB uv ${(a.upvalues/M).toFixed(1)}MB ov ${(a.overflow/M).toFixed(1)}MB | ${total.toFixed(1)}MB rss ${(s.rss/M).toFixed(1)}MB`
  );

  if (count >= 500) {
    clearInterval(interval);
    setTimeout(() => {
      const a = Ant.stats();
      console.log(`done: total ${((a.pools.totalUsed + a.alloc.total) / 1024 / 1024).toFixed(1)}MB`);
      __gcPerfLog();
    }, 10);
  }
}, 10); // 500 * 10ms = 5 seconds
