const assert = require('node:assert');

const watchdog = setTimeout(() => {
  console.error('timed out');
  process.exit(1);
}, 8000);

const reported = [];
process.on('unhandledRejection', error => reported.push(error.message));

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const handlers = {
  '/async-throw': async () => {
    throw new Error('handled');
  },
  '/async-await-throw': async () => {
    await null;
    throw new Error('handled');
  },
  '/rejected': () => Promise.reject(new Error('handled')),
  '/reject-later': () => new Promise((_, reject) => setTimeout(() => reject(new Error('handled')), 1)),
  '/sync-throw': () => {
    throw new Error('handled');
  },
  '/stray': () => {
    Promise.reject(new Error('stray'));
    return new Response('ok');
  },
  '/ok': () => new Response('ok'),
  '/not-a-response': () => 42,
  '/async-not-a-response': async () => 42,
};

const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(request) {
    return handlers[new URL(request.url).pathname]();
  },
});

async function main() {
  for (const path of ['/async-throw', '/async-await-throw', '/rejected', '/reject-later', '/sync-throw']) {
    const response = await fetch(`http://127.0.0.1:${server.port}${path}`);
    assert.strictEqual(response.status, 500, path);
    assert.strictEqual(await response.text(), 'Internal Server Error', path);
  }

  for (const path of ['/not-a-response', '/async-not-a-response']) {
    const response = await fetch(`http://127.0.0.1:${server.port}${path}`);
    assert.strictEqual(response.status, 500, path);
    assert.strictEqual(await response.text(), 'Internal Server Error', path);
  }

  await sleep(10);
  assert.deepStrictEqual(reported, [], 'a rejection the server handled is not reported');

  const after = await fetch(`http://127.0.0.1:${server.port}/ok`);
  assert.strictEqual(await after.text(), 'ok');

  const stray = await fetch(`http://127.0.0.1:${server.port}/stray`);
  assert.strictEqual(await stray.text(), 'ok');
  await sleep(10);
  assert.deepStrictEqual(reported, ['stray'], 'a rejection nobody handled is still reported');

  await server.stop();
  clearTimeout(watchdog);
  console.log('server-handler-errors:ok');
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
