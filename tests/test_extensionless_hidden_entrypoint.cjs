const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-hidden-entry-'));
const hiddenDir = path.join(tmpRoot, '.bin');
const scriptPath = path.join(hiddenDir, 'kat2');

fs.mkdirSync(hiddenDir, { recursive: true });
fs.writeFileSync(
  scriptPath,
  [
    '#!/usr/bin/env ant',
    '',
    'console.log("hidden entry ok");',
    '',
  ].join('\n')
);
fs.chmodSync(scriptPath, 0o755);

const env = { ...process.env };
env.PATH = `${path.dirname(process.execPath)}${path.delimiter}${env.PATH || ''}`;

const direct = spawnSync(scriptPath, [], { env });
if (direct.error) throw direct.error;

assert(
  direct.status === 0,
  `extensionless shebang entrypoint in hidden dir should exit 0, got ${direct.status}\nstdout:\n${String(direct.stdout)}\nstderr:\n${String(direct.stderr)}`
);
assert(
  String(direct.stdout) === 'hidden entry ok\n',
  `expected shebang stdout to be hidden entry ok, got ${JSON.stringify(String(direct.stdout))}`
);

const viaAnt = spawnSync(process.execPath, [scriptPath]);
if (viaAnt.error) throw viaAnt.error;

assert(
  viaAnt.status === 0,
  `direct ant entrypoint in hidden dir should exit 0, got ${viaAnt.status}\nstdout:\n${String(viaAnt.stdout)}\nstderr:\n${String(viaAnt.stderr)}`
);
assert(
  String(viaAnt.stdout) === 'hidden entry ok\n',
  `expected ant stdout to be hidden entry ok, got ${JSON.stringify(String(viaAnt.stdout))}`
);

console.log('extensionless hidden entrypoint test passed');
