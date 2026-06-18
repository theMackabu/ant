const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

async function reservePort() {
  return await new Promise((resolve, reject) => {
    const server = net.createServer();
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}

async function waitForServer(port, child) {
  const startedAt = Date.now();
  while (Date.now() - startedAt < 2000) {
    assert.equal(child.exitCode, null, 'server exited before accepting connections');
    try {
      await new Promise((resolve, reject) => {
        const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
          socket.end();
          resolve();
        });
        socket.once('error', reject);
      });
      return;
    } catch {
      await new Promise(resolve => setTimeout(resolve, 10));
    }
  }
  throw new Error('server did not start');
}

async function main() {
  const port = await reservePort();
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-server-invalid-target-'));
  const serverPath = path.join(tmpDir, 'server.mjs');

  fs.writeFileSync(serverPath, `
export default {
  hostname: '127.0.0.1',
  port: ${port},
  fetch() {
    return new Response('ok');
  },
};
`);

  const child = spawn(process.execPath, [serverPath], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  let stderr = '';
  child.stderr.on('data', chunk => { stderr += String(chunk); });

  try {
    await waitForServer(port, child);

    const response = await new Promise((resolve, reject) => {
      let data = '';
      const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
        socket.write('GET http:// HTTP/1.1\\r\\nHost: 127.0.0.1\\r\\nConnection: close\\r\\n\\r\\n');
      });
      socket.on('data', chunk => { data += String(chunk); });
      socket.on('end', () => resolve(data));
      socket.on('error', reject);
    });

    assert.match(response, /^HTTP\/1\.1 /, `expected HTTP response, got ${JSON.stringify(response)}`);
    assert.equal(child.exitCode, null, `server crashed: ${stderr}`);
    console.log('ok');
  } finally {
    child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
