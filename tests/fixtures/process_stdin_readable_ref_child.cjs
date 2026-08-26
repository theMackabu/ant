function cleanup() {
  try { process.stdin.setRawMode(false); } catch {}
  try { process.stdin.unref(); } catch {}
}

if (!process.stdin.isTTY || typeof process.stdin.setRawMode !== 'function') {
  console.log('SKIP');
  process.exit(0);
}

if (typeof process.stdin.ref !== 'function' || typeof process.stdin.unref !== 'function') {
  console.log('MISSING_REF_METHODS');
  process.exit(2);
}

if (process.stdin.ref() !== process.stdin) {
  console.log('BAD_REF_RETURN');
  process.exit(3);
}

process.stdin.setRawMode(true);
process.stdin.setEncoding('utf8');

const timeout = setTimeout(() => {
  cleanup();
  console.log('TIMEOUT');
  process.exit(4);
}, 2000);
timeout.unref();

process.stdin.on('readable', () => {
  let input = '';
  let chunk;
  while ((chunk = process.stdin.read()) !== null) input += chunk;
  if (input.length === 0) return;

  process.stdin.setRawMode(false);
  if (process.stdin.unref() !== process.stdin) {
    console.log('BAD_UNREF_RETURN');
    process.exit(5);
  }
  console.log('READABLE', JSON.stringify(input));
});

process.stdout.write('READY\n');
