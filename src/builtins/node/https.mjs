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

function createInvalidProtocolError(protocol) {
  const error = new TypeError(`Protocol "${protocol}" not supported. Expected "https:"`);
  error.code = 'ERR_INVALID_PROTOCOL';
  return error;
}

function getProtocolFromInput(input) {
  if (typeof input === 'string') return new URL(input).protocol;
  if (!input || typeof input !== 'object') return undefined;
  if (typeof input.href === 'string') return new URL(String(input)).protocol;
  if (input.protocol === undefined || input.protocol === null) return undefined;
  return String(input.protocol);
}

function assertHttpsProtocol(protocol) {
  if (protocol === undefined || protocol === null || protocol === '') return;
  if (String(protocol) !== 'https:') throw createInvalidProtocolError(String(protocol));
}

function applyDefaultHttpsProtocol(input, options) {
  const optionProtocol = options && typeof options === 'object' ? options.protocol : undefined;
  assertHttpsProtocol(optionProtocol);

  if (typeof input === 'string' || (input && typeof input === 'object' && typeof input.href === 'string')) {
    if (optionProtocol === undefined) assertHttpsProtocol(getProtocolFromInput(input));
    return [input, options];
  }

  if (typeof input === 'function' || input === undefined || input === null) {
    return [{ protocol: 'https:' }, options];
  }

  assertHttpsProtocol(getProtocolFromInput(input));
  return [{ ...input, protocol: input.protocol ?? 'https:' }, options];
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

export function request(input, options, callback) {
  const [resolvedInput, resolvedOptions] = applyDefaultHttpsProtocol(input, options);
  return http.request(resolvedInput, resolvedOptions, callback);
}

export function get(input, options, callback) {
  const [resolvedInput, resolvedOptions] = applyDefaultHttpsProtocol(input, options);
  return http.get(resolvedInput, resolvedOptions, callback);
}

export { METHODS, STATUS_CODES } from 'ant:internal/http_metadata';
