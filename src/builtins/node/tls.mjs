import net from 'node:net';
import internalTls from 'ant:internal/tls';

function defineTlsState(target) {
  if (!target || (typeof target !== 'object' && typeof target !== 'function')) return target;

  if (target.encrypted === undefined) target.encrypted = true;
  if (target.authorized === undefined) target.authorized = true;
  if (target.authorizationError === undefined) target.authorizationError = null;
  if (target.secureConnecting === undefined) target.secureConnecting = false;

  return target;
}

function wrapSocket(socket) {
  if (!socket || (typeof socket !== 'object' && typeof socket !== 'function')) return socket;
  defineTlsState(socket);

  if (!(socket instanceof TLSSocket)) {
    Object.setPrototypeOf(socket, TLSSocket.prototype);
  }

  return socket;
}

function createConnectionArgs(args) {
  if (args.length === 1 && typeof args[0] === 'object' && args[0] !== null) {
    return args[0];
  }

  const [port, host, options] = args;
  const normalized = Object.create(null);

  if (typeof port === 'number') normalized.port = port;
  if (typeof host === 'string') normalized.host = host;
  if (options && typeof options === 'object') Object.assign(normalized, options);
  return normalized;
}

export class TLSSocket extends net.Socket {
  constructor(socket, options = undefined) {
    if (socket && typeof socket === 'object') {
      super(options);
      return wrapSocket(socket);
    }

    super(socket);
    defineTlsState(this);
  }

  getCipher() {
    return undefined;
  }

  getProtocol() {
    return undefined;
  }

  getSession() {
    return undefined;
  }

  getPeerCertificate() {
    return {};
  }

  renegotiate(_options, callback) {
    if (typeof callback === 'function') callback(null);
    return true;
  }
}

export class SecureContext {
  constructor(options) {
    return internalTls.createContext(options);
  }
}

export function createSecureContext(options) {
  return internalTls.createContext(options);
}

export function isSecureContext(value) {
  return internalTls.isContext(value);
}

export function setConfigPath(path) {
  return internalTls.setConfigPath(path);
}

export function checkServerIdentity() {
  return undefined;
}

export function getCiphers() {
  return [];
}

export function createConnection(...args) {
  const socket = net.connect(createConnectionArgs(args));
  return wrapSocket(socket);
}

export function connect(...args) {
  return createConnection(...args);
}

export const rootCertificates = Object.freeze([]);
export const DEFAULT_ECDH_CURVE = 'auto';
export const DEFAULT_MIN_VERSION = 'TLSv1.2';
export const DEFAULT_MAX_VERSION = 'TLSv1.3';
export const CLIENT_RENEG_LIMIT = 3;
export const CLIENT_RENEG_WINDOW = 600;

export default {
  TLSSocket,
  SecureContext,
  CLIENT_RENEG_LIMIT,
  CLIENT_RENEG_WINDOW,
  DEFAULT_ECDH_CURVE,
  DEFAULT_MIN_VERSION,
  DEFAULT_MAX_VERSION,
  rootCertificates,
  checkServerIdentity,
  connect,
  createConnection,
  createSecureContext,
  getCiphers,
  isSecureContext,
  setConfigPath
};
