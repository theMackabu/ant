const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const path = require('node:path');

const ant = path.resolve(process.execPath);
const fixture = path.resolve(__dirname, 'fixtures/cron_scheduled.mjs');
const timerFixture = path.resolve(__dirname, 'fixtures/cron_job_timer.mjs');

const help = spawnSync(ant, ['--help'], { encoding: 'utf8' });
assert.strictEqual(help.status, 0, help.stderr);
assert.doesNotMatch(help.stdout, /--cron-(?:title|period)/);

const result = spawnSync(
  ant,
  ['--no-color', '--cron-title=test-job', '--cron-period=* * * * *', fixture],
  { encoding: 'utf8' }
);
assert.strictEqual(
  result.status,
  0,
  `cron runner failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);
const controller = JSON.parse(result.stdout.trim());
assert.strictEqual(controller.cron, '* * * * *');
assert.strictEqual(controller.type, 'scheduled');
assert.strictEqual(typeof controller.scheduledTime, 'number');
assert.ok(Math.abs(Date.now() - controller.scheduledTime) < 30_000);

const late = spawnSync(
  ant,
  ['--no-color', '--cron-title=test-job', '--cron-period=@yearly', fixture],
  { encoding: 'utf8' }
);
assert.strictEqual(late.status, 0, late.stderr);
assert.strictEqual(JSON.parse(late.stdout.trim()).cron, '@yearly');

const rejected = spawnSync(
  ant,
  ['--no-color', '--cron-title=test-job', '--cron-period=* * * * *', fixture],
  { encoding: 'utf8', env: { ...process.env, ANT_CRON_REJECT: '1' } }
);
assert.strictEqual(rejected.status, 1);
assert.match(rejected.stderr, /scheduled rejection/);

const incomplete = spawnSync(ant, ['--no-color', '--cron-period=* * * * *', fixture], {
  encoding: 'utf8',
});
assert.notStrictEqual(incomplete.status, 0);
assert.match(incomplete.stderr, /--cron-title and --cron-period must be used together/);

const timer = spawnSync(ant, ['--no-color', timerFixture], {
  encoding: 'utf8',
  env: { ...process.env, ANT_TEST_CRON_TIMER_MS: '5' },
  timeout: 5_000,
});
assert.strictEqual(
  timer.status,
  0,
  `cron timer fixture failed\nstdout:\n${timer.stdout}\nstderr:\n${timer.stderr}`
);
assert.deepStrictEqual(JSON.parse(timer.stdout.trim()), {
  calls: 2,
  maxActive: 1,
  receiverMatches: true,
});

console.log('Ant.cron scheduled runner ok');
