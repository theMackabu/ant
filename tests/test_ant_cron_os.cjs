const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform !== 'darwin') {
  console.log('Ant.cron OS registration test skipped outside macOS');
  process.exit(0);
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cron-os-'));
const bin = path.join(root, 'bin');
const home = path.join(root, 'home');
const log = path.join(root, 'launchctl.log');
const completed = path.join(root, 'launchctl.completed');
const ant = path.resolve(process.execPath);

function run(file, extraEnv = {}) {
  const result = spawnSync(ant, ['--no-color', file], {
    cwd: root,
    encoding: 'utf8',
    env: {
      ...process.env,
      HOME: home,
      PATH: `${bin}:${process.env.PATH}`,
      ANT_CRON_LAUNCHCTL_LOG: log,
      ...extraEnv,
    },
  });
  if (result.error) throw result.error;
  assert.strictEqual(
    result.status,
    0,
    `cron OS helper failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
}

try {
  fs.mkdirSync(bin, { recursive: true });
  fs.mkdirSync(home, { recursive: true });
  const launchctl = path.join(bin, 'launchctl');
  fs.writeFileSync(
    launchctl,
    '#!/bin/sh\nprintf "%s\\n" "$*" >> "$ANT_CRON_LAUNCHCTL_LOG"\n' +
    'if [ "$ANT_CRON_FAIL_BOOTOUT" = "1" ] && [ "$1" = "bootout" ]; then exit 2; fi\n' +
    'if [ "$ANT_CRON_FAIL_BOOTSTRAP" = "1" ] && [ "$1" = "bootstrap" ]; then exit 3; fi\n' +
    'if [ "$ANT_CRON_DELAY" = "1" ]; then sleep 0.2; : > "$ANT_CRON_COMPLETED"; fi\n' +
    'exit 0\n'
  );
  fs.chmodSync(launchctl, 0o755);
  fs.writeFileSync(path.join(root, 'worker.mjs'), 'export default { scheduled() {} };\n');
  fs.writeFileSync(
    path.join(root, 'register.mjs'),
    'import fs from "node:fs";\n' +
    'const registration = Ant.cron("./worker.mjs", "*/15 * * * *", "test_job");\n' +
    'if (fs.existsSync(process.env.ANT_CRON_COMPLETED)) throw new Error("registration blocked");\n' +
    'await registration;\nconsole.log("registered");\n'
  );
  fs.writeFileSync(
    path.join(root, 'remove.mjs'),
    'await Ant.cron.remove("test_job");\nconsole.log("removed");\n'
  );

  run(path.join(root, 'register.mjs'), {
    ANT_CRON_DELAY: '1',
    ANT_CRON_COMPLETED: completed,
  });
  assert.strictEqual(fs.existsSync(completed), true);
  const plist = path.join(home, 'Library', 'LaunchAgents', 'ant.cron.test_job.plist');
  assert.strictEqual(fs.existsSync(plist), true);
  const contents = fs.readFileSync(plist, 'utf8');
  assert.match(contents, /<string>ant\.cron\.test_job<\/string>/);
  assert.match(contents, /--cron-title=test_job/);
  assert.match(contents, /--cron-period=\*\/15 \* \* \* \*/);
  assert.match(contents, /worker\.mjs/);
  assert.match(contents, /<key>StartCalendarInterval<\/key><array>/);
  assert.doesNotMatch(contents, /<key>StartCalendarInterval<\/key><dict\/>/);
  assert.doesNotMatch(contents, /StandardOutPath|StandardErrorPath/);
  assert.strictEqual((contents.match(/<key>Minute<\/key>/g) || []).length, 4);

  fs.writeFileSync(
    path.join(root, 'register.mjs'),
    'await Ant.cron("./worker.mjs", "@daily", "test_job");\nconsole.log("replaced");\n'
  );
  run(path.join(root, 'register.mjs'));
  const replaced = fs.readFileSync(plist, 'utf8');
  assert.match(replaced, /--cron-period=@daily/);
  assert.doesNotMatch(replaced, /--cron-period=\*\/15/);
  assert.match(replaced, /<key>Minute<\/key><integer>0<\/integer>/);
  assert.match(replaced, /<key>Hour<\/key><integer>0<\/integer>/);

  fs.writeFileSync(
    path.join(root, 'replace-failing.mjs'),
    'let rejected = false;\n' +
    'try { await Ant.cron("./worker.mjs", "@hourly", "test_job"); } catch (error) {\n' +
    '  rejected = /rollback failed/.test(String(error));\n' +
    '}\n' +
    'if (!rejected) throw new Error("failed replacement should reject");\n'
  );
  run(path.join(root, 'replace-failing.mjs'), { ANT_CRON_FAIL_BOOTSTRAP: '1' });
  assert.strictEqual(fs.readFileSync(plist, 'utf8'), replaced);

  fs.writeFileSync(
    path.join(root, 'too-dense.mjs'),
    'let rejected = false;\n' +
    'try { await Ant.cron("./worker.mjs", "0-58 0-22 1-30 1-11 *", "dense"); } catch (error) {\n' +
    '  rejected = /maximum 10000/.test(String(error));\n' +
    '}\n' +
    'if (!rejected) throw new Error("dense calendar should reject");\n'
  );
  run(path.join(root, 'too-dense.mjs'));

  fs.writeFileSync(
    path.join(root, 'remove-failing.mjs'),
    'let rejected = false;\n' +
    'try { await Ant.cron.remove("test_job"); } catch { rejected = true; }\n' +
    'if (!rejected) throw new Error("remove should reject");\n'
  );
  run(path.join(root, 'remove-failing.mjs'), { ANT_CRON_FAIL_BOOTOUT: '1' });
  assert.strictEqual(fs.existsSync(plist), true);

  run(path.join(root, 'remove.mjs'));
  assert.strictEqual(fs.existsSync(plist), false);
  const calls = fs.readFileSync(log, 'utf8');
  assert.match(calls, /bootstrap/);
  assert.match(calls, /bootout/);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('Ant.cron OS registration ok');
