const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform !== 'win32') {
  console.log('Ant.cron Windows registration test skipped outside Windows');
  process.exit(0);
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cron-windows-'));
const ant = path.resolve(process.execPath);
const helper = path.join(root, 'schtasks-helper.mjs');
const xmlCopy = path.join(root, 'task.xml');
const argvCopy = path.join(root, 'schtasks-argv.json');

function writeOperation(name, source) {
  const file = path.join(root, name);
  fs.writeFileSync(
    file,
    'try {\n' + source +
    '} catch (error) {\n' +
    '  console.error(String(error));\n' +
    '  process.exit(1);\n' +
    '}\n'
  );
  return file;
}

function run(file, extraEnv = {}) {
  const result = spawnSync(ant, ['--no-color', file], {
    cwd: root,
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_CRON_SCHTASKS: `"${ant}" "${helper}"`,
      ANT_CRON_XML_COPY: xmlCopy,
      ANT_CRON_ARGV_COPY: argvCopy,
      ...extraEnv,
    },
  });
  if (result.error) throw result.error;
  assert.strictEqual(
    result.status,
    0,
    `cron Windows helper failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
  return result;
}

function runRegistration(name, source) {
  fs.rmSync(xmlCopy, { force: true });
  fs.rmSync(argvCopy, { force: true });
  const result = run(writeOperation(name, source));
  const helperArgv = fs.existsSync(argvCopy)
    ? fs.readFileSync(argvCopy, 'utf8')
    : '(scheduler helper was not invoked)';
  assert.ok(
    fs.existsSync(xmlCopy),
    `scheduler helper did not copy task XML\nargv: ${helperArgv}\n` +
      `stdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
  return fs.readFileSync(xmlCopy, 'utf8');
}

try {
  fs.writeFileSync(
    helper,
    'import fs from "node:fs";\n' +
    'fs.writeFileSync(process.env.ANT_CRON_ARGV_COPY, JSON.stringify(process.argv));\n' +
    'const index = process.argv.findIndex(value => value.toLowerCase() === "/xml");\n' +
    'if (index >= 0) fs.copyFileSync(process.argv[index + 1], process.env.ANT_CRON_XML_COPY);\n' +
    'if (process.env.ANT_CRON_SCHTASKS_EXIT === "1") console.error("Aufgabe wurde nicht gefunden.");\n' +
    'process.exit(Number(process.env.ANT_CRON_SCHTASKS_EXIT || 0));\n'
  );
  fs.writeFileSync(path.join(root, 'worker.mjs'), 'export default { scheduled() {} };\n');
  let xml = runRegistration(
    'register.mjs',
    'await Ant.cron("./worker.mjs", "0 9 * * MON-FRI", "weekday_job");\n'
  );
  assert.match(xml, /<LogonType>S4U<\/LogonType>/);
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Monday\/>/);
  assert.match(xml, /<Friday\/>/);

  xml = runRegistration(
    'business-hours.mjs',
    'await Ant.cron("./worker.mjs", "0,30 9-17 * * MON-FRI", "business_hours");\n'
  );
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 18);
  assert.strictEqual((xml.match(/<Monday\/>/g) || []).length, 18);
  assert.strictEqual((xml.match(/<Friday\/>/g) || []).length, 18);

  xml = runRegistration(
    'repeat.mjs',
    'await Ant.cron("./worker.mjs", "*/5 * * * *", "repeat_job");\n'
  );
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Interval>PT5M<\/Interval>/);

  xml = runRegistration(
    'wildcard.mjs',
    'await Ant.cron("./worker.mjs", "* * * * *", "wildcard_job");\n'
  );
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Interval>PT1M<\/Interval>/);

  xml = runRegistration(
    'semantic-repeat.mjs',
    'await Ant.cron("./worker.mjs", "0,20,40 * * * *", "semantic_repeat");\n'
  );
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Interval>PT20M<\/Interval>/);

  const limit = writeOperation(
    'limit.mjs',
    'let rejected = false;\n' +
    'try { await Ant.cron("./worker.mjs", "*/7 * * * *", "too_many"); } catch (error) {\n' +
    '  rejected = /maximum 48/.test(String(error));\n' +
    '}\n' +
    'if (!rejected) throw new Error("trigger limit should reject");\n'
  );
  run(limit);

  const remove = writeOperation('remove.mjs', 'await Ant.cron.remove("missing");\n');
  run(remove, { ANT_CRON_SCHTASKS_EXIT: '1' });
  const removeFailing = writeOperation(
    'remove-failing.mjs',
    'let rejected = false;\n' +
    'try { await Ant.cron.remove("denied"); } catch { rejected = true; }\n' +
    'if (!rejected) throw new Error("scheduler failure should reject");\n'
  );
  run(removeFailing, { ANT_CRON_SCHTASKS_EXIT: '5' });
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('Ant.cron Windows registration ok');
