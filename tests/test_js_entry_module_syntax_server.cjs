const assert = require('node:assert');
const { spawn, spawnSync } = require('node:child_process');
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

function assertExplicitCommonJsRejectsModuleSyntax() {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cjs-module-syntax-'));
  const entryPath = path.join(tmpDir, 'entry.js');
  const packagePath = path.join(tmpDir, 'package.json');

  try {
    fs.writeFileSync(packagePath, '{"type":"commonjs"}\n');
    fs.writeFileSync(entryPath, 'export default 1;\n');

    const result = spawnSync(process.execPath, [entryPath], { encoding: 'utf8' });
    if (result.error) throw result.error;

    assert.notEqual(result.status, 0, 'explicit CommonJS file with export syntax should fail');
    assert.match(
      result.stderr,
      /Cannot use import\/export syntax in CommonJS|Cannot use import\/export syntax outside a module/,
      `expected CJS module syntax error, got stderr:\n${result.stderr}`
    );
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

async function main() {
  const port = await reservePort();
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-js-entry-module-syntax-'));
  const serverPath = path.join(tmpDir, 'server.js');
  const sideEffectPath = path.join(tmpDir, 'side-effect.js');
  const packagePath = path.join(tmpDir, 'package.json');

  fs.writeFileSync(packagePath, '{"dependencies":{}}\n');
  fs.writeFileSync(sideEffectPath, 'globalThis.__syntaxDetectedJs = "syntax-detected-js";\n');
  fs.writeFileSync(serverPath, `
import './side-effect.js';

export default {
  hostname: '127.0.0.1',
  port: ${port},
  fetch() {
    return new Response(globalThis.__syntaxDetectedJs);
  },
};
`);

  const child = spawn(process.execPath, [serverPath], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  let stdout = '';
  let stderr = '';
  child.stdout.on('data', chunk => { stdout += String(chunk); });
  child.stderr.on('data', chunk => { stderr += String(chunk); });

  try {
    await waitForServer(port, child);

    const response = await new Promise((resolve, reject) => {
      let data = '';
      const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
        socket.write('GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n');
      });
      socket.on('data', chunk => { data += String(chunk); });
      socket.on('end', () => resolve(data));
      socket.on('error', reject);
    });

    assert.match(response, /^HTTP\/1\.1 /, `expected HTTP response, got ${JSON.stringify(response)}`);
    assert.match(response, /\r\n\r\nsyntax-detected-js$/, `expected server export to handle request, got ${JSON.stringify(response)}`);
    assert.equal(child.exitCode, null, `server crashed\nstdout:\n${stdout}\nstderr:\n${stderr}`);
    assertExplicitCommonJsRejectsModuleSyntax();
    console.log('js entry module syntax starts exported server');
  } finally {
    child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
