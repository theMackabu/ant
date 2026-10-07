// An unhandled rejection goes to process 'unhandledRejection' listeners, else
// becomes an uncaught exception with origin 'unhandledRejection': an
// 'uncaughtException' listener sees it, otherwise the process exits with 1
// once the current job's microtasks are done, before later timers run.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const run = code => {
  const env = { ...process.env, NO_COLOR: '1' };
  delete env.FORCE_COLOR;
  const child = spawnSync(process.execPath, ['-e', code], { encoding: 'utf8', timeout: 30000, env });
  return [child.status, child.stdout];
};

const cases = [
  ["Promise.reject(new Error('boom')); setTimeout(() => console.log('timer ran'), 10)", 1, ''],
  ["process.on('unhandledRejection', (r, p) => console.log('listener', r.message, p instanceof Promise)); Promise.reject(new Error('boom')); setTimeout(() => console.log('timer ran'), 10)", 0, 'listener boom true\ntimer ran\n'],
  ["process.on('exit', c => console.log('exit', c, process.exitCode)); Promise.reject(new Error('boom'))", 1, 'exit 1 1\n'],
  ["const p = Promise.reject(new Error('late')); setTimeout(() => p.catch(() => console.log('caught late')), 0)", 1, ''],
  ["const p = Promise.reject(new Error('handled')); p.catch(() => console.log('caught'))", 0, 'caught\n'],
  ["process.on('uncaughtException', (e, origin) => console.log('uce', e.message, origin)); Promise.reject(new Error('boom')); setTimeout(() => console.log('timer ran'), 10)", 0, 'uce boom unhandledRejection\ntimer ran\n'],
  ["(async () => { throw new Error('async boom') })(); console.log('sync done')", 1, 'sync done\n'],
  ["Promise.reject(42)", 1, ''],
  ["process.exitCode = 3; Promise.reject(new Error('x'))", 1, ''],
  ["process.on('unhandledRejection', () => { throw new Error('from listener') }); Promise.reject(1)", 1, ''],
];

for (const [code, status, stdout] of cases) assert.deepStrictEqual(run(code), [status, stdout], code);
console.log('PASS unhandled rejections reach listeners or exit 1 like Node');
