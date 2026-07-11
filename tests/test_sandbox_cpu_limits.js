import assert from 'node:assert';
import { Sandbox } from 'ant:sandbox';

const idle = new Sandbox({ memory: '128mb', cpuTimeMs: 5_000 });
let ready;
const idleReady = new Promise(resolve => { ready = resolve; });
idle.on('message', message => {
  if (message.type === 'ready') ready();
});

const idleRun = idle.run('tests/fixtures/sandbox_cpu_idle.js').catch(error => error);
await idleReady;
const idleBefore = idle.stats();
await new Promise(resolve => setTimeout(resolve, 500));
const idleAfter = idle.stats();
assert.ok(idleAfter.wallTimeMs - idleBefore.wallTimeMs >= 400);
assert.ok(idleAfter.cpuTimeMs - idleBefore.cpuTimeMs < 100);
await idle.terminate();
const idleResult = await idleRun;
assert.strictEqual(idleResult.name, 'SandboxCanceled');

const busy = new Sandbox({ memory: '128mb', timeoutMs: 5_000, cpuTimeMs: 1_000 });
let busyError;
try {
  await busy.run('tests/fixtures/sandbox_cpu_busy.js');
} catch (error) {
  busyError = error;
}
assert.strictEqual(busyError.name, 'SandboxCpuTimeLimit');
const busyStats = busy.stats();
assert.ok(busyStats.cpuTimeMs >= 1_000);
assert.ok(busyStats.cpuTimeMs < 2_000);
assert.ok(busyStats.wallTimeMs < 5_000);
await busy.terminate();

console.log('sandbox cpu limits: ok');
