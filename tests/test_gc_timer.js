// Timer GC stress: interval-driven allocation without coroutines

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
  const width = 120;
  const height = 40;
  const lines = [];

  for (let y = 0; y < height; y++) {
    lines.push(' '.repeat(width));
  }

  for (let y = 0; y < height; y++) {
    let text = `\x1b[38;5;196mRow ${y}\x1b[0m: `;
    text += `\x1b[38;5;82m${'█'.repeat(20)}\x1b[0m`;
    text += `\x1b[2m${'░'.repeat(20)}\x1b[0m`;
    text = pad(text, width);
    lines[y] = text;
  }

  return lines.join('\n');
}

function handleEvent(n) {
  for (let i = 0; i < 10; i++) {
    render();
  }
}

let count = 0;
const interval = setInterval(() => {
  count++;
  handleEvent(count);

  const stats = Ant.stats();
  console.log(
    `tick ${count}: arena ${(stats.arenaUsed / 1024 / 1024).toFixed(1)}MB / ` +
    `${(stats.arenaSize / 1024 / 1024).toFixed(1)}MB, ` +
    `rss ${(stats.rss / 1024 / 1024).toFixed(1)}MB`
  );

  if (count >= 500) {
    clearInterval(interval);
    console.log('Done - forcing GC...');
    Ant.gc();
    const after = Ant.stats();
    console.log(`after GC: arena ${(after.arenaUsed / 1024 / 1024).toFixed(1)}MB`);
  }
}, 10);
