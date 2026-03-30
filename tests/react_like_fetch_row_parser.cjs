const http = require('node:http');

class ReactPromise {
  constructor(status, value, reason) {
    this.status = status;
    this.value = value;
    this.reason = reason;
  }
}

function createPendingChunk() {
  return new ReactPromise('pending', null, null);
}

function createResolvedModelChunk(response, value) {
  return new ReactPromise('resolved_model', value, response);
}

function resolveModelChunk(response, chunk, value) {
  if ('pending' !== chunk.status) {
    chunk.reason.enqueueModel(value);
    return;
  }
  chunk.status = 'resolved_model';
  chunk.value = value;
  chunk.reason = response;
}

function startAsyncIterable(response, id) {
  const seen = [];
  const controller = {
    enqueueModel(value) {
      seen.push(value);
    },
    close(value) {
      seen.push(`close:${value}`);
    },
    error(error) {
      seen.push(`error:${error && error.message ? error.message : String(error)}`);
    }
  };
  response.controllers.set(id, { seen, controller });
  response._chunks.set(id, new ReactPromise('fulfilled', {}, controller));
}

function processFullBinaryRow(response, id, tag, buffer, chunk) {
  let row = '';
  const decoder = new TextDecoder();
  for (let i = 0; i < buffer.length; i++) row += decoder.decode(buffer[i], { stream: true });
  row += decoder.decode(chunk);

  switch (tag) {
    case 120:
      startAsyncIterable(response, id);
      break;
    case 67: {
      const resolved = response._chunks.get(id);
      if (resolved && resolved.status === 'fulfilled') resolved.reason.close(row === '' ? '"$undefined"' : row);
      break;
    }
    default: {
      const existing = response._chunks.get(id);
      if (existing) {
        resolveModelChunk(response, existing, row);
      } else {
        response._chunks.set(id, createResolvedModelChunk(response, row));
      }
      break;
    }
  }
}

async function parseResponseBody(body) {
  const reader = body.getReader();
  const response = {
    _chunks: new Map(),
    controllers: new Map()
  };

  let rowState = 0;
  let rowID = 0;
  let rowTag = 0;
  let rowLength = 0;
  const buffer = [];

  while (true) {
    const { value, done } = await reader.read();
    if (done) break;

    let i = 0;
    while (i < value.length) {
      let lastIdx = -1;
      switch (rowState) {
        case 0: {
          const ch = value[i++];
          if (ch === 58) rowState = 1;
          else rowID = (rowID << 4) | (ch > 96 ? ch - 87 : ch - 48);
          continue;
        }
        case 1: {
          rowState = value[i];
          if (
            rowState === 84 || rowState === 65 || rowState === 79 || rowState === 111 ||
            rowState === 85 || rowState === 83 || rowState === 115 || rowState === 76 ||
            rowState === 108 || rowState === 71 || rowState === 103 || rowState === 77 ||
            rowState === 109 || rowState === 86
          ) {
            rowTag = rowState;
            rowState = 2;
            i++;
          } else if (
            (rowState > 64 && rowState < 91) || rowState === 35 || rowState === 114 || rowState === 120
          ) {
            rowTag = rowState;
            rowState = 3;
            i++;
          } else {
            rowTag = 0;
            rowState = 3;
          }
          continue;
        }
        case 2: {
          const ch = value[i++];
          if (ch === 44) rowState = 4;
          else rowLength = (rowLength << 4) | (ch > 96 ? ch - 87 : ch - 48);
          continue;
        }
        case 3:
          lastIdx = value.indexOf(10, i);
          break;
        case 4:
          lastIdx = i + rowLength;
          if (lastIdx > value.length) lastIdx = -1;
          break;
      }

      const offset = value.byteOffset + i;
      if (lastIdx > -1) {
        const rowChunk = new Uint8Array(value.buffer, offset, lastIdx - i);
        processFullBinaryRow(response, rowID, rowTag, buffer, rowChunk);
        i = lastIdx;
        if (rowState === 3) i++;
        rowLength = 0;
        rowID = 0;
        rowTag = 0;
        rowState = 0;
        buffer.length = 0;
      } else {
        const partial = new Uint8Array(value.buffer, offset, value.byteLength - i);
        buffer.push(partial);
        rowLength -= partial.byteLength;
        break;
      }
    }
  }

  return response;
}

const server = http.createServer((_req, res) => {
  res.writeHead(200, { 'content-type': 'application/octet-stream' });
  res.write('1:x\n1:"a');
  res.write('"\n1:"b"\n');
  res.end();
});

server.listen(0, async () => {
  const { port } = server.address();
  try {
    const res = await fetch(`http://127.0.0.1:${port}/`);
    const parsed = await parseResponseBody(res.body);
    const state = parsed.controllers.get(1);
    console.log(`controllerType:${typeof (state && state.controller)}`);
    console.log(`seen:${state ? state.seen.join(',') : 'missing'}`);
  } catch (error) {
    console.log(`ERR:${error && error.stack ? error.stack : String(error)}`);
  } finally {
    server.close();
  }
});
