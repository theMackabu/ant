class ReactPromise {
  constructor(status, value, reason) {
    this.status = status;
    this.value = value;
    this.reason = reason;
  }
}

function createPendingChunk() {
  return new ReactPromise("pending", null, null);
}

function createResolvedIteratorResultChunk(response, value, done) {
  return new ReactPromise(
    "resolved_model",
    (done ? "{\"done\":true,\"value\":" : "{\"done\":false,\"value\":") + value + "}",
    response
  );
}

function resolveModelChunk(response, chunk, value) {
  if ("pending" !== chunk.status) {
    chunk.reason.enqueueModel(value);
    return;
  }

  chunk.status = "resolved_model";
  chunk.value = value;
  chunk.reason = response;
}

function resolveIteratorResultChunk(response, chunk, value, done) {
  resolveModelChunk(
    response,
    chunk,
    (done ? "{\"done\":true,\"value\":" : "{\"done\":false,\"value\":") + value + "}"
  );
}

function startAsyncIterable(response, id, iterator) {
  const buffer = [];
  let closed = false;
  let nextWriteIndex = 0;
  const iterable = {};

  iterable[Symbol.asyncIterator] = function () {
    let nextReadIndex = 0;
    return {
      next(arg) {
        if (arg !== undefined) throw new Error("bad arg");
        if (nextReadIndex === buffer.length) {
          if (closed) {
            return new ReactPromise("fulfilled", { done: true, value: undefined }, null);
          }
          buffer[nextReadIndex] = createPendingChunk(response);
        }
        return buffer[nextReadIndex++];
      }
    };
  };

  const controller = {
    enqueueValue(value) {
      if (nextWriteIndex === buffer.length) {
        buffer[nextWriteIndex] = new ReactPromise("fulfilled", { done: false, value }, null);
      } else {
        const chunk = buffer[nextWriteIndex];
        chunk.status = "fulfilled";
        chunk.value = { done: false, value };
        chunk.reason = null;
      }
      nextWriteIndex++;
    },
    enqueueModel(value) {
      if (nextWriteIndex === buffer.length) {
        buffer[nextWriteIndex] = createResolvedIteratorResultChunk(response, value, false);
      } else {
        resolveIteratorResultChunk(response, buffer[nextWriteIndex], value, false);
      }
      nextWriteIndex++;
    },
    close(value) {
      if (!closed) {
        closed = true;
        if (nextWriteIndex === buffer.length) {
          buffer[nextWriteIndex] = createResolvedIteratorResultChunk(response, value, true);
        } else {
          resolveIteratorResultChunk(response, buffer[nextWriteIndex], value, true);
        }
        nextWriteIndex++;
      }
    },
    error(error) {
      throw error;
    }
  };

  response._chunks.set(id, new ReactPromise("fulfilled", iterator ? iterable[Symbol.asyncIterator]() : iterable, controller));
  return { buffer, controller };
}

function log(label, fn) {
  try {
    const out = fn();
    console.log(`${label}:${out}`);
  } catch (error) {
    console.log(`${label}:ERR:${error && error.message ? error.message : String(error)}`);
  }
}

const response = { _chunks: new Map() };
const id = 1;

startAsyncIterable(response, id, true);
const chunk = response._chunks.get(id);

log("reasonType", () => typeof chunk.reason);
log("enqueueModelType", () => typeof chunk.reason.enqueueModel);

resolveModelChunk(response, chunk, "\"1\"");
console.log(`bufferLength:${chunk.reason ? "controller" : "missing"}`);

const response2 = { _chunks: new Map() };
const id2 = 2;
response2._chunks.set(id2, new ReactPromise("resolved_model", "\"boot\"", response2));
const badChunk = response2._chunks.get(id2);
log("badReasonType", () => typeof badChunk.reason);
log("badEnqueueModelType", () => typeof badChunk.reason.enqueueModel);
log("badResolve", () => {
  resolveModelChunk(response2, badChunk, "\"next\"");
  return "ok";
});
