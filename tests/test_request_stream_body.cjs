const assert = require('node:assert');

const encoder = new TextEncoder();

function streamOf(...chunks) {
  return new ReadableStream({
    start(controller) {
      for (const chunk of chunks) controller.enqueue(chunk);
      controller.close();
    },
  });
}

function requestWith(body) {
  return new Request('http://localhost/', { method: 'POST', body, duplex: 'half' });
}

(async () => {
  assert.strictEqual(await requestWith(streamOf(encoder.encode('a'), encoder.encode('b'))).text(), 'ab');
  assert.strictEqual((await requestWith(streamOf(encoder.encode('xyz'))).arrayBuffer()).byteLength, 3);
  assert.deepStrictEqual(await requestWith(streamOf(encoder.encode('{"a":1}'))).json(), { a: 1 });

  const piped = streamOf('p', 'q').pipeThrough(new TextEncoderStream());
  assert.strictEqual(await requestWith(piped).text(), 'pq');

  await assert.rejects(requestWith(streamOf('str')).text(), TypeError);
  await assert.rejects(requestWith(streamOf(new Uint16Array([1]))).text(), TypeError);

  console.log('ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
