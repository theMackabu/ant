import assert from 'node:assert';
import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({ memory: '128mb' });
try {
  await assert.rejects(sandbox.run('tests/sandbox/fixtures/exit_code_guest.js'), err => {
    assert.strictEqual(err.name, 'SandboxScriptExit');
    assert.strictEqual(err.code, 3);
    return true;
  });
} finally {
  await sandbox.terminate().catch(() => {});
}

console.log('sandbox exit code: ok');
