const assert = require('node:assert');
const net = require('node:net');

const holder = net.createServer();
holder.listen(0, '127.0.0.1', async () => {
  const { port } = holder.address();

  for (let attempt = 0; attempt < 4; attempt++) {
    assert.throws(
      () => Ant.serve({ hostname: '127.0.0.1', port, fetch: () => new Response('no') }),
      { code: 'EADDRINUSE' }
    );
    assert.throws(() => Ant.serve({ fd: 1 << 20, fetch: () => new Response('no') }), { code: 'EINVAL' });
    await new Promise(resolve => setTimeout(resolve, 5));
  }

  holder.close(() => {
    const server = Ant.serve({ hostname: '127.0.0.1', port, fetch: () => new Response('yes') });
    fetch(`http://127.0.0.1:${port}/`).then(response => response.text()).then(async text => {
      assert.strictEqual(text, 'yes');
      await server.stop(true);
      console.log('ok');
    });
  });
});
