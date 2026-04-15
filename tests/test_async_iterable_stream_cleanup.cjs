function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function createAsyncIterableStream(source, trace) {
  const stream = source.pipeThrough(new TransformStream());

  stream[Symbol.asyncIterator] = function() {
    const reader = this.getReader();
    let finished = false;

    async function cleanup(cancelStream) {
      if (finished) return;
      finished = true;

      try {
        if (cancelStream) {
          trace.push('cleanup-before-cancel');
          await reader.cancel();
          trace.push('cleanup-after-cancel');
        }
      } finally {
        trace.push('cleanup-before-releaseLock');
        reader.releaseLock();
        trace.push('cleanup-after-releaseLock');
      }
    }

    return {
      async next() {
        const result = await reader.read();
        if (result.done) {
          await cleanup(false);
          return { done: true, value: void 0 };
        }

        trace.push(`next:${result.value}`);
        return { done: false, value: result.value };
      },

      async return() {
        trace.push('return-start');
        await cleanup(true);
        trace.push('return-end');
        return { done: true, value: void 0 };
      },
    };
  };

  return stream;
}

async function main() {
  const trace = [];
  const source = new ReadableStream({
    start(controller) {
      controller.enqueue('alpha');
      controller.enqueue('beta');
      controller.close();
    },
    cancel() {
      trace.push('source-cancel');
    },
  });

  const stream = createAsyncIterableStream(source, trace);

  assert(typeof stream[Symbol.asyncIterator] === 'function', 'stream should expose @@asyncIterator');

  for await (const chunk of stream) {
    trace.push(`chunk:${chunk}`);
    break;
  }

  trace.push('loop-exit');

   assertEq(
     trace.join(','),
     [
       'next:alpha',
       'chunk:alpha',
       'return-start',
       'cleanup-before-cancel',
       'cleanup-after-cancel',
       'cleanup-before-releaseLock',
       'cleanup-after-releaseLock',
       'return-end',
       'loop-exit',
    ].join(','),
    'for-await cleanup should finish after cancel/releaseLock'
  );

  console.log('async iterable stream cleanup survives cancel/releaseLock');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
