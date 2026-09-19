const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const source = `
  const assert = require('node:assert');
  const reason = new Error('cron continuation');
  let calls = 0, reports = 0;
  const deadline = setTimeout(() => { console.error('cron never resumed'); process.exit(1); }, 3000);
  process.on('uncaughtException', error => {
    assert.strictEqual(error, reason);
    reports++;
  });
  const job = Ant.cron('@yearly', () => {
    if (++calls === 1) {
      const promise = Promise.resolve();
      Object.defineProperty(promise, 'constructor', { get() { throw reason; } });
      return promise;
    }
    job.stop();
    clearTimeout(deadline);
    assert.strictEqual(reports, 1);
    console.log('cron resumed');
  });
`;
const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_TEST_CRON_TIMER_MS: '5' }, encoding: 'utf8', timeout: 5000,
});
assert.strictEqual(result.status, 0, result.stderr);
assert.match(result.stdout, /cron resumed/);
console.log('Cron continuation error ok');
