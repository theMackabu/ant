const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

function reservePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(error => error ? reject(error) : resolve(port));
    });
  });
}

async function waitForServer(port, child) {
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
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

function rawRequest(port, method, target, host, splitTarget = false) {
  return new Promise((resolve, reject) => {
    let data = '';
    const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
      const requestTail =
        ` HTTP/1.1\r\n` +
        `Host: ${host}\r\n` +
        'X-Mixed-Case: \t value \t\r\n' +
        'X-Duplicate: first\r\n' +
        'X-Duplicate: second\r\n' +
        'X-Empty:\r\n' +
        'Connection: close\r\n\r\n';
      if (splitTarget) {
        socket.write(`${method} ${target.slice(0, 1)}`);
        setTimeout(() => socket.write(target.slice(1) + requestTail), 1);
      } else {
        socket.write(`${method} ${target}${requestTail}`);
      }
    });
    socket.on('data', chunk => { data += String(chunk); });
    socket.on('end', () => resolve(data));
    socket.on('error', reject);
  });
}

async function main() {
  const port = await reservePort();
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-server-request-url-'));
  const serverPath = path.join(tmpDir, 'server.mjs');
  fs.writeFileSync(serverPath, `
export default {
  hostname: '127.0.0.1',
  port: ${port},
  fetch(request) {
    const copy = new Request(request, {
      cache: 'reload',
      credentials: 'include',
      integrity: 'copied',
      mode: 'cors',
      redirect: 'manual',
      referrer: 'https://referrer.example/',
      referrerPolicy: 'origin',
    });
    return Response.json({
      url: request.url,
      method: request.method,
      referrer: request.referrer,
      referrerPolicy: request.referrerPolicy,
      mode: request.mode,
      credentials: request.credentials,
      cache: request.cache,
      redirect: request.redirect,
      integrity: request.integrity,
      headers: {
        mixed: request.headers.get('x-mixed-case'),
        duplicate: request.headers.get('X-Duplicate'),
        empty: request.headers.get('x-empty'),
      },
      copy: {
        referrer: copy.referrer,
        referrerPolicy: copy.referrerPolicy,
        mode: copy.mode,
        credentials: copy.credentials,
        cache: copy.cache,
        redirect: copy.redirect,
        integrity: copy.integrity,
      },
    });
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
    const cases = [
      ['GET', '/', 'example.test:8080', true],
      ['GET', '/', '127.0.0.1:8080'],
      ['GET', '/a/../b?x=1', 'example.test'],
      ['GET', '/%E2%9C%93?q=%20', 'example.test:8080'],
      ['GET', '//other.test/x', 'example.test'],
      ['GET', '/ipv6', '[::1]:8080'],
      ['POST', '/submit', 'example.test'],
      ['GET', '/split', 'example.test', true],
    ];

    for (const [method, target, host, splitTarget] of cases) {
      const response = await rawRequest(port, method, target, host, splitTarget);
      assert.match(response, /^HTTP\/1\.1 200 /, response);
      const payload = JSON.parse(response.split('\r\n\r\n', 2)[1]);
      assert.equal(payload.url, new URL(target, `http://${host}/`).href);
      assert.deepEqual(
        {
          method: payload.method,
          referrer: payload.referrer,
          referrerPolicy: payload.referrerPolicy,
          mode: payload.mode,
          credentials: payload.credentials,
          cache: payload.cache,
          redirect: payload.redirect,
          integrity: payload.integrity,
        },
        {
          method,
          referrer: 'about:client',
          referrerPolicy: '',
          mode: 'same-origin',
          credentials: 'same-origin',
          cache: 'default',
          redirect: 'follow',
          integrity: '',
        },
      );
      assert.deepEqual(payload.copy, {
        referrer: 'https://referrer.example/',
        referrerPolicy: 'origin',
        mode: 'cors',
        credentials: 'include',
        cache: 'reload',
        redirect: 'manual',
        integrity: 'copied',
      });
      assert.deepEqual(payload.headers, {
        mixed: 'value',
        duplicate: 'first, second',
        empty: '',
      });
    }
    assert.equal(child.exitCode, null, stderr);
    console.log('server Request URLs and shared defaults preserve semantics');
  } finally {
    child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
