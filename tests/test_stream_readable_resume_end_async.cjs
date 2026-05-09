// Minimal repro for an Ant streams ordering bug surfaced by undici's
// `consumeStart` (node_modules/undici/lib/api/readable.js).
//
// undici does (paraphrased):
//
//   stream.on('end', onEnd);          // attach AFTER buffer drain
//   stream.resume();                  // start flowing
//   while (stream.read() != null) {}  // drain anything still buffered
//
// Where `onEnd` may null out an object that the loop touches. Node guarantees
// that `'end'` is emitted asynchronously (via process.nextTick), so the
// `while (stream.read())` loop always runs before `onEnd`. On Ant the `'end'`
// event currently fires synchronously inside `.resume()` once the buffer is
// drained, so `onEnd` runs before the loop and a downstream null-deref
// crashes (the original symptom: `TypeError: Cannot read properties of null
// (reading 'read')` deep inside undici).

const { Readable } = require('node:stream');

async function main() {
  const order = [];

  const r = new Readable({
    read() {
      this.push('chunk');
      this.push(null);
    },
  });

  // Force the buffer to fill without consuming it (mirrors how undici sees
  // the stream after the response body has been pushed in but no consumer
  // is attached yet).
  r.read(0);

  await new Promise(setImmediate);

  r.on('end', () => order.push('end'));

  r.resume();
  order.push('after-resume');

  // Mimic undici's drain loop. `r` should still be a usable stream here
  // because `'end'` must not have fired yet.
  while (r.read() != null) order.push('read');
  order.push('after-loop');

  await new Promise(setImmediate);

  const got = JSON.stringify(order);
  // 'end' MUST come last: it is required to be deferred past the synchronous
  // tail of `.resume()` and the read() loop.
  const want = JSON.stringify(['after-resume', 'read', 'after-loop', 'end']);
  if (got !== want) {
    throw new Error(
      `stream emitted 'end' synchronously inside resume()/read()\n` +
        `  expected: ${want}\n` +
        `  actual:   ${got}`
    );
  }

  console.log("readable defers 'end' past resume()/read()");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
