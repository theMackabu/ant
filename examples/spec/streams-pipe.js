import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('ReadableStream pipe operations (tee, pipeTo, pipeThrough) Tests\n');

async function testTeeBasic() {
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('a');
      c.enqueue('b');
      c.close();
    }
  });
  const [b1, b2] = rs.tee();
  test('tee returns array', Array.isArray([b1, b2]), true);
  test('tee branch1 is ReadableStream', b1 instanceof ReadableStream, true);
  test('tee branch2 is ReadableStream', b2 instanceof ReadableStream, true);
  test('tee locks original', rs.locked, true);

  const r1 = b1.getReader();
  const r2 = b2.getReader();

  const v1a = await r1.read();
  test('tee b1 read1 value', v1a.value, 'a');
  test('tee b1 read1 done', v1a.done, false);
  const v1b = await r1.read();
  test('tee b1 read2 value', v1b.value, 'b');
  const v1c = await r1.read();
  test('tee b1 done', v1c.done, true);

  const v2a = await r2.read();
  test('tee b2 read1 value', v2a.value, 'a');
  const v2b = await r2.read();
  test('tee b2 read2 value', v2b.value, 'b');
  const v2c = await r2.read();
  test('tee b2 done', v2c.done, true);
}

async function testTeePullBased() {
  let pullCount = 0;
  const rs = new ReadableStream({
    pull(c) {
      pullCount++;
      if (pullCount <= 3) c.enqueue(pullCount);
      else c.close();
    }
  });
  const [b1, b2] = rs.tee();
  const r1 = b1.getReader();
  const r2 = b2.getReader();

  const chunks1 = [];
  const chunks2 = [];
  while (true) {
    const { value, done } = await r1.read();
    if (done) break;
    chunks1.push(value);
  }
  while (true) {
    const { value, done } = await r2.read();
    if (done) break;
    chunks2.push(value);
  }
  testDeep('tee pull b1 chunks', chunks1, [1, 2, 3]);
  testDeep('tee pull b2 chunks', chunks2, [1, 2, 3]);
}

async function testTeeCancelOne() {
  let cancelReason = null;
  const rs = new ReadableStream({
    pull(c) {
      c.enqueue('x');
    },
    cancel(r) {
      cancelReason = r;
    }
  });
  const [b1, b2] = rs.tee();
  const cancelPromise = b1.cancel('reason1');
  test('tee cancel one does not cancel original', cancelReason, null);

  const r2 = b2.getReader();
  const v = await r2.read();
  test('tee other branch still works', v.value, 'x');

  // The first branch's cancel does not settle until the other branch finishes.
  await r2.cancel('reason2');
  await cancelPromise;
}

async function testTeeCancelBoth() {
  let cancelReason = null;
  const rs = new ReadableStream({
    pull(c) {
      c.enqueue('x');
    },
    cancel(r) {
      cancelReason = r;
    }
  });
  const [b1, b2] = rs.tee();
  const p1 = b1.cancel('r1');
  const p2 = b2.cancel('r2');
  await Promise.all([p1, p2]);
  test('tee cancel both cancels original', Array.isArray(cancelReason), true);
  testDeep('tee composite reason', cancelReason, ['r1', 'r2']);
}

testThrows('tee locked stream throws', () => {
  const rs = new ReadableStream();
  rs.getReader();
  rs.tee();
});

async function testPipeToBasic() {
  const chunks = [];
  const rs = new ReadableStream({
    start(c) {
      c.enqueue(1);
      c.enqueue(2);
      c.enqueue(3);
      c.close();
    }
  });
  const ws = new WritableStream({
    write(chunk) {
      chunks.push(chunk);
    }
  });
  await rs.pipeTo(ws);
  testDeep('pipeTo basic chunks', chunks, [1, 2, 3]);
}

async function testPipeToClosesDestination() {
  let closed = false;
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('a');
      c.close();
    }
  });
  const ws = new WritableStream({
    write() {},
    close() {
      closed = true;
    }
  });
  await rs.pipeTo(ws);
  test('pipeTo closes dest', closed, true);
}

async function testPipeToPreventClose() {
  let closed = false;
  const rs = new ReadableStream({
    start(c) {
      c.close();
    }
  });
  const ws = new WritableStream({
    close() {
      closed = true;
    }
  });
  await rs.pipeTo(ws, { preventClose: true });
  test('pipeTo preventClose', closed, false);
}

async function testPipeToErrorPropagation() {
  const err = new Error('source error');
  const rs = new ReadableStream({
    start(c) {
      c.error(err);
    }
  });
  let abortReason = null;
  const ws = new WritableStream({
    write() {},
    abort(r) {
      abortReason = r;
    }
  });
  try {
    await rs.pipeTo(ws);
    test('pipeTo should reject on source error', false, true);
  } catch (e) {
    test('pipeTo rejects with source error', e, err);
  }
}

async function testPipeToPreventAbort() {
  const err = new Error('source error');
  const rs = new ReadableStream({
    start(c) {
      c.error(err);
    }
  });
  let aborted = false;
  const ws = new WritableStream({
    write() {},
    abort() {
      aborted = true;
    }
  });
  try {
    await rs.pipeTo(ws, { preventAbort: true });
  } catch (e) {}
  test('pipeTo preventAbort', aborted, false);
}

async function testPipeToLocksStreams() {
  const rs = new ReadableStream({
    start(c) {
      c.close();
    }
  });
  const ws = new WritableStream();
  const p = rs.pipeTo(ws);
  test('pipeTo locks source', rs.locked, true);
  test('pipeTo locks dest', ws.locked, true);
  await p;
  test('pipeTo unlocks source', rs.locked, false);
  test('pipeTo unlocks dest', ws.locked, false);
}

async function testPipeToWithSignal() {
  const ac = new AbortController();
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('a');
      c.enqueue('b');
      c.close();
    }
  });
  const chunks = [];
  const ws = new WritableStream({
    write(chunk) {
      chunks.push(chunk);
      ac.abort();
    }
  });

  try {
    await rs.pipeTo(ws, { signal: ac.signal });
    test('pipeTo with signal should reject', false, true);
  } catch (e) {
    test('pipeTo aborted by signal', true, true);
    test('pipeTo signal rejects with abort semantics',
      !(e instanceof TypeError) && !(e && /AbortSignal/.test(String(e && e.message))),
      true);
  }
  testDeep('pipeTo signal stops further writes', chunks, ['a']);
}

async function testPipeToAlreadyAbortedSignal() {
  const ac = new AbortController();
  ac.abort('pre-aborted');
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('x');
      c.close();
    }
  });
  const ws = new WritableStream();
  try {
    await rs.pipeTo(ws, { signal: ac.signal });
    test('pipeTo pre-aborted should reject', false, true);
  } catch (e) {
    test('pipeTo pre-aborted signal rejects', true, true);
  }
}

async function testPipeToRejectsLocked() {
  const rs = new ReadableStream();
  rs.getReader();
  const ws = new WritableStream();
  try {
    await rs.pipeTo(ws);
    test('pipeTo locked source should reject', false, true);
  } catch (e) {
    test('pipeTo locked source rejects', true, true);
  }
}

async function testPipeThroughBasic() {
  const rs = new ReadableStream({
    start(c) {
      c.enqueue(1);
      c.enqueue(2);
      c.enqueue(3);
      c.close();
    }
  });

  let transformController;
  const transform = {
    writable: new WritableStream({
      write(chunk) {
        transformController.enqueue(chunk * 10);
      },
      close() {
        transformController.close();
      }
    }),
    readable: new ReadableStream({
      start(c) {
        transformController = c;
      }
    })
  };

  const result = rs.pipeThrough(transform);
  test('pipeThrough returns readable', result instanceof ReadableStream, true);

  const reader = result.getReader();
  const chunks = [];
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  testDeep('pipeThrough transforms data', chunks, [10, 20, 30]);
}

testThrows('pipeThrough locked source throws', () => {
  const rs = new ReadableStream();
  rs.getReader();
  rs.pipeThrough({ writable: new WritableStream(), readable: new ReadableStream() });
});

testThrows('pipeThrough missing transform throws', () => {
  new ReadableStream().pipeThrough();
});

testThrows('pipeThrough locked writable throws', () => {
  const ws = new WritableStream();
  ws.getWriter();
  new ReadableStream().pipeThrough({ writable: ws, readable: new ReadableStream() });
});

await testTeeBasic();
await testTeePullBased();
await testTeeCancelOne();
await testTeeCancelBoth();

await testPipeToBasic();
await testPipeToClosesDestination();
await testPipeToPreventClose();
await testPipeToErrorPropagation();
await testPipeToPreventAbort();
await testPipeToLocksStreams();
await testPipeToWithSignal();
await testPipeToAlreadyAbortedSignal();
await testPipeToRejectsLocked();

await testPipeThroughBasic();

summary();
