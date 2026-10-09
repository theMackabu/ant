const assert = require('assert');
const http = require('http');

const routes = {
  '/head-then-end': res => {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
  },
  '/head-then-write': res => {
    res.writeHead(200);
    res.write('a');
    res.write('b');
    res.end('c');
  },
  '/length-then-write': res => {
    res.writeHead(200, { 'Content-Length': '3' });
    res.write('ab');
    res.end('c');
  },
  '/implicit': res => {
    res.end('plain');
  },
  '/no-content': res => {
    res.writeHead(204);
    res.end();
  }
};

const server = http.createServer((req, res) => routes[req.url](res));

server.listen(0, async () => {
  const base = `http://127.0.0.1:${server.address().port}`;
  try {
    for (let round = 0; round < 2; round++) {
      assert.deepStrictEqual(await (await fetch(`${base}/head-then-end`)).json(), { ok: true });
      assert.strictEqual(await (await fetch(`${base}/head-then-write`)).text(), 'abc');
      assert.strictEqual(await (await fetch(`${base}/length-then-write`)).text(), 'abc');
      assert.strictEqual(await (await fetch(`${base}/implicit`)).text(), 'plain');

      const empty = await fetch(`${base}/no-content`);
      assert.strictEqual(empty.status, 204);
      assert.strictEqual(await empty.text(), '');
    }

    console.log('http server response framing: ok');
  } finally {
    server.close();
  }
});
