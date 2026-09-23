// A remote import blocks by running the event loop again from inside the
// caller. Started from a socket callback, that inner uv_run() polls while the
// outer poll is still using the loop's event buffer, so it must use its own.
// The module server here is on the same loop, so every request is served by
// the nested run. Each module allocates enough to collect under the nested
// run; run with ANT_GC_STRESS on a verify build to collect at every step.
import assert from 'node:assert';
import http from 'node:http';
import net from 'node:net';

let served = 0;
const server = http.createServer((req, res) => {
  served++;
  const n = Number(new URL(req.url, 'http://x').searchParams.get('n'));
  res.setHeader('content-type', 'text/javascript');
  res.end(`
    const junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ i, s: 'v' + i });
    export const value = ${n} + junk.length;
  `);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}/mod.js`;

const results = [];

const echo = net.createServer(sock => sock.on('data', d => sock.write(d)));
await new Promise(resolve => echo.listen(0, '127.0.0.1', resolve));

await new Promise((resolve, reject) => {
  const client = net.connect(echo.address().port, '127.0.0.1', () => client.write('a'));
  let rounds = 0;
  client.on('data', async () => {
    try {
      const outer = await import(`${base}?n=${rounds}`);
      results.push(outer.value);
      if (++rounds < 5) return client.write('b');
      client.end();
      resolve();
    } catch (err) {
      reject(err);
    }
  });
  client.on('error', reject);
});

echo.close();
server.close();

assert.deepStrictEqual(results, [20000, 20001, 20002, 20003, 20004]);
assert.strictEqual(served, 5);
console.log('PASS remote imports from socket callbacks run nested event loops');
