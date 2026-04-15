const assert = require('node:assert');
const http = require('node:http');

const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);

  const body = Buffer.concat(chunks).toString('utf8');

  if (req.url === '/redirect') {
    res.writeHead(302, { location: '/final' });
    res.end('redirecting');
    return;
  }

  if (req.url === '/redirect-post') {
    res.writeHead(302, { location: '/final-post' });
    res.end('redirecting post');
    return;
  }

  if (req.url === '/redirect-307') {
    res.writeHead(307, { location: '/final-307' });
    res.end('redirecting preserve');
    return;
  }

  if (req.url === '/final') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end('redirect-ok');
    return;
  }

  if (req.url === '/final-post') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end(`${req.method}:${body}`);
    return;
  }

  if (req.url === '/final-307') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end(`${req.method}:${body}`);
    return;
  }

  res.writeHead(404);
  res.end('missing');
});

server.listen(0, async () => {
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;

  try {
    const followed = await fetch(`${base}/redirect`);
    assert.equal(followed.status, 200);
    assert.equal(followed.redirected, true);
    assert.equal(followed.url, `${base}/final`);
    assert.equal(await followed.text(), 'redirect-ok');

    const rewritten = await fetch(`${base}/redirect-post`, {
      method: 'POST',
      body: 'hello-body',
    });
    assert.equal(rewritten.status, 200);
    assert.equal(await rewritten.text(), 'GET:');

    const preserved = await fetch(`${base}/redirect-307`, {
      method: 'POST',
      body: 'hello-again',
    });
    assert.equal(preserved.status, 200);
    assert.equal(await preserved.text(), 'POST:hello-again');

    const manual = await fetch(`${base}/redirect`, { redirect: 'manual' });
    assert.equal(manual.status, 302);
    assert.equal(manual.redirected, false);
    assert.equal(manual.url, `${base}/redirect`);

    let sawRedirectError = false;
    try {
      await fetch(`${base}/redirect`, { redirect: 'error' });
    } catch (error) {
      sawRedirectError = /redirect mode is set to error/.test(String(error));
    }
    assert.equal(sawRedirectError, true);

    console.log('ok');
  } finally {
    server.close();
  }
});
