const assert = require('assert');
const http = require('http');

let header = null;
let sent = null;

const server = http.createServer((req, res) => {
  res.statusCode = 404;
  res.setHeader('X-Test', 'implicit');
  res._implicitHeader();
  header = res._header;
  res.end('missing');
  sent = res._header;
});

server.listen(0, async () => {
  try {
    const response = await fetch(`http://127.0.0.1:${server.address().port}/`);
    assert.strictEqual(response.status, 404);
    assert.strictEqual(response.statusText, 'Not Found');
    assert.strictEqual(await response.text(), 'missing');

    assert.match(header, /^HTTP\/1\.1 404 Not Found\r\n/);
    assert.match(header, /\r\nX-Test: implicit\r\n/);
    assert.match(header, /\r\nDate: /);
    assert.match(header, /\r\nTransfer-Encoding: chunked\r\n/);
    assert.strictEqual(sent, header);

    console.log('http server implicit header: ok');
  } finally {
    server.close();
  }
});
