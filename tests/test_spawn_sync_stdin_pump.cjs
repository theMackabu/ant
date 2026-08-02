const { spawnSync } = require('child_process');

let failed = 0;
function check(name, ok, detail) {
  if (!ok) failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${ok ? '' : ` (${detail})`}`);
}

// child floods stdout (200KB > pipe buffer) before reading any stdin, while the
// parent supplies 1MB of input; requires interleaved stdin/stdout pumping
const big = spawnSync('sh', ['-c', 'dd if=/dev/zero bs=1024 count=200 2>/dev/null; cat > /dev/null; echo done'], {
  input: 'x'.repeat(1 << 20),
  encoding: 'utf8',
});
check('no deadlock, child completed', big.status === 0, `status ${big.status}`);
check('stdout fully captured', big.stdout.length === 204805, `len ${big.stdout.length}`);
check('trailing echo present', big.stdout.endsWith('done\n'), JSON.stringify(big.stdout.slice(-8)));

// timeout must be able to interrupt a child that never reads stdin
const t0 = Date.now();
const timed = spawnSync('sh', ['-c', 'sleep 30'], { input: 'x'.repeat(1 << 20), timeout: 1500, encoding: 'utf8' });
check('timeout fired', timed.error && timed.error.message.includes('ETIMEDOUT'), timed.error && timed.error.message);
check('killed by signal', timed.signal === 'SIGTERM', `signal ${timed.signal}`);
check('returned promptly', Date.now() - t0 < 10000, `${Date.now() - t0}ms`);

// stdin round-trip integrity through the nonblocking writer
const echo = spawnSync('cat', [], { input: 'a'.repeat(300000) + 'END', encoding: 'utf8' });
check('large input round-trips', echo.stdout.length === 300003 && echo.stdout.endsWith('END'), `len ${echo.stdout.length}`);

if (failed) { console.log(`${failed} checks failed`); process.exit(1); }
console.log('OK');
