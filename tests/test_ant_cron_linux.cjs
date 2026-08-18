const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform !== 'linux') {
  console.log('Ant.cron Linux registration test skipped outside Linux');
  process.exit(0);
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cron-linux-'));
const bin = path.join(root, 'bin');
const state = path.join(root, 'crontab');
const completed = path.join(root, 'completed');
const ant = path.resolve(process.execPath);

function run(file, extraEnv = {}) {
  const result = spawnSync(ant, ['--no-color', file], {
    cwd: root,
    encoding: 'utf8',
    env: {
      ...process.env,
      PATH: `${bin}:${process.env.PATH}`,
      ANT_CRON_STATE: state,
      ANT_CRON_COMPLETED: completed,
      ...extraEnv,
    },
  });
  if (result.error) throw result.error;
  assert.strictEqual(
    result.status,
    0,
    `cron Linux helper failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
}

try {
  fs.mkdirSync(bin, { recursive: true });
  const crontab = path.join(bin, 'crontab');
  fs.writeFileSync(
    crontab,
    '#!/bin/sh\n' +
    'if [ "$1" = "-l" ]; then\n' +
    '  if [ "$ANT_CRON_FAIL_READ" = "1" ]; then echo "permission denied" >&2; exit 2; fi\n' +
    '  if [ -f "$ANT_CRON_STATE" ]; then cat "$ANT_CRON_STATE"; exit 0; fi\n' +
    '  echo "no crontab for test" >&2; exit 1\n' +
    'fi\n' +
    'if [ "$ANT_CRON_DELAY" = "1" ]; then sleep 0.2; : > "$ANT_CRON_COMPLETED"; fi\n' +
    'cp "$1" "$ANT_CRON_STATE"\n'
  );
  fs.chmodSync(crontab, 0o755);
  // Keep the final line unterminated to cover filtering's newline growth.
  fs.writeFileSync(state, '5 4 * * * /usr/bin/existing-job');
  fs.writeFileSync(path.join(root, 'worker.mjs'), 'export default { scheduled() {} };\n');
  fs.writeFileSync(
    path.join(root, 'register.mjs'),
    'import fs from "node:fs";\n' +
    'const registration = Ant.cron("./worker.mjs", "*/15 * * * *", "test_job");\n' +
    'if (fs.existsSync(process.env.ANT_CRON_COMPLETED)) throw new Error("registration blocked");\n' +
    'await registration;\n'
  );
  run(path.join(root, 'register.mjs'), { ANT_CRON_DELAY: '1' });
  let contents = fs.readFileSync(state, 'utf8');
  assert.match(contents, /existing-job/);
  assert.match(contents, /# ant-cron: test_job/);

  fs.writeFileSync(
    path.join(root, 'concurrent.mjs'),
    'await Promise.all([\n' +
    '  Ant.cron("./worker.mjs", "@hourly", "parallel_a"),\n' +
    '  Ant.cron("./worker.mjs", "@daily", "parallel_b"),\n' +
    ']);\n'
  );
  run(path.join(root, 'concurrent.mjs'));
  contents = fs.readFileSync(state, 'utf8');
  assert.match(contents, /# ant-cron: parallel_a/);
  assert.match(contents, /# ant-cron: parallel_b/);

  fs.writeFileSync(
    path.join(root, 'read-failing.mjs'),
    'let rejected = false;\n' +
    'try { await Ant.cron("./worker.mjs", "@daily", "other_job"); } catch { rejected = true; }\n' +
    'if (!rejected) throw new Error("failed crontab read should reject");\n'
  );
  const beforeFailure = contents;
  run(path.join(root, 'read-failing.mjs'), { ANT_CRON_FAIL_READ: '1' });
  assert.strictEqual(fs.readFileSync(state, 'utf8'), beforeFailure);

  fs.writeFileSync(
    path.join(root, 'remove.mjs'),
    'await Ant.cron.remove("test_job");\n'
  );
  run(path.join(root, 'remove.mjs'));
  contents = fs.readFileSync(state, 'utf8');
  assert.match(contents, /existing-job/);
  assert.doesNotMatch(contents, /ant-cron: test_job/);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('Ant.cron Linux registration ok');
