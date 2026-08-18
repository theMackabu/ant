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

function run(file, extraEnv = {}) {
  const result = spawnSync(ant, ['--no-color', file], {
    cwd: root,
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_CRON_SCHTASKS: `"${ant}" "${helper}"`,
      ANT_CRON_XML_COPY: xmlCopy,
      ...extraEnv,
    },
  });
  if (result.error) throw result.error;
  assert.strictEqual(
    result.status,
    0,
    `cron Windows helper failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
}

try {
  fs.writeFileSync(
    helper,
    'import fs from "node:fs";\n' +
    'const index = process.argv.findIndex(value => value.toLowerCase() === "/xml");\n' +
    'if (index >= 0) fs.copyFileSync(process.argv[index + 1], process.env.ANT_CRON_XML_COPY);\n' +
    'if (process.env.ANT_CRON_SCHTASKS_EXIT === "1") console.error("Aufgabe wurde nicht gefunden.");\n' +
    'process.exit(Number(process.env.ANT_CRON_SCHTASKS_EXIT || 0));\n'
  );
  fs.writeFileSync(path.join(root, 'worker.mjs'), 'export default { scheduled() {} };\n');
  fs.writeFileSync(
    path.join(root, 'register.mjs'),
    'await Ant.cron("./worker.mjs", "0 9 * * MON-FRI", "weekday_job");\n'
  );
  run(path.join(root, 'register.mjs'));
  let xml = fs.readFileSync(xmlCopy, 'utf8');
  assert.match(xml, /<LogonType>S4U<\/LogonType>/);
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Monday\/>/);
  assert.match(xml, /<Friday\/>/);

  fs.writeFileSync(
    path.join(root, 'business-hours.mjs'),
    'await Ant.cron("./worker.mjs", "0,30 9-17 * * MON-FRI", "business_hours");\n'
  );
  run(path.join(root, 'business-hours.mjs'));
  xml = fs.readFileSync(xmlCopy, 'utf8');
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 18);
  assert.strictEqual((xml.match(/<Monday\/>/g) || []).length, 18);
  assert.strictEqual((xml.match(/<Friday\/>/g) || []).length, 18);

  fs.writeFileSync(
    path.join(root, 'repeat.mjs'),
    'await Ant.cron("./worker.mjs", "*/5 * * * *", "repeat_job");\n'
  );
  run(path.join(root, 'repeat.mjs'));
  xml = fs.readFileSync(xmlCopy, 'utf8');
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Interval>PT5M<\/Interval>/);

  fs.writeFileSync(
    path.join(root, 'wildcard.mjs'),
    'await Ant.cron("./worker.mjs", "* * * * *", "wildcard_job");\n'
  );
  run(path.join(root, 'wildcard.mjs'));
  xml = fs.readFileSync(xmlCopy, 'utf8');
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Interval>PT1M<\/Interval>/);

  fs.writeFileSync(
    path.join(root, 'semantic-repeat.mjs'),
    'await Ant.cron("./worker.mjs", "0,20,40 * * * *", "semantic_repeat");\n'
  );
  run(path.join(root, 'semantic-repeat.mjs'));
  xml = fs.readFileSync(xmlCopy, 'utf8');
  assert.strictEqual((xml.match(/<CalendarTrigger>/g) || []).length, 1);
  assert.match(xml, /<Interval>PT20M<\/Interval>/);

  fs.writeFileSync(
    path.join(root, 'limit.mjs'),
    'let rejected = false;\n' +
    'try { await Ant.cron("./worker.mjs", "*/7 * * * *", "too_many"); } catch (error) {\n' +
    '  rejected = /maximum 48/.test(String(error));\n' +
    '}\n' +
    'if (!rejected) throw new Error("trigger limit should reject");\n'
  );
  run(path.join(root, 'limit.mjs'));

  fs.writeFileSync(path.join(root, 'remove.mjs'), 'await Ant.cron.remove("missing");\n');
  run(path.join(root, 'remove.mjs'), { ANT_CRON_SCHTASKS_EXIT: '1' });
  fs.writeFileSync(
    path.join(root, 'remove-failing.mjs'),
    'let rejected = false;\n' +
    'try { await Ant.cron.remove("denied"); } catch { rejected = true; }\n' +
    'if (!rejected) throw new Error("scheduler failure should reject");\n'
  );
  run(path.join(root, 'remove-failing.mjs'), { ANT_CRON_SCHTASKS_EXIT: '5' });
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('Ant.cron Windows registration ok');
