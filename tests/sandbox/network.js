import assert from 'node:assert';
import http from 'node:http';
import { Sandbox } from 'ant:sandbox';

function listen() {
  return new Promise(resolve => {
    const server = http.createServer((req, res) => {
      if (req.url === '/echo') {
        let received = 0;
        req.on('data', chunk => { received += chunk.length; });
        req.on('end', () => res.end(String(received)));
        return;
      }
      res.end(Buffer.alloc(Number(req.url.split('?')[1]), 0x61));
    });
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

const allowed = await listen();
const blocked = await listen();
const port = allowed.address().port;
const sizes = [1000, 70000, 300000, 2000000];
const upload = 200000;

const sandbox = new Sandbox({ memory: '128mb', allowHost: port });
const running = sandbox.run('tests/sandbox/fixtures/network_guest.js');
try {
  assert.deepStrictEqual(await sandbox.once('ready'), { type: 'ready' });
  sandbox.send({ port, blocked: blocked.address().port, sizes, upload });
  assert.deepStrictEqual(await sandbox.once('result'), { type: 'result', sizes, upload, blocked: 'refused' });
} finally {
  await sandbox.terminate();
  await running.catch(() => {});
  allowed.close();
  blocked.close();
}

console.log('sandbox network: ok');
