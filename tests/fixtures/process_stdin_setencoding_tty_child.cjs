function cleanup() {
  try { process.stdin.setRawMode(false); } catch {}
  try { process.stdin.pause(); } catch {}
}

if (!process.stdin.isTTY || typeof process.stdin.setRawMode !== 'function') {
  console.log('SKIP');
  process.exit(0);
}

process.stdin.setRawMode(true);
process.stdin.setEncoding('utf8');
process.stdin.resume();
process.stdout.write('READY\n');

const timeout = setTimeout(() => {
  cleanup();
  console.log('TIMEOUT');
  process.exit(3);
}, 2000);

process.stdin.on('data', chunk => {
  clearTimeout(timeout);
  cleanup();

  if (typeof chunk !== 'string') {
    console.log('TYPE', typeof chunk);
    process.exit(4);
  }

  console.log('DATA', JSON.stringify(chunk));
  process.exit(chunk === 'A€' ? 0 : 5);
});
