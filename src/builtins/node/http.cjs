const net = require('node:net');
const httpMetadata = require('ant:internal/http_metadata');
const httpParser = require('ant:internal/http_parser');
const httpWriter = require('ant:internal/http_writer');

const METHODS = httpMetadata.METHODS;
const STATUS_CODES = httpMetadata.STATUS_CODES;

function createHeadersObject() {
  return Object.create(null);
}

function normalizeHeaderName(name) {
  return String(name).toLowerCase();
}

function appendHeaderValue(headers, key, value) {
  if (key === 'set-cookie') {
    if (!headers[key]) headers[key] = [];
    headers[key].push(value);
    return;
  }

  if (headers[key] === undefined) {
    headers[key] = value;
    return;
  }

  if (key === 'cookie') headers[key] += '; ' + value;
  else headers[key] += ', ' + value;
}

function buildHeaders(rawHeaders) {
  const headers = createHeadersObject();
  let i = 0;

  for (i = 0; i + 1 < rawHeaders.length; i += 2) {
    appendHeaderValue(headers, normalizeHeaderName(rawHeaders[i]), String(rawHeaders[i + 1]));
  }

  return headers;
}

function bufferFrom(value, encoding) {
  if (value === undefined || value === null) return Buffer.alloc(0);
  if (Buffer.isBuffer(value)) return value;
  if (value && typeof value === 'object' && typeof value.byteLength === 'number') return Buffer.from(value);
  if (typeof value === 'string') return Buffer.from(value, encoding || 'utf8');
  return Buffer.from(String(value), encoding || 'utf8');
}

function appendRawHeader(rawHeaders, name, value) {
  if (Array.isArray(value)) {
    value.forEach(item => rawHeaders.push(name, String(item)));
    return;
  }

  rawHeaders.push(name, String(value));
}

function hasResponseBody(method, statusCode) {
  if (method === 'HEAD') return false;
  if (statusCode === 204 || statusCode === 304) return false;
  return true;
}

function applyHeaderObject(target, headers) {
  if (!headers) return;

  if (Array.isArray(headers)) {
    for (let i = 0; i + 1 < headers.length; i += 2) {
      target.setHeader(headers[i], headers[i + 1]);
    }
    return;
  }

  Object.keys(headers).forEach(name => {
    target.setHeader(name, headers[name]);
  });
}

function makeSocketState(server, socket) {
  return {
    server,
    socket,
    buffered: Buffer.alloc(0),
    activeResponse: null,
    closed: false
  };
}

function appendSocketChunk(buffered, chunk) {
  const next = bufferFrom(chunk);
  if (buffered.length === 0) return next;
  return Buffer.concat([buffered, next]);
}

function handleClientError(server, socket, error) {
  if (server.listenerCount('clientError') > 0) {
    server.emit('clientError', error, socket);
    return;
  }

  if (socket && !socket.destroyed) {
    socket.end(httpWriter.writeBasicResponse(400, 'Bad Request', 'text/plain;charset=UTF-8', 'Bad Request', false));
  }
}

class IncomingMessage extends EventEmitter {
  constructor(socket, parsed) {
    super();
    this.socket = socket;
    this.connection = socket;
    this.method = parsed.method;
    this.url = parsed.target;
    this.headers = buildHeaders(parsed.rawHeaders || []);
    this.rawHeaders = (parsed.rawHeaders || []).slice();
    this.httpVersion = parsed.httpVersion;
    this.httpVersionMajor = parsed.httpVersionMajor;
    this.httpVersionMinor = parsed.httpVersionMinor;
    this._keepAlive = !!parsed.keepAlive;
    this.complete = false;
    this.aborted = false;
    this.destroyed = false;
    this.readableEnded = false;
    this._body = bufferFrom(parsed.body);
  }

  _deliverBody() {
    if (this.destroyed || this.complete) return;

    if (this._body.length > 0) this.emit('data', this._body);
    this.complete = true;
    this.readableEnded = true;
    this.emit('end');
    this.emit('close');
  }

  destroy(error) {
    if (this.destroyed) return this;
    this.destroyed = true;
    if (error) this.emit('error', error);
    if (this.socket && !this.socket.destroyed) this.socket.destroy(error);
    return this;
  }

  setTimeout(msecs, callback) {
    if (this.socket && this.socket.setTimeout) this.socket.setTimeout(msecs, callback);
    return this;
  }
}

class ServerResponse extends EventEmitter {
  constructor(req, socket, socketState) {
    super();
    this.req = req;
    this.socket = socket;
    this.connection = socket;
    this.statusCode = 200;
    this.statusMessage = undefined;
    this.sendDate = true;
    this.strictContentLength = false;
    this.headersSent = false;
    this.writableEnded = false;
    this.writableFinished = false;
    this.finished = false;
    this._headers = createHeadersObject();
    this._headerNames = createHeadersObject();
    this._socketState = socketState;
    this._streaming = false;
  }

  setHeader(name, value) {
    const headerName = String(name);
    const key = normalizeHeaderName(headerName);
    this._headers[key] = value;
    this._headerNames[key] = headerName;
    return this;
  }

  getHeader(name) {
    return this._headers[normalizeHeaderName(name)];
  }

  getHeaders() {
    const copy = createHeadersObject();
    Object.keys(this._headers).forEach(key => {
      copy[key] = this._headers[key];
    });
    return copy;
  }

  getHeaderNames() {
    return Object.keys(this._headers);
  }

  hasHeader(name) {
    return this._headers[normalizeHeaderName(name)] !== undefined;
  }

  removeHeader(name) {
    const key = normalizeHeaderName(name);
    delete this._headers[key];
    delete this._headerNames[key];
    return this;
  }

  writeHead(statusCode, statusMessage, headers) {
    this.statusCode = statusCode | 0;

    if (typeof statusMessage === 'string') {
      this.statusMessage = statusMessage;
      applyHeaderObject(this, headers);
    } else {
      applyHeaderObject(this, statusMessage);
    }

    return this;
  }

  _rawHeaders() {
    const rawHeaders = [];

    if (this.sendDate && !this.hasHeader('date')) {
      this.setHeader('Date', new Date().toUTCString());
    }

    Object.keys(this._headers).forEach(key => {
      const name = this._headerNames[key] || key;
      appendRawHeader(rawHeaders, name, this._headers[key]);
    });

    return rawHeaders;
  }

  _shouldKeepAlive() {
    const connection = this.getHeader('connection');
    if (typeof connection === 'string') {
      if (connection.toLowerCase() === 'close') return false;
      if (connection.toLowerCase() === 'keep-alive') return true;
    }

    return !!(this.req && this.req._keepAlive);
  }

  _writeHead(bodyIsStream, bodySize) {
    if (this.headersSent) return;

    const statusText = this.statusMessage || STATUS_CODES[this.statusCode] || httpWriter.defaultStatusText(this.statusCode);
    const head = httpWriter.writeHead(this.statusCode, statusText, this._rawHeaders(), bodyIsStream, bodySize, this._shouldKeepAlive());

    this.headersSent = true;
    this.socket.write(head);
  }

  write(chunk, encoding, callback) {
    const body = hasResponseBody(this.req && this.req.method, this.statusCode)
      ? bufferFrom(chunk, typeof encoding === 'string' ? encoding : undefined)
      : Buffer.alloc(0);

    if (typeof encoding === 'function') callback = encoding;
    if (!this.headersSent) this._writeHead(true, 0);
    this._streaming = true;

    if (body.length > 0) this.socket.write(httpWriter.writeChunk(body));
    if (typeof callback === 'function') callback();
    return true;
  }

  end(chunk, encoding, callback) {
    let body = Buffer.alloc(0);
    let keepAlive = false;

    if (this.writableEnded) return this;
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }

    if (hasResponseBody(this.req && this.req.method, this.statusCode)) {
      body = bufferFrom(chunk, typeof encoding === 'string' ? encoding : undefined);
    }

    if (this._streaming) {
      if (!this.headersSent) this._writeHead(true, 0);
      if (body.length > 0) this.socket.write(httpWriter.writeChunk(body));
      this.socket.write(httpWriter.writeFinalChunk());
    } else {
      this._writeHead(false, body.length);
      if (body.length > 0) this.socket.write(body);
    }

    this.writableEnded = true;
    this.writableFinished = true;
    this.finished = true;
    this.emit('finish');
    this.emit('close');

    keepAlive = this._shouldKeepAlive();
    this._socketState.activeResponse = null;
    if (typeof callback === 'function') callback();

    if (!keepAlive) this.socket.end();
    else if (this._socketState.buffered.length > 0) this._socketState.server._drainSocket(this.socket, this._socketState);
    return this;
  }

  setTimeout(msecs, callback) {
    if (this.socket && this.socket.setTimeout) this.socket.setTimeout(msecs, callback);
    return this;
  }
}

class Server extends net.Server {
  constructor(options, requestListener) {
    const serverOptions = typeof options === 'function' ? undefined : options;
    const onRequest = typeof options === 'function' ? options : requestListener;

    super(serverOptions);
    this.timeout = 0;
    this.headersTimeout = 60000;
    this.requestTimeout = 300000;
    this.keepAliveTimeout = 5000;
    this.maxHeadersCount = 2000;

    this.on('connection', socket => {
      this._attachSocket(socket);
    });

    if (typeof onRequest === 'function') this.on('request', onRequest);
  }

  _attachSocket(socket) {
    const state = makeSocketState(this, socket);

    if (this.timeout > 0 && socket.setTimeout) {
      socket.setTimeout(this.timeout, () => {
        this.emit('timeout', socket);
      });
    }

    socket.on('data', chunk => {
      state.buffered = appendSocketChunk(state.buffered, chunk);
      this._drainSocket(socket, state);
    });

    socket.on('error', error => {
      handleClientError(this, socket, error);
    });

    socket.on('close', () => {
      state.closed = true;
    });
  }

  _drainSocket(socket, state) {
    while (!state.closed) {
      let parsed = null;

      if (state.activeResponse && !state.activeResponse.writableEnded) return;
      if (state.buffered.length === 0) return;

      try {
        parsed = httpParser.parseRequest(state.buffered);
      } catch (error) {
        state.closed = true;
        handleClientError(this, socket, error);
        return;
      }

      if (parsed === null) return;
      if (!parsed.consumed || parsed.consumed < 0) {
        state.closed = true;
        handleClientError(this, socket, new Error('Invalid HTTP parser state'));
        return;
      }

      state.buffered = parsed.consumed < state.buffered.length ? state.buffered.subarray(parsed.consumed) : Buffer.alloc(0);

      const req = new IncomingMessage(socket, parsed);
      const res = new ServerResponse(req, socket, state);
      state.activeResponse = res;

      this.emit('request', req, res);
      req._deliverBody();

      if (!res.writableEnded) return;
    }
  }

  setTimeout(msecs, callback) {
    this.timeout = msecs | 0;
    if (typeof callback === 'function') this.on('timeout', callback);
    return this;
  }
}

function createServer(options, requestListener) {
  return new Server(options, requestListener);
}

module.exports = {
  METHODS,
  STATUS_CODES,
  IncomingMessage,
  Server,
  ServerResponse,
  createServer
};
