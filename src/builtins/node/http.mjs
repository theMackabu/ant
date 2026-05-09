import net from 'node:net';
import tls from 'node:tls';

import httpParser from 'ant:internal/http_parser';
import httpWriter from 'ant:internal/http_writer';

import { EventEmitter } from 'node:events';
import { STATUS_CODES } from 'ant:internal/http_metadata';

export const maxHeaderSize = 16 * 1024;

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

function closeSocket(socket) {
  if (!socket || socket.destroyed) return;
  if (typeof socket.destroy === 'function') socket.destroy();
  else if (typeof socket.end === 'function') socket.end();
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

function clientNotImplemented() {
  throw new Error('node:http client transport is not implemented yet');
}

function isURLLike(value) {
  return !!value && typeof value === 'object' && typeof value.href === 'string';
}

function cloneOptionObject(value) {
  return value && typeof value === 'object' ? { ...value } : {};
}

function normalizeRequestArgs(input, options, callback) {
  let resolvedInput = input;
  let resolvedOptions = options;
  let resolvedCallback = callback;

  if (typeof resolvedOptions === 'function') {
    resolvedCallback = resolvedOptions;
    resolvedOptions = undefined;
  }

  if (typeof resolvedInput === 'function' || resolvedInput === undefined || resolvedInput === null) {
    resolvedCallback = typeof resolvedInput === 'function' ? resolvedInput : resolvedCallback;
    resolvedInput = undefined;
  }

  return {
    input: resolvedInput,
    options: cloneOptionObject(resolvedOptions),
    callback: resolvedCallback
  };
}

function defaultPortForProtocol(protocol) {
  return protocol === 'https:' ? 443 : 80;
}

function hostIncludesExplicitPort(host) {
  if (host === undefined || host === null) return false;
  const value = String(host);
  const bracketEnd = value.lastIndexOf(']');
  const colonIndex = value.lastIndexOf(':');
  return colonIndex > bracketEnd;
}

function buildRequestOptions(input, options) {
  const requestOptions = cloneOptionObject(options);

  if (typeof input === 'string' || isURLLike(input)) {
    const url = new URL(String(input));
    if (requestOptions.protocol === undefined) requestOptions.protocol = url.protocol;
    if (requestOptions.hostname === undefined && requestOptions.host === undefined) {
      requestOptions.hostname = url.hostname;
    }
    if (requestOptions.port === undefined && url.port) requestOptions.port = url.port;
    if (requestOptions.path === undefined) requestOptions.path = `${url.pathname}${url.search}`;
    if (requestOptions.auth === undefined && url.username) {
      requestOptions.auth = url.password ? `${url.username}:${url.password}` : url.username;
    }
  } else if (input && typeof input === 'object') {
    Object.assign(requestOptions, input);
  }

  if (!requestOptions.protocol) requestOptions.protocol = 'http:';
  if (!requestOptions.method) requestOptions.method = 'GET';
  if (!requestOptions.path) requestOptions.path = '/';
  if (
    (requestOptions.port === undefined || requestOptions.port === null || requestOptions.port === '') &&
    !hostIncludesExplicitPort(requestOptions.host)
  ) {
    requestOptions.port = defaultPortForProtocol(requestOptions.protocol);
  }

  return requestOptions;
}

function buildRequestUrl(options) {
  const protocol = options.protocol || 'http:';
  const base = new URL(`${protocol}//localhost/`);

  if (options.hostname !== undefined && options.hostname !== null) base.hostname = String(options.hostname);
  else if (options.host !== undefined && options.host !== null) base.host = String(options.host);

  if (options.port !== undefined && options.port !== null && options.port !== '') {
    base.port = String(options.port);
  }

  if (options.auth) {
    const auth = String(options.auth);
    const sep = auth.indexOf(':');
    if (sep === -1) base.username = auth;
    else {
      base.username = auth.slice(0, sep);
      base.password = auth.slice(sep + 1);
    }
  }

  return new URL(String(options.path || '/'), base).toString();
}

function createAbortError(message) {
  const error = new Error(message || 'The operation was aborted');
  error.name = 'AbortError';
  return error;
}

function buildRequestHeadersObject(headers) {
  const normalized = createHeadersObject();

  if (!headers) return normalized;
  Object.keys(headers).forEach(name => {
    const value = headers[name];
    normalized[name] = Array.isArray(value) ? value.join(', ') : String(value);
  });
  return normalized;
}

function buildHeadersFromFetch(response) {
  const headers = createHeadersObject();
  const rawHeaders = [];

  for (const [name, value] of response.headers.entries()) {
    rawHeaders.push(name, value);
    appendHeaderValue(headers, normalizeHeaderName(name), value);
  }

  return { headers, rawHeaders };
}

function getFetchBody(chunks) {
  if (!chunks || chunks.length === 0) return undefined;
  if (chunks.length === 1) return chunks[0];
  return Buffer.concat(chunks);
}

function hasUpgradeHeader(headers) {
  const value = headers && headers.upgrade;
  return value !== undefined && value !== null;
}

function formatHostHeader(options) {
  const host = options.host ?? options.hostname ?? 'localhost';
  const port = options.port;
  const protocol = options.protocol || 'http:';
  const defaultPort = defaultPortForProtocol(protocol);

  if (hostIncludesExplicitPort(host) || port === undefined || port === null || port === '' || Number(port) === defaultPort) {
    return String(host);
  }

  return `${host}:${port}`;
}

function buildRawRequestHeaders(options, headers) {
  const rawHeaders = [];
  const seen = Object.create(null);

  Object.keys(headers).forEach(name => {
    seen[normalizeHeaderName(name)] = true;
    appendRawHeader(rawHeaders, name, headers[name]);
  });

  if (!seen.host) appendRawHeader(rawHeaders, 'Host', formatHostHeader(options));

  return rawHeaders;
}

function parseHttpResponseHead(head) {
  const lines = String(head).split('\r\n');
  const statusLine = lines.shift() || '';
  const match = /^HTTP\/([0-9]+)\.([0-9]+)\s+(\d{3})(?:\s+(.*))?$/.exec(statusLine);
  if (!match) throw new Error('Invalid HTTP response');

  const rawHeaders = [];
  for (const line of lines) {
    if (!line) continue;
    const sep = line.indexOf(':');
    if (sep === -1) continue;
    rawHeaders.push(line.slice(0, sep), line.slice(sep + 1).trimStart());
  }

  return {
    statusCode: Number(match[3]),
    statusMessage: match[4] || STATUS_CODES[Number(match[3])] || '',
    httpVersion: `${match[1]}.${match[2]}`,
    httpVersionMajor: Number(match[1]),
    httpVersionMinor: Number(match[2]),
    rawHeaders,
    headers: buildHeaders(rawHeaders)
  };
}

// compatibility stub only
function createAgentState() {
  return Object.create(null);
}

export class OutgoingMessage extends EventEmitter {}

class FetchIncomingMessage extends EventEmitter {
  constructor(response) {
    super();

    const { headers, rawHeaders } = buildHeadersFromFetch(response);

    this.socket = null;
    this.connection = null;
    this.statusCode = response.status;
    this.statusMessage = response.statusText || STATUS_CODES[response.status] || '';
    this.headers = headers;
    this.rawHeaders = rawHeaders;
    this.httpVersion = '1.1';
    this.httpVersionMajor = 1;
    this.httpVersionMinor = 1;
    this.complete = false;
    this.aborted = false;
    this.destroyed = false;
    this.readableEnded = false;
    this.url = response.url;
    this._reader = response.body && typeof response.body.getReader === 'function' ? response.body.getReader() : null;
    this._encoding = null;
    this._decoder = null;
    this._pumpStarted = false;
    this._closeEmitted = false;
  }

  _emitClose() {
    if (this._closeEmitted) return;
    this._closeEmitted = true;
    this.emit('close');
  }

  async _pumpBody() {
    if (this._pumpStarted) return;
    this._pumpStarted = true;

    if (!this._reader) {
      this.complete = true;
      this.readableEnded = true;
      this.emit('end');
      this._emitClose();
      return;
    }

    try {
      for (;;) {
        const { done, value } = await this._reader.read();
        if (done) break;
        if (!value || value.byteLength === 0) continue;

        const chunk = Buffer.from(value);
        if (this._decoder) {
          const text = this._decoder.decode(chunk, { stream: true });
          if (text.length > 0) this.emit('data', text);
        } else if (this._encoding) {
          this.emit('data', chunk.toString(this._encoding));
        } else {
          this.emit('data', chunk);
        }
      }

      if (this._decoder) {
        const finalChunk = this._decoder.decode();
        if (finalChunk.length > 0) this.emit('data', finalChunk);
      }

      this.complete = true;
      this.readableEnded = true;
      this.emit('end');
      this._emitClose();
    } catch (error) {
      if (this.destroyed) return;
      this.destroyed = true;
      this.aborted = true;
      this.emit('error', error);
      this._emitClose();
    }
  }

  setEncoding(encoding) {
    this._encoding = encoding || 'utf8';
    if (typeof TextDecoder === 'function') {
      try {
        this._decoder = new TextDecoder(this._encoding === 'utf8' ? 'utf-8' : this._encoding);
      } catch {
        this._decoder = null;
      }
    }
    return this;
  }

  resume() {
    queueMicrotask(() => {
      this._pumpBody();
    });
    return this;
  }

  destroy(error) {
    if (this.destroyed) return this;
    this.destroyed = true;
    this.aborted = true;
    if (this._reader && typeof this._reader.cancel === 'function') {
      Promise.resolve(this._reader.cancel(error)).catch(() => {});
    }
    if (error) this.emit('error', error);
    this._emitClose();
    return this;
  }
}

export class ClientRequest extends OutgoingMessage {
  constructor(options = {}, callback) {
    super();

    this.agent = options.agent ?? globalAgent;
    this.method = String(options.method || 'GET').toUpperCase();
    this.protocol = options.protocol || 'http:';
    this.host = options.host ?? options.hostname ?? 'localhost';
    this.hostname = options.hostname ?? options.host ?? 'localhost';
    this.port = options.port ?? defaultPortForProtocol(this.protocol);
    this.path = options.path || '/';
    this.servername = options.servername;
    this.ALPNProtocols = options.ALPNProtocols;
    this.createConnection = options.createConnection;
    this.socket = null;
    this.connection = null;
    this.destroyed = false;
    this.aborted = false;
    this.finished = false;
    this.reusedSocket = false;
    this._headers = createHeadersObject();
    this._bodyChunks = [];
    this._controller = typeof AbortController === 'function' ? new AbortController() : null;
    this._timeout = 0;
    this._timeoutHandle = null;
    this._dispatchStarted = false;
    this._requestUrl = buildRequestUrl(options);
    this._closeEmitted = false;
    this._timedOut = false;
    this._socket = null;
    this._upgradeBuffer = Buffer.alloc(0);

    if (options.headers) {
      Object.keys(options.headers).forEach(name => {
        this.setHeader(name, options.headers[name]);
      });
    }

    if (typeof callback === 'function') this.once('response', callback);
    if (options.timeout !== undefined) this.setTimeout(options.timeout);
  }

  _emitClose() {
    if (this._closeEmitted) return;
    this._closeEmitted = true;
    this.emit('close');
  }

  _clearTimeoutTimer() {
    if (!this._timeoutHandle) return;
    clearTimeout(this._timeoutHandle);
    this._timeoutHandle = null;
  }

  _armTimeoutTimer() {
    this._clearTimeoutTimer();
    if (!(this._timeout > 0)) return;

    this._timeoutHandle = setTimeout(() => {
      if (this.destroyed) return;
      this._timedOut = true;
      this.emit('timeout');
      if (this._controller) this._controller.abort(createAbortError('Request timed out'));
    }, this._timeout);
  }

  _dispatch() {
    if (this._dispatchStarted || this.destroyed) return;
    this._dispatchStarted = true;
    this._armTimeoutTimer();

    if (hasUpgradeHeader(this._headers)) {
      this._dispatchUpgrade();
      return;
    }

    Promise.resolve()
      .then(async () => {
        const response = await fetch(this._requestUrl, {
          method: this.method,
          headers: buildRequestHeadersObject(this._headers),
          body: getFetchBody(this._bodyChunks),
          signal: this._controller ? this._controller.signal : undefined
        });

        if (this.destroyed) return;
        this._clearTimeoutTimer();

        const incoming = new FetchIncomingMessage(response);
        incoming.on('close', () => {
          this._emitClose();
        });

        this.emit('response', incoming);
        queueMicrotask(() => {
          incoming._pumpBody();
        });
      })
      .catch(error => {
        this._clearTimeoutTimer();
        if (this.destroyed || this._timedOut) {
          this._emitClose();
          return;
        }
        this.emit('error', error);
        this._emitClose();
      });
  }

  _dispatchUpgrade() {
    const connectOptions = {
      ...this.agent?.options,
      host: this.hostname || this.host,
      hostname: this.hostname || this.host,
      port: this.port,
      servername: this.servername,
      ALPNProtocols: this.ALPNProtocols
    };
    const createConnection =
      typeof this.createConnection === 'function'
        ? this.createConnection
        : this.protocol === 'https:'
          ? tls.connect
          : net.connect;

    let socket;
    try {
      socket = createConnection(connectOptions);
    } catch (error) {
      this._clearTimeoutTimer();
      this.emit('error', error);
      this._emitClose();
      return;
    }

    this._socket = socket;
    this.socket = socket;
    this.connection = socket;
    this.emit('socket', socket);

    const onConnect = () => {
      if (this.destroyed) return;

      const rawHeaders = buildRawRequestHeaders(this, this._headers);
      let head = `${this.method} ${this.path || '/'} HTTP/1.1\r\n`;
      for (let i = 0; i + 1 < rawHeaders.length; i += 2) {
        head += `${rawHeaders[i]}: ${rawHeaders[i + 1]}\r\n`;
      }
      head += '\r\n';
      socket.write(head);
    };

    const cleanup = () => {
      socket.removeListener('data', onData);
      socket.removeListener('error', onError);
      socket.removeListener('end', onEnd);
      socket.removeListener('close', onClose);
      socket.removeListener('connect', onConnect);
      socket.removeListener('secureConnect', onConnect);
    };

    const fail = (error) => {
      this._clearTimeoutTimer();
      if (this.destroyed) return;
      this.destroyed = true;
      cleanup();
      this.emit('error', error);
      this._emitClose();
    };

    const onError = (error) => fail(error);
    const onEnd = () => fail(new Error('socket hang up'));
    const onClose = () => {
      if (!this.destroyed) fail(new Error('socket hang up'));
    };

    const onData = (chunk) => {
      if (this.destroyed) return;

      this._upgradeBuffer = appendSocketChunk(this._upgradeBuffer, chunk);
      const headerEnd = this._upgradeBuffer.indexOf('\r\n\r\n');
      if (headerEnd === -1) return;

      const headText = this._upgradeBuffer.subarray(0, headerEnd).toString('latin1');
      const rest = this._upgradeBuffer.subarray(headerEnd + 4);
      this._upgradeBuffer = Buffer.alloc(0);
      this._clearTimeoutTimer();
      cleanup();

      let response;
      try {
        response = parseHttpResponseHead(headText);
      } catch (error) {
        fail(error);
        return;
      }

      if (response.statusCode === 101) {
        this.emit('upgrade', response, socket, rest);
      } else {
        const incoming = new FetchIncomingMessage({
          status: response.statusCode,
          statusText: response.statusMessage,
          headers: new Map(Object.entries(response.headers)),
          body: null,
          url: this._requestUrl
        });
        incoming.rawHeaders = response.rawHeaders;
        incoming.httpVersion = response.httpVersion;
        incoming.httpVersionMajor = response.httpVersionMajor;
        incoming.httpVersionMinor = response.httpVersionMinor;
        socket.destroy();
        this.emit('response', incoming);
        incoming._pumpBody();
      }
    };

    socket.on('data', onData);
    socket.on('error', onError);
    socket.on('end', onEnd);
    socket.on('close', onClose);

    if (this.protocol === 'https:') socket.once('secureConnect', onConnect);
    else socket.once('connect', onConnect);
  }

  setHeader(name, value) {
    this._headers[normalizeHeaderName(name)] = value;
    return this;
  }

  getHeader(name) {
    return this._headers[normalizeHeaderName(name)];
  }

  getHeaders() {
    return { ...this._headers };
  }

  removeHeader(name) {
    delete this._headers[normalizeHeaderName(name)];
    return this;
  }

  setTimeout(msecs, callback) {
    this._timeout = Number(msecs) || 0;
    if (typeof callback === 'function') this.on('timeout', callback);
    if (this._dispatchStarted && !this.destroyed) this._armTimeoutTimer();
    return this;
  }

  setNoDelay() {
    return this;
  }

  setSocketKeepAlive() {
    return this;
  }

  write(chunk, encoding, callback) {
    if (this.finished) throw new Error('write after end');
    this._bodyChunks.push(bufferFrom(chunk, typeof encoding === 'string' ? encoding : undefined));
    if (typeof encoding === 'function') callback = encoding;
    if (typeof callback === 'function') callback();
    return true;
  }

  end(chunk, encoding, callback) {
    if (this.finished) return this;
    if (typeof chunk === 'function') {
      callback = chunk;
      chunk = undefined;
      encoding = undefined;
    } else if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }

    if (chunk !== undefined && chunk !== null) {
      this.write(chunk, encoding);
    }

    this.finished = true;
    this.emit('finish');
    if (typeof callback === 'function') callback();
    this._dispatch();
    return this;
  }

  abort() {
    return this.destroy(createAbortError('Request aborted'));
  }

  destroy(error) {
    if (this.destroyed) return this;
    this.destroyed = true;
    this.aborted = true;
    this._clearTimeoutTimer();
    if (this._socket && typeof this._socket.destroy === 'function') this._socket.destroy();
    if (this._controller) this._controller.abort(error || createAbortError('Request destroyed'));
    if (error) this.emit('error', error);
    this._emitClose();
    return this;
  }
}

export class Agent extends EventEmitter {
  constructor(options = {}) {
    super();
    this.options = options && typeof options === 'object' ? options : {};
    this.requests = createAgentState();
    this.sockets = createAgentState();
    this.freeSockets = createAgentState();
    this.keepAlive = !!this.options.keepAlive;
    this.keepAliveMsecs = this.options.keepAliveMsecs ?? 1000;
    this.maxSockets = this.options.maxSockets ?? Infinity;
    this.maxFreeSockets = this.options.maxFreeSockets ?? 256;
    this.maxTotalSockets = this.options.maxTotalSockets ?? Infinity;
    this.totalSocketCount = 0;
    this.defaultPort = 80;
    this.protocol = 'http:';
    this.scheduling = this.options.scheduling ?? 'lifo';
  }

  createConnection() {
    clientNotImplemented();
  }

  createSocket(_options, callback) {
    const error = new Error('node:http Agent client transport is not implemented yet');
    if (typeof callback === 'function') callback(error);
    else throw error;
  }

  addRequest() {
    clientNotImplemented();
  }

  removeSocket() {}

  keepSocketAlive() {
    return false;
  }

  reuseSocket() {}

  destroy() {
    return this;
  }

  getName(options = {}) {
    if (options.socketPath) return String(options.socketPath);
    const host = options.host ?? 'localhost';
    const port = options.port ?? this.defaultPort;
    const localAddress = options.localAddress ? `:${options.localAddress}` : '';
    const family = options.family ? `:${options.family}` : '';
    return `${host}:${port}${localAddress}${family}`;
  }
}

export const globalAgent = new Agent();

export class IncomingMessage extends EventEmitter {
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
    this._bodyConsumed = false;
  }

  _takeBody() {
    if (this._bodyConsumed) return Buffer.alloc(0);
    this._bodyConsumed = true;
    return this._body;
  }

  _deliverBody() {
    if (this.destroyed || this.complete) return;

    const body = this._takeBody();
    if (body.length > 0) this.emit('data', body);
    this.complete = true;
    this.readableEnded = true;
    this.emit('end');
    this.emit('close');
  }

  [Symbol.asyncIterator]() {
    const body = this._takeBody();
    let emitted = false;
    let finished = false;

    return {
      next: async () => {
        if (finished) return { done: true, value: undefined };
        if (!emitted && body.length > 0) {
          emitted = true;
          return { done: false, value: body };
        }
        finished = true;
        this.complete = true;
        this.readableEnded = true;
        return { done: true, value: undefined };
      },
      return: async () => {
        finished = true;
        this.complete = true;
        this.readableEnded = true;
        return { done: true, value: undefined };
      },
      [Symbol.asyncIterator]() {
        return this;
      }
    };
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

export class ServerResponse extends EventEmitter {
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

  appendHeader(name, value) {
    const key = normalizeHeaderName(name);
    const existing = this._headers[key];
    if (existing === undefined) {
      this._headers[key] = value;
    } else if (Array.isArray(existing)) {
      if (Array.isArray(value)) existing.push(...value);
      else existing.push(value);
    } else {
      this._headers[key] = Array.isArray(value) ? [existing, ...value] : [existing, value];
    }
    if (!this._headerNames[key]) this._headerNames[key] = String(name);
    return this;
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
    if (typeof chunk === 'function') {
      callback = chunk;
      chunk = undefined;
      encoding = undefined;
    }
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

    keepAlive = !this._socketState.server._closing && this._shouldKeepAlive();
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

export class Server extends net.Server {
  constructor(options, requestListener) {
    const serverOptions = typeof options === 'function' ? undefined : options;
    const onRequest = typeof options === 'function' ? options : requestListener;

    super(serverOptions);
    this.timeout = 0;
    this.headersTimeout = 60000;
    this.requestTimeout = 300000;
    this.keepAliveTimeout = 5000;
    this.maxHeadersCount = 2000;
    this._socketStates = new Set();
    this._closing = false;

    this.on('connection', socket => {
      this._attachSocket(socket);
    });

    if (typeof onRequest === 'function') this.on('request', onRequest);
  }

  _attachSocket(socket) {
    const state = makeSocketState(this, socket);
    this._socketStates.add(state);

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
      this._socketStates.delete(state);
    });
  }

  listen(...args) {
    this._closing = false;
    return super.listen(...args);
  }

  close(callback) {
    this._closing = true;
    this.closeIdleConnections();
    return super.close(callback);
  }

  closeAllConnections() {
    for (const state of [...this._socketStates]) {
      closeSocket(state.socket);
    }
    return this;
  }

  closeIdleConnections() {
    for (const state of [...this._socketStates]) {
      if (state.closed) continue;
      if (state.activeResponse && !state.activeResponse.writableEnded) continue;
      closeSocket(state.socket);
    }
    return this;
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

export function createServer(options, requestListener) {
  return new Server(options, requestListener);
}

export function request(input, options, callback) {
  const normalized = normalizeRequestArgs(input, options, callback);
  const requestOptions = buildRequestOptions(normalized.input, normalized.options);
  return new ClientRequest(requestOptions, normalized.callback);
}

export function get(input, options, callback) {
  const req = request(input, options, callback);
  req.end();
  return req;
}

export { METHODS, STATUS_CODES } from 'ant:internal/http_metadata';
