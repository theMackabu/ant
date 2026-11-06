const assert = require('node:assert');
const http = require('node:http');

const watchdog = setTimeout(() => {
  console.error('timed out');
  process.exit(1);
}, 8000);

const closes = new Map();
const server = http.createServer((request, response) => {
  response.writeHead(200);
  response.write('part1');
  if (request.url === '/finished') return response.end('part2');
  request.socket.on('close', () => closes.get(request.url)?.());
});

const closed = path => new Promise(resolve => closes.set(path, resolve));
const url = path => `http://127.0.0.1:${server.address().port}${path}`;

async function readerCancelClosesConnection() {
  const seen = closed('/reader');
  const reader = (await fetch(url('/reader'))).body.getReader();
  await reader.read();
  await reader.cancel('done');
  await seen;
  assert.strictEqual((await reader.read()).done, true);
}

async function bodyCancelClosesConnection() {
  const seen = closed('/body');
  await (await fetch(url('/body'))).body.cancel();
  await seen;
}

async function cancelAfterCompletionIsHarmless() {
  const reader = (await fetch(url('/finished'))).body.getReader();
  let text = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    text += Buffer.from(value).toString();
  }
  assert.strictEqual(text, 'part1part2');
  await reader.cancel();
}

async function repeatedAndAbortCancelsAreHarmless() {
  const seen = closed('/twice');
  const controller = new AbortController();
  const reader = (await fetch(url('/twice'), { signal: controller.signal })).body.getReader();
  await reader.cancel();
  await reader.cancel();
  controller.abort();
  await seen;
}

server.listen(0, '127.0.0.1', async () => {
  try {
    await readerCancelClosesConnection();
    await bodyCancelClosesConnection();
    await cancelAfterCompletionIsHarmless();
    await repeatedAndAbortCancelsAreHarmless();
    server.close();
    clearTimeout(watchdog);
    console.log('fetch-body-cancel:ok');
  } catch (error) {
    console.error(error && error.stack ? error.stack : String(error));
    process.exit(1);
  }
});
