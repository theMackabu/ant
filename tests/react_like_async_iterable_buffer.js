class ReactPromise {
  constructor(status, value, reason) {
    this.status = status;
    this.value = value;
    this.reason = reason;
  }
}

function createPendingChunk(response) {
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
    return chunk.reason.enqueueModel(value);
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

function startAsyncIterable(response) {
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

  return {
    buffer,
    iterator: iterable[Symbol.asyncIterator](),
    enqueueModel(value) {
      if (nextWriteIndex === buffer.length) {
        buffer[nextWriteIndex] = createResolvedIteratorResultChunk(response, value, false);
      } else {
        resolveIteratorResultChunk(response, buffer[nextWriteIndex], value, false);
      }
      nextWriteIndex++;
    }
  };
}

const response = {
  seen: [],
  enqueueModel(value) {
    this.seen.push(value);
  }
};

const state = startAsyncIterable(response);

console.log(`len0:${state.buffer.length}`);
const first = state.iterator.next();
console.log(`len1:${state.buffer.length}`);
console.log(`first.status:${first.status}`);

state.enqueueModel("1");
console.log(`len2:${state.buffer.length}`);
console.log(`slot0.status:${state.buffer[0].status}`);
console.log(`slot0.reasonType:${typeof state.buffer[0].reason}`);

state.enqueueModel("2");
console.log(`len3:${state.buffer.length}`);
console.log(`seen:${response.seen.join(",")}`);
