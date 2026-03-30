class ReactPromise {
  constructor(status, value, reason) {
    this.status = status;
    this.value = value;
    this.reason = reason;
  }
}

function resolveModelChunk(chunk, value) {
  if ("pending" !== chunk.status) {
    chunk.reason.enqueueModel(value);
    return;
  }

  throw new Error("unexpected pending chunk");
}

const seen = [];

function makeController() {
  return {
    enqueueValue(value) {
      seen.push(`value:${value}`);
    },
    enqueueModel(value) {
      seen.push(`model:${value}`);
    },
    close() {
      seen.push("close");
    },
    error(error) {
      seen.push(`error:${error && error.message ? error.message : String(error)}`);
    }
  };
}

const chunk = new ReactPromise("fulfilled", null, makeController());
resolveModelChunk(chunk, "ok");
console.log(seen.join(","));
