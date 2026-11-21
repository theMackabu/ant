const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

const once = (emitter, event) => new Promise(resolve => emitter.once(event, resolve));

function raw(port, parts) {
  return new Promise((resolve, reject) => {
    let out = '';
    const socket = net.connect(port, '127.0.0.1', async () => {
      for (const part of parts) {
        socket.write(part);
        await new Promise(done => setTimeout(done, 5));
      }
    });
    socket.on('data', chunk => { out += String(chunk); });
    socket.on('close', () => resolve(out));
    socket.on('error', reject);
  });
}

(async () => {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-server-fd-'));
  const serverPath = path.join(tmpDir, 'server.mjs');
  fs.writeFileSync(serverPath, `
export default {
  fd: 3,
  async fetch(request) {
    return Response.json({
      url: request.url,
      host: request.headers.get('host'),
      trailer: request.headers.get('x-t'),
      body: await request.text(),
    });
  },
};
`);

  const listener = net.createServer(() => assert.fail('parent accepted a connection'));
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const { port } = listener.address();

  const child = spawn(process.execPath, [serverPath], { stdio: ['ignore', 'ignore', 'pipe', listener] });
  listener.close();
  let stderr = '';
  child.stderr.on('data', chunk => { stderr += String(chunk); });

  try {
    const head = 'POST /p HTTP/1.1\r\nHost: good.test\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n';
    const body = '3\r\nabc\r\n0\r\nHost: evil.test\r\nX-T: 1\r\n\r\n';

    for (const parts of [[head + body], [head, '3\r\nabc\r\n', '0\r\nHost: evil.test\r\n', 'X-T: 1\r\n\r\n']]) {
      const response = await raw(port, parts);
      assert.match(response, /^HTTP\/1\.1 200 /, response + stderr);
      assert.deepStrictEqual(JSON.parse(response.split('\r\n\r\n', 2)[1]), {
        url: 'http://good.test/p',
        host: 'good.test',
        trailer: null,
        body: 'abc',
      });
    }
  } finally {
    child.kill();
    await once(child, 'exit');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }

  console.log('ok');
})();
