import * as http from 'node:http';
import tls from 'ant:internal/tls';

const kSecureContext = Symbol('ant.internal.https.secureContext');

const TLS_CONTEXT_OPTION_KEYS = ['allowPartialChain', 'ca', 'cert', 'key'];

const TLS_SERVER_OPTION_KEYS = [
  'ALPNProtocols',
  'SNICallback',
  'allowPartialChain',
  'ca',
  'cert',
  'ciphers',
  'clientCertEngine',
  'crl',
  'dhparam',
  'ecdhCurve',
  'honorCipherOrder',
  'key',
  'maxVersion',
  'minVersion',
  'passphrase',
  'pfx',
  'rejectUnauthorized',
  'requestCert',
  'secureOptions',
  'secureProtocol',
  'sessionIdContext',
  'sigalgs',
  'ticketKeys'
];

function createPlainObject() {
  return Object.create(null);
}

function splitServerOptions(options) {
  const httpOptions = createPlainObject();
  const tlsOptions = createPlainObject();
  let hasTlsOptions = false;

  if (!options || typeof options !== 'object') {
    return { httpOptions: undefined, tlsOptions: undefined, hasTlsOptions: false };
  }

  Object.keys(options).forEach(key => {
    if (TLS_SERVER_OPTION_KEYS.includes(key)) {
      tlsOptions[key] = options[key];
      hasTlsOptions = true;
      return;
    }

    httpOptions[key] = options[key];
  });

  return {
    httpOptions,
    tlsOptions: hasTlsOptions ? tlsOptions : undefined,
    hasTlsOptions
  };
}

function toContextOptions(options) {
  const contextOptions = createPlainObject();

  if (!options) return contextOptions;
  TLS_CONTEXT_OPTION_KEYS.forEach(key => {
    if (options[key] !== undefined) contextOptions[key] = options[key];
  });
  return contextOptions;
}

function maybeCreateSecureContext(options) {
  if (!options) return undefined;
  return tls.createContext(toContextOptions(options));
}

function normalizeCreateServerArgs(options, requestListener) {
  if (typeof options === 'function') {
    return {
      serverOptions: undefined,
      requestListener: options
    };
  }

  return {
    serverOptions: options,
    requestListener
  };
}

function clientNotImplemented() {
  throw new Error('node:https client transport is not implemented yet');
}

// compatibility stub only
export class Server extends http.Server {
  constructor(options, requestListener) {
    const normalized = normalizeCreateServerArgs(options, requestListener);
    const split = splitServerOptions(normalized.serverOptions);

    super(split.httpOptions, normalized.requestListener);
    this[kSecureContext] = split.hasTlsOptions ? maybeCreateSecureContext(split.tlsOptions) : undefined;
  }
}

export class Agent extends http.Agent {
  constructor(options) {
    super(options);
    this.defaultPort = 443;
    this.protocol = 'https:';
    this.maxCachedSessions = this.options.maxCachedSessions ?? 100;
    this._sessionCache = { map: Object.create(null), list: [] };
  }
}

export const globalAgent = new Agent();

export function createSecureContext(options) {
  return tls.createContext(options);
}

export function isSecureContext(value) {
  return tls.isContext(value);
}

export function setConfigPath(path) {
  return tls.setConfigPath(path);
}

export function createServer(options, requestListener) {
  return new Server(options, requestListener);
}

export function request() {
  clientNotImplemented();
}

export function get() {
  clientNotImplemented();
}

export { METHODS, STATUS_CODES } from 'ant:internal/http_metadata';
