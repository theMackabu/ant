import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({
  memory: '256mb',
  timeoutMs: 60_000,
  cpuTimeMs: 5_000
});

const reporting = setInterval(() => {
  const stats = sandbox.stats();
  console.log({
    cpuTimeMs: Math.round(stats.cpuTimeMs),
    wallTimeMs: Math.round(stats.wallTimeMs),
    residentMemory: stats.residentMemory
  });
}, 500);

try {
  await sandbox.run('examples/demo/cpu-limit/guest.js');
} catch (error) {
  console.log(`${error.name}: ${error.message}`);
} finally {
  clearInterval(reporting);
  console.log('final stats:', sandbox.stats());
  await sandbox.terminate();
}
