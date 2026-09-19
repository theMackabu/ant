// Operational failures delivered to callbacks/events/promises must not remain
// pending runtime exceptions or expose the internal throwing sentinel.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-io-handled-'));
let index = 0;

const helpers = `
function assert(condition, message) {
  if (!condition) throw new Error(message);
}
function checkError(error) {
  assert(error instanceof Error, 'expected an Error object');
  assert(typeof error.message === 'string' && error.message.length > 0, 'expected an error message');
}
async function rejected(promise) {
  let error;
  try { await promise; } catch (caught) { error = caught; }
  checkError(error);
}
async function closedPort() {
  const server = require('node:net').createServer();
  await new Promise(resolve => server.listen({ host: '127.0.0.1', port: 0 }, resolve));
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}
`;

const cases = {
  tls: `
    const socket = require('node:tls').connect({ host: '127.0.0.1', port: await closedPort() });
    let handled = false;
    let closed = false;
    try {
      await new Promise(resolve => {
        socket.on('error', error => { checkError(error); handled = true; });
        socket.on('close', () => { closed = true; resolve(); });
      });
      assert(handled, 'TLS error handler did not run');
    } finally { if (!closed) socket.destroy(); }
  `,
  childWrite: `
    const child = require('node:child_process').spawn(process.execPath, ['-e',
      "require('node:fs').closeSync(0); console.log('READY'); setTimeout(() => {}, 100);"
    ]);
    let wrote = false;
    let handled = false;
    child.stdin.on('error', checkError);
    try {
      await new Promise(resolve => {
        child.stdout.on('data', () => {
          if (wrote) return;
          wrote = true;
          child.stdin.write('data', error => { checkError(error); handled = true; });
        });
        child.on('close', resolve);
      });
      assert(handled, 'child write callback did not handle the error');
    } finally { child.kill(); }
  `,
  netClose: `
    let handled = false;
    require('node:net').createServer().close(error => { checkError(error); handled = true; });
    await new Promise(resolve => setTimeout(resolve, 0));
    assert(handled, 'server close callback did not run');
  `,
  rpc: `
    const { RpcClient } = require('ant:rpc');
    const client = new RpcClient({ host: '127.0.0.1', port: await closedPort() });
    try { await rejected(client.connect()); } finally { await client.close(); }
    await rejected(client.connect());
    await rejected(client.ping());
    await rejected(client.call('unused', []));
  `,
  rpcListen: `
    const { RpcServer } = require('ant:rpc');
    const server = new RpcServer();
    try {
      await rejected(server.listen());
      await rejected(server.listen({ port: 0, integrity: 'invalid' }));
    } finally { await server.close(); }
  `,
  dns: `
    const dns = require('node:dns').promises;
    // c-ares rejects this name before sending any network traffic.
    await rejected(dns.resolve('bad..name', 'A'));
    await rejected(dns.resolve('localhost', 'INVALID'));
  `,
};

function run(source) {
  const file = path.join(dir, `case${index++}.cjs`);
  fs.writeFileSync(file, source);
  return spawnSync(process.execPath, [file], { encoding: 'utf8', timeout: 5000 });
}

try {
  for (const [name, body] of Object.entries(cases)) {
    for (const observeUncaught of [false, true]) {
      const result = run(helpers + `
        ${observeUncaught ? "process.on('uncaughtException', error => console.log('UNCAUGHT ' + error.message));" : ''}
        process.on('unhandledRejection', error => console.log('UNHANDLED ' + error.message));
        const deadline = setTimeout(() => { console.error('TIMEOUT'); process.exit(2); }, 3000);
        (async () => {
          ${body}
          await new Promise(resolve => setTimeout(resolve, 10));
          console.log('PASS');
        })().catch(error => {
          console.error(error.stack || error);
          process.exitCode = 1;
        }).finally(() => clearTimeout(deadline));
      `);
      const label = `${name} (uncaught observer: ${observeUncaught})`;
      assert(result.status === 0, `${label}: exit ${result.status}; ${result.stderr}`);
      assert(result.stdout.includes('PASS'), `${label}: incomplete; ${result.stdout}; ${result.stderr}`);
      assert(!result.stdout.includes('UNCAUGHT'), `${label}: ${result.stdout}`);
      assert(!result.stdout.includes('UNHANDLED'), `${label}: ${result.stdout}`);
    }
  }

  // Converting an operational error must happen before invoking user code.
  // A new exception thrown by the callback must still escape and be reported.
  const thrown = run(`
    require('node:net').createServer().close(() => { throw new Error('close-callback-threw'); });
  `);
  assert(thrown.status === 1, `throwing close callback: exit ${thrown.status}`);
  assert(thrown.stderr.includes('close-callback-threw'), `throwing close callback: ${thrown.stderr}`);
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}

console.log('PASS');
