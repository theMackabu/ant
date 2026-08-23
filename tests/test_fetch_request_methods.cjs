const assert = require('node:assert');
const http = require('node:http');

const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);

  const payload = JSON.stringify({
    method: req.method,
    body: Buffer.concat(chunks).toString('utf8'),
    contentLength: req.headers['content-length'],
    contentType: req.headers['content-type'],
    transferEncoding: req.headers['transfer-encoding'],
  });

  res.writeHead(200, { 'content-type': 'application/json' });
  res.end(payload);
});

server.listen(0, async () => {
  const { port } = server.address();
  const url = `http://127.0.0.1:${port}/echo`;

  try {
    for (const method of ['PUT', 'PATCH', 'QUERY']) {
      const response = await fetch(url, {
        method,
        body: `${method} body`,
      });
      const request = await response.json();

      assert.equal(response.status, 200);
      assert.equal(request.method, method);
      assert.equal(request.body, `${method} body`);
      assert.equal(request.contentLength, String(Buffer.byteLength(`${method} body`)));
      assert.equal(request.contentType, 'text/plain;charset=UTF-8');
    }

    const encoder = new TextEncoder();
    const streamedResponse = await fetch(url, {
      method: 'QUERY',
      body: new ReadableStream({
        start(controller) {
          controller.enqueue(encoder.encode('{"kind":'));
          controller.enqueue(encoder.encode('"stream"}'));
          controller.close();
        },
      }),
      duplex: 'half',
      headers: { 'content-type': 'application/json' },
    });
    const streamedRequest = await streamedResponse.json();

    assert.equal(streamedRequest.method, 'QUERY');
    assert.equal(streamedRequest.body, '{"kind":"stream"}');
    assert.equal(streamedRequest.contentType, 'application/json');
    assert.equal(streamedRequest.transferEncoding, 'chunked');

    console.log('ok');
  } finally {
    server.close();
  }
});
