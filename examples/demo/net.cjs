const net = require('node:net');
const httpParser = require('ant:internal/http_parser');
const httpWriter = require('ant:internal/http_writer');

const TEXT_CONTENT_TYPE = 'text/plain; charset=utf-8';
const JSON_CONTENT_TYPE = 'application/json; charset=utf-8';

function send(socket, status, statusText, contentType, body) {
  socket.write(httpWriter.writeBasicResponse(status, statusText, contentType, body, false));
  socket.end();
}

function sendBadRequest(socket, error) {
  const message = error && error.message ? error.message : 'Bad Request';
  send(socket, 400, 'Bad Request', TEXT_CONTENT_TYPE, `${message}\n`);
}

function sendStream(socket, request) {
  socket.write(httpWriter.writeHead(200, 'OK', ['Content-Type', TEXT_CONTENT_TYPE], true, 0, false));
  socket.write(httpWriter.writeChunk('streamed from ant:internal/http_writer\n'));
  socket.write(httpWriter.writeChunk(`method=${request.method} target=${request.target}\n`));
  socket.write(httpWriter.writeFinalChunk());
  socket.end();
}

const buildTextBody = request =>
  [
    'Hello from ant:internal/http_parser + ant:internal/http_writer!',
    `method=${request.method}`,
    `target=${request.target}`,
    `httpVersion=${request.httpVersion}`,
    `keepAlive=${request.keepAlive}`,
    `bodyLength=${request.bodyLength}`
  ].join('\n') + '\n';

const buildJsonBody = request =>
  JSON.stringify(
    {
      method: request.method,
      target: request.target,
      httpVersion: request.httpVersion,
      keepAlive: request.keepAlive,
      bodyLength: request.bodyLength,
      rawHeaders: request.rawHeaders
    },
    null,
    2
  );

function handleRequest(socket, request) {
  if (request.target === '/stream') {
    sendStream(socket, request);
    return;
  }

  if (request.target === '/json') {
    send(socket, 200, 'OK', JSON_CONTENT_TYPE, buildJsonBody(request));
    return;
  }

  send(socket, 200, 'OK', TEXT_CONTENT_TYPE, buildTextBody(request));
}

function appendChunk(buffered, chunk) {
  return buffered.length === 0 ? chunk : Buffer.concat([buffered, chunk]);
}

function onSocketData(socket, state, chunk) {
  state.buffer = appendChunk(state.buffer, chunk);

  try {
    const request = httpParser.parseRequest(state.buffer);
    if (!request) return;

    handleRequest(socket, request);

    if (request.consumed > 0) {
      state.buffer = state.buffer.subarray(request.consumed);
    }
  } catch (error) {
    sendBadRequest(socket, error);
  }
}

const server = net.createServer(socket => {
  const state = { buffer: Buffer.alloc(0) };
  console.log('client connected');

  socket.on('data', chunk => onSocketData(socket, state, chunk));
  socket.on('end', () => console.log('client disconnected'));
  socket.on('error', error => console.error(`socket error: ${error.message}`));
});

server.listen(3000, () => {
  console.log('server on http://localhost:3000');
});
