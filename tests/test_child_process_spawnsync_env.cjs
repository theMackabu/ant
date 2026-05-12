const { spawnSync } = require('child_process');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const result = spawnSync(
  process.execPath,
  ['-e', 'console.log(process.env.ANT_SYNC_ENV_TEST)'],
  {
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_SYNC_ENV_TEST: 'present'
    }
  }
);

assert(result.status === 0, `child exited ${result.status}`);
assert(result.stdout === 'present\n', `expected env in stdout, got ${JSON.stringify(result.stdout)}`);

console.log('child_process.spawnSync env ok');
