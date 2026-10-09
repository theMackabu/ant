const assert = require('assert');
const http = require('http');

const seen = {};

const server = http.createServer((req, res) => {
  assert.strictEqual(res._header, null);

  if (req.url === '/explicit') {
    res.writeHead(201, { 'X-Test': 'explicit' });
    seen.explicit = res._header;
    res.end('ok');
  } else {
    res.setHeader('X-Test', 'implicit');
    res.end('ok');
    seen.implicit = res._header;
  }
});

server.listen(0, async () => {
  const { port } = server.address();
  try {
    await (await fetch(`http://127.0.0.1:${port}/explicit`)).text();
    await (await fetch(`http://127.0.0.1:${port}/implicit`)).text();

    assert.strictEqual(typeof seen.explicit, 'string');
    assert.match(seen.explicit, /^HTTP\/1\.1 201 /);
    assert.match(seen.explicit, /\r\nX-Test: explicit\r\n/);
    assert.ok(seen.explicit.endsWith('\r\n\r\n'));

    assert.strictEqual(typeof seen.implicit, 'string');
    assert.match(seen.implicit, /^HTTP\/1\.1 200 /);
    assert.match(seen.implicit, /\r\nX-Test: implicit\r\n/);
    assert.ok(seen.implicit.endsWith('\r\n\r\n'));

    console.log('http server response _header: ok');
  } finally {
    server.close();
  }
});
