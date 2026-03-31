function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function makeAbortError() {
  try {
    return new DOMException('aborted', 'AbortError');
  } catch {
    const error = new Error('aborted');
    error.name = 'AbortError';
    return error;
  }
}

function abortRequest(request, reason) {
  if (request.status === 10 || request.status === 11) request.status = 12;
  request.fatalError = reason;
  if (request.destination) {
    try {
      request.destination.error?.(reason);
    } catch {}
    request.destination = null;
  }
}

async function runIteration(index) {
  const controller = new AbortController();
  const signal = controller.signal;
  const request = {
    status: 10,
    destination: null,
    fatalError: null,
    seen: 0,
  };

  setTimeout(() => {
    if (request.status === 10) request.status = 11;
  }, 0);

  const listener = () => {
    abortRequest(request, signal.reason ?? makeAbortError());
    signal.removeEventListener('abort', listener);
  };

  signal.addEventListener('abort', listener);

  const stream = new ReadableStream({
    pull(controllerObj) {
      request.seen++;

      if (request.status === 13) {
        request.status = 14;
        controllerObj.error(request.fatalError);
        return;
      }

      if (request.status !== 14 && request.destination === null) {
        request.destination = controllerObj;
        controllerObj.enqueue(new Uint8Array([index & 255]));
        controllerObj.close();
        request.destination = null;
      }
    },
    cancel(reason) {
      request.destination = null;
      abortRequest(request, reason);
    },
  }, { highWaterMark: 0 });

  queueMicrotask(() => controller.abort(makeAbortError()));

  const reader = stream.getReader();
  try {
    await reader.read();
  } catch {}

  assert(request.seen >= 1, `pull was not invoked for iteration ${index}`);
}

for (let i = 0; i < 2000; i++) {
  await runIteration(i);
  if ((i + 1) % 100 === 0) console.log(`iter:${i + 1}`);
}

console.log('ok');
