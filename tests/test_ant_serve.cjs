const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function waitForLine(child) {
  return new Promise((resolve, reject) => {
    let stdout = '';
    let stderr = '';
    const timeout = setTimeout(() => {
      child.kill('SIGTERM');
      reject(new Error(`timed out waiting for server metadata\nstdout:\n${stdout}\nstderr:\n${stderr}`));
    }, 2000);

    child.stdout.on('data', chunk => {
      stdout += String(chunk);
      const newline = stdout.indexOf('\n');
      if (newline === -1) return;
      clearTimeout(timeout);
      resolve(stdout.slice(0, newline));
    });

    child.stderr.on('data', chunk => {
      stderr += String(chunk);
    });

    child.on('exit', code => {
      clearTimeout(timeout);
      if (code !== null && stdout.indexOf('\n') === -1) {
        reject(new Error(`server exited before metadata; code=${code}\nstderr:\n${stderr}`));
      }
    });
  });
}

async function waitForExit(child) {
  if (child.exitCode !== null) return child.exitCode;
  return await new Promise(resolve => child.once('exit', resolve));
}

async function main() {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-serve-api-'));
  const serverPath = path.join(tmpDir, 'server.mjs');

  fs.writeFileSync(serverPath, `
const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(request, ctx) {
    const url = new URL(request.url);
    if (ctx !== server) return new Response('ctx mismatch', { status: 500 });
    if (server.hostname !== '127.0.0.1') return new Response('bad hostname', { status: 500 });
    if (typeof server.port !== 'number' || server.port <= 0) return new Response('bad port', { status: 500 });
    if (typeof server.stop !== 'function') return new Response('bad stop', { status: 500 });
    if (url.pathname === '/stop') {
      queueMicrotask(() => { server.stop(); });
      return new Response('stopping');
    }
    return new Response(JSON.stringify({
      hostname: server.hostname,
      port: server.port,
      url: server.url,
      requestIP: typeof server.requestIP,
      timeout: typeof server.timeout,
      upgradeWebSocket: typeof server.upgradeWebSocket,
      eventSource: typeof server.eventSource,
    }));
  },
});
console.log(JSON.stringify({
  hostname: server.hostname,
  port: server.port,
  url: server.url,
  stop: typeof server.stop,
}));
`);

  const child = spawn(process.execPath, [serverPath], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  try {
    const metadata = JSON.parse(await waitForLine(child));
    assert.equal(metadata.hostname, '127.0.0.1');
    assert.equal(typeof metadata.port, 'number');
    assert(metadata.port > 0);
    assert.equal(metadata.url, `http://127.0.0.1:${metadata.port}`);
    assert.equal(metadata.stop, 'function');

    const response = await fetch(`http://127.0.0.1:${metadata.port}/`);
    assert.equal(response.status, 200);
    const body = await response.json();
    assert.equal(body.hostname, '127.0.0.1');
    assert.equal(body.port, metadata.port);
    assert.equal(body.url, metadata.url);
    assert.equal(body.requestIP, 'function');
    assert.equal(body.timeout, 'function');
    assert.equal(body.upgradeWebSocket, 'function');
    assert.equal(body.eventSource, 'function');

    const stop = await fetch(`http://127.0.0.1:${metadata.port}/stop`);
    assert.equal(await stop.text(), 'stopping');
    assert.equal(await waitForExit(child), 0);

    console.log('ant:serve:ok');
  } finally {
    if (child.exitCode === null) child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
