import querystringModule from 'node:querystring';
import helpers from 'ant:internal/path_helpers';
import * as primordials from 'ant:internal/primordials';
import * as internalUrl from 'ant:internal/url';

const querystring = querystringModule;
const { receivedType } = helpers;
const { StringPrototypeCharCodeAt, StringPrototypeIndexOf, StringPrototypeSlice } = primordials;

const {
  URL,
  URLSearchParams,
  domainToASCII,
  domainToUnicode,
  fileURLToPath,
  pathToFileURL,
  formatHref,
  legacyParse
} = internalUrl;

const decodeURIComponentFn = decodeURIComponent;
const replaceAll = String.prototype.replaceAll;
const StringPrototypeReplaceAll = (str, search, replacement) => replaceAll.call(str, search, replacement);

const CHAR_HASH = 35;
const CHAR_FORWARD_SLASH = 47;
const CHAR_LEFT_SQUARE_BRACKET = 91;
const CHAR_RIGHT_SQUARE_BRACKET = 93;
const CHAR_QUESTION_MARK = 63;
const CHAR_COLON = 58;

const hostlessProtocol = new Set(['javascript', 'javascript:']);

const slashedProtocol = new Set([
  'http', 'http:', 'https', 'https:', 'ftp', 'ftp:', 'gopher', 'gopher:',
  'file', 'file:', 'ws', 'ws:', 'wss', 'wss:'
]);

function invalidArgType(name, expected, value) {
  const error = new TypeError(`The "${name}" argument must be ${expected}. Received ${receivedType(value)}`);
  error.code = 'ERR_INVALID_ARG_TYPE';
  return error;
}

function invalidUrl(input) {
  const error = new TypeError('Invalid URL');
  error.code = 'ERR_INVALID_URL';
  error.input = input;
  return error;
}

function invalidPort(url) {
  const error = new TypeError(`The argument 'url' ${url}. Received 'Invalid port in url'`);
  error.code = 'ERR_INVALID_ARG_VALUE';
  return error;
}

function validateString(value, name) {
  if (typeof value !== 'string') throw invalidArgType(name, 'of type string', value);
}

function validateObject(value, name) {
  if (value === null || Array.isArray(value) || typeof value !== 'object') throw invalidArgType(name, 'of type object', value);
}

function isObject(value) {
  return value !== null && typeof value === 'object';
}

function Url() {
  this.protocol = null;
  this.slashes = null;
  this.auth = null;
  this.host = null;
  this.port = null;
  this.hostname = null;
  this.hash = null;
  this.search = null;
  this.query = null;
  this.pathname = null;
  this.path = null;
  this.href = null;
}

const portPattern = /:[0-9]*$/;

function urlParse(url, parseQueryString, slashesDenoteHost) {
  if (url instanceof Url) return url;
  const urlObject = new Url();
  urlObject.parse(url, parseQueryString, slashesDenoteHost);
  return urlObject;
}

function isIpv6Hostname(hostname) {
  return (
    StringPrototypeCharCodeAt(hostname, 0) === CHAR_LEFT_SQUARE_BRACKET &&
    StringPrototypeCharCodeAt(hostname, hostname.length - 1) === CHAR_RIGHT_SQUARE_BRACKET
  );
}

const PARSED_PROTOCOL = 1 << 0;
const PARSED_SLASHES = 1 << 1;
const PARSED_AUTH = 1 << 2;
const PARSED_HOST = 1 << 3;
const PARSED_PORT = 1 << 4;
const PARSED_HOSTNAME = 1 << 5;
const PARSED_HASH = 1 << 6;
const PARSED_SEARCH = 1 << 7;
const PARSED_QUERY = 1 << 8;
const PARSED_PATHNAME = 1 << 9;
const PARSED_SIMPLE = 1 << 12;
const PARSED_PARSE_QUERY = 1 << 13;
const PARSED_HOST_BRANCH = 1 << 14;
const PARSED_IPV6 = 1 << 15;
const PARSED_STATUS_SHIFT = 16;
const PARSED_INVALID_PORT = 1;
const PARSED_INVALID_URL = 2;

Url.prototype.parse = function parse(url, parseQueryString, slashesDenoteHost) {
  validateString(url, 'url');

  const parsed = legacyParse(this, url, parseQueryString, slashesDenoteHost);
  const flags = parsed[0];

  if (flags & PARSED_SIMPLE) {
    this.path = parsed[11];
    this.href = parsed[12];
    this.pathname = parsed[10];
    if (flags & PARSED_SEARCH) this.search = parsed[8];
    if (flags & PARSED_QUERY) this.query = flags & PARSED_PARSE_QUERY ? querystring.parse(parsed[9]) : parsed[9];
    return this;
  }

  if (flags & PARSED_PROTOCOL) this.protocol = parsed[1];
  if (flags & PARSED_SLASHES) this.slashes = true;
  if (flags & PARSED_AUTH) this.auth = decodeURIComponentFn(parsed[3]);
  if (flags & PARSED_HOST) this.host = parsed[4];
  if (flags & PARSED_PORT) this.port = parsed[5];
  if (flags & PARSED_HOSTNAME) this.hostname = parsed[6];

  const status = flags >>> PARSED_STATUS_SHIFT;
  if (status === PARSED_INVALID_PORT) throw invalidPort(url);
  if (status === PARSED_INVALID_URL) throw invalidUrl(url);

  if (flags & PARSED_HOST_BRANCH) {
    const p = this.port ? ':' + this.port : '';
    const h = this.hostname || '';
    this.host = h + p;
    if (flags & PARSED_IPV6) this.hostname = this.hostname.slice(1, -1);
  }

  if (flags & PARSED_HASH) this.hash = parsed[7];
  if (flags & PARSED_SEARCH) this.search = parsed[8];
  if (flags & PARSED_QUERY) {
    this.query = parsed[9];
    if (flags & PARSED_PARSE_QUERY) this.query = querystring.parse(this.query);
  }
  if (flags & PARSED_PATHNAME) this.pathname = parsed[10];

  if (this.pathname || this.search) {
    const p = this.pathname || '';
    const s = this.search || '';
    this.path = p + s;
  }

  this.href = this.format();
  return this;
};

function urlFormat(urlObject, options) {
  if (typeof urlObject === 'string') {
    urlObject = urlParse(urlObject);
  } else if (!isObject(urlObject)) {
    throw invalidArgType('urlObject', 'one of type object or string', urlObject);
  } else if (urlObject instanceof URL) {
    let fragment = true;
    let unicode = false;
    let search = true;
    let auth = true;

    if (options) {
      validateObject(options, 'options');
      if (options.fragment != null) fragment = Boolean(options.fragment);
      if (options.unicode != null) unicode = Boolean(options.unicode);
      if (options.search != null) search = Boolean(options.search);
      if (options.auth != null) auth = Boolean(options.auth);
    }

    return formatHref(urlObject.href, fragment, unicode, search, auth);
  }

  return Url.prototype.format.call(urlObject);
}

const hexTable = new Array(256);
for (let i = 0; i < 256; ++i) hexTable[i] = '%' + ((i < 16 ? '0' : '') + i.toString(16)).toUpperCase();

const noEscapeAuth = new Int8Array([
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0
]);

const noEscapeAuthPattern = /[^A-Za-z0-9!'()*\-._~:]/;

function encodeAuth(str) {
  if (typeof str === 'string' && !noEscapeAuthPattern.test(str)) return str;
  const len = str.length;
  if (len === 0) return '';

  let out = '';
  let lastPos = 0;
  let i = 0;

  outer: for (; i < len; i++) {
    let c = StringPrototypeCharCodeAt(str, i);

    while (c < 0x80) {
      if (noEscapeAuth[c] !== 1) {
        if (lastPos < i) out += StringPrototypeSlice(str, lastPos, i);
        lastPos = i + 1;
        out += hexTable[c];
      }
      if (++i === len) break outer;
      c = StringPrototypeCharCodeAt(str, i);
    }

    if (lastPos < i) out += StringPrototypeSlice(str, lastPos, i);

    if (c < 0x800) {
      lastPos = i + 1;
      out += hexTable[0xc0 | (c >> 6)] + hexTable[0x80 | (c & 0x3f)];
      continue;
    }
    if (c < 0xd800 || c >= 0xe000) {
      lastPos = i + 1;
      out += hexTable[0xe0 | (c >> 12)] + hexTable[0x80 | ((c >> 6) & 0x3f)] + hexTable[0x80 | (c & 0x3f)];
      continue;
    }
    ++i;

    if (i >= len) {
      const error = new URIError('URI malformed');
      error.code = 'ERR_INVALID_URI';
      throw error;
    }

    const c2 = StringPrototypeCharCodeAt(str, i) & 0x3ff;

    lastPos = i + 1;
    c = 0x10000 + (((c & 0x3ff) << 10) | c2);
    out +=
      hexTable[0xf0 | (c >> 18)] +
      hexTable[0x80 | ((c >> 12) & 0x3f)] +
      hexTable[0x80 | ((c >> 6) & 0x3f)] +
      hexTable[0x80 | (c & 0x3f)];
  }
  if (lastPos === 0) return str;
  if (lastPos < len) return out + StringPrototypeSlice(str, lastPos);
  return out;
}

function escapePathDelimiters(pathname) {
  let newPathname = '';
  let lastPos = 0;
  const len = pathname.length;
  for (let i = 0; i < len; i++) {
    const code = StringPrototypeCharCodeAt(pathname, i);
    if (code === CHAR_HASH || code === CHAR_QUESTION_MARK) {
      if (i > lastPos) newPathname += StringPrototypeSlice(pathname, lastPos, i);
      newPathname += code === CHAR_HASH ? '%23' : '%3F';
      lastPos = i + 1;
    }
  }
  if (lastPos < len) newPathname += StringPrototypeSlice(pathname, lastPos);
  return newPathname;
}

Url.prototype.format = function format() {
  const rawAuth = this.auth || '';
  const auth = rawAuth ? encodeAuth(rawAuth) + '@' : rawAuth;

  const rawProtocol = this.protocol || '';
  const protocol =
    rawProtocol && StringPrototypeCharCodeAt(rawProtocol, rawProtocol.length - 1) !== CHAR_COLON
      ? rawProtocol + ':'
      : rawProtocol;

  let pathname = this.pathname || '';
  let hash = this.hash || '';
  let host = '';
  let query = '';

  if (this.host) {
    host = auth + this.host;
  } else if (this.hostname) {
    const hostname = auth + (
      StringPrototypeIndexOf(this.hostname, ':') !== -1 && !isIpv6Hostname(this.hostname)
        ? '[' + this.hostname + ']'
        : this.hostname
    );
    host = this.port ? hostname + (':' + this.port) : hostname;
  }

  if (isObject(this.query)) {
    query = querystring.stringify(this.query);
  }
  let search = this.search || (query && '?' + query) || '';

  if (StringPrototypeIndexOf(pathname, '#') !== -1 || StringPrototypeIndexOf(pathname, '?') !== -1) {
    pathname = escapePathDelimiters(pathname);
  }

  if (this.slashes || slashedProtocol.has(protocol)) {
    if (this.slashes || host) {
      if (pathname && StringPrototypeCharCodeAt(pathname, 0) !== CHAR_FORWARD_SLASH) pathname = '/' + pathname;
      host = '//' + host;
    } else if (
      protocol.length >= 4 &&
      StringPrototypeCharCodeAt(protocol, 0) === 102 &&
      StringPrototypeCharCodeAt(protocol, 1) === 105 &&
      StringPrototypeCharCodeAt(protocol, 2) === 108 &&
      StringPrototypeCharCodeAt(protocol, 3) === 101
    ) {
      host = '//';
    }
  }

  if (StringPrototypeIndexOf(search, '#') !== -1) {
    search = StringPrototypeReplaceAll(search, '#', '%23');
  }

  if (hash && StringPrototypeCharCodeAt(hash, 0) !== CHAR_HASH) {
    hash = '#' + hash;
  }
  if (search && StringPrototypeCharCodeAt(search, 0) !== CHAR_QUESTION_MARK) {
    search = '?' + search;
  }

  return protocol + host + pathname + search + hash;
};

function urlResolve(source, relative) {
  return urlParse(source, false, true).resolve(relative);
}

Url.prototype.resolve = function resolve(relative) {
  return this.resolveObject(urlParse(relative, false, true)).format();
};

function urlResolveObject(source, relative) {
  if (!source) return relative;
  return urlParse(source, false, true).resolveObject(relative);
}

function spliceOne(list, index) {
  for (; index + 1 < list.length; index++) list[index] = list[index + 1];
  list.pop();
}

Url.prototype.resolveObject = function resolveObject(relative) {
  if (typeof relative === 'string') {
    const rel = new Url();
    rel.parse(relative, false, true);
    relative = rel;
  }

  const result = new Url();
  Object.assign(result, this);

  result.hash = relative.hash;

  if (relative.href === '') {
    result.href = result.format();
    return result;
  }

  if (relative.slashes && !relative.protocol) {
    const relativeWithoutProtocol = Object.keys(relative).reduce((acc, key) => {
      if (key !== 'protocol') {
        acc[key] = relative[key];
      }
      return acc;
    }, {});
    Object.assign(result, relativeWithoutProtocol);

    if (slashedProtocol.has(result.protocol) && result.hostname && !result.pathname) {
      result.path = result.pathname = '/';
    }

    result.href = result.format();
    return result;
  }

  if (relative.protocol && relative.protocol !== result.protocol) {
    if (!slashedProtocol.has(relative.protocol)) {
      Object.assign(result, relative);
      result.href = result.format();
      return result;
    }

    result.protocol = relative.protocol;
    if (!relative.host && !/^file:?$/.test(relative.protocol) && !hostlessProtocol.has(relative.protocol)) {
      const relPath = (relative.pathname || '').split('/');
      while (relPath.length && !(relative.host = relPath.shift()));
      relative.host ||= '';
      relative.hostname ||= '';
      if (relPath[0] !== '') relPath.unshift('');
      if (relPath.length < 2) relPath.unshift('');
      result.pathname = relPath.join('/');
    } else {
      result.pathname = relative.pathname;
    }
    result.search = relative.search;
    result.query = relative.query;
    result.host = relative.host || '';
    result.auth = relative.auth;
    result.hostname = relative.hostname || relative.host;
    result.port = relative.port;
    if (result.pathname || result.search) {
      const p = result.pathname || '';
      const s = result.search || '';
      result.path = p + s;
    }
    result.slashes ||= relative.slashes;
    result.href = result.format();
    return result;
  }

  const isSourceAbs = result.pathname && result.pathname.charAt(0) === '/';
  const isRelAbs = relative.host || (relative.pathname && relative.pathname.charAt(0) === '/');
  let mustEndAbs = isRelAbs || isSourceAbs || (result.host && relative.pathname);
  const removeAllDots = mustEndAbs;
  let srcPath = (result.pathname && result.pathname.split('/')) || [];
  const relPath = (relative.pathname && relative.pathname.split('/')) || [];
  const noLeadingSlashes = result.protocol && !slashedProtocol.has(result.protocol);

  if (noLeadingSlashes) {
    result.hostname = '';
    result.port = null;
    if (result.host) {
      if (srcPath[0] === '') srcPath[0] = result.host;
      else srcPath.unshift(result.host);
    }
    result.host = '';
    if (relative.protocol) {
      relative.hostname = null;
      relative.port = null;
      result.auth = null;
      if (relative.host) {
        if (relPath[0] === '') relPath[0] = relative.host;
        else relPath.unshift(relative.host);
      }
      relative.host = null;
    }
    mustEndAbs &&= relPath[0] === '' || srcPath[0] === '';
  }

  if (isRelAbs) {
    if (relative.host || relative.host === '') {
      if (result.host !== relative.host) result.auth = null;
      result.host = relative.host;
      result.port = relative.port;
    }
    if (relative.hostname || relative.hostname === '') {
      if (result.hostname !== relative.hostname) result.auth = null;
      result.hostname = relative.hostname;
    }
    result.search = relative.search;
    result.query = relative.query;
    srcPath = relPath;
  } else if (relPath.length) {
    srcPath ||= [];
    srcPath.pop();
    srcPath = srcPath.concat(relPath);
    result.search = relative.search;
    result.query = relative.query;
  } else if (relative.search !== null && relative.search !== undefined) {
    if (noLeadingSlashes) {
      result.hostname = result.host = srcPath.shift();
      const authInHost = result.host && result.host.indexOf('@') > 0 && result.host.split('@');
      if (authInHost) {
        result.auth = authInHost.shift();
        result.host = result.hostname = authInHost.shift();
      }
    }
    result.search = relative.search;
    result.query = relative.query;
    if (result.pathname !== null || result.search !== null) {
      result.path = (result.pathname ? result.pathname : '') + (result.search ? result.search : '');
    }
    result.href = result.format();
    return result;
  }

  if (!srcPath.length) {
    result.pathname = null;
    if (result.search) {
      result.path = '/' + result.search;
    } else {
      result.path = null;
    }
    result.href = result.format();
    return result;
  }

  let last = srcPath[srcPath.length - 1];
  const hasTrailingSlash =
    ((result.host || relative.host || srcPath.length > 1) && (last === '.' || last === '..')) || last === '';

  let up = 0;
  for (let i = srcPath.length - 1; i >= 0; i--) {
    last = srcPath[i];
    if (last === '.') {
      spliceOne(srcPath, i);
    } else if (last === '..') {
      spliceOne(srcPath, i);
      up++;
    } else if (up) {
      spliceOne(srcPath, i);
      up--;
    }
  }

  if (!mustEndAbs && !removeAllDots) {
    while (up--) {
      srcPath.unshift('..');
    }
  }

  if (mustEndAbs && srcPath[0] !== '' && (!srcPath[0] || srcPath[0].charAt(0) !== '/')) {
    srcPath.unshift('');
  }

  if (hasTrailingSlash && srcPath.join('/').at(-1) !== '/') {
    srcPath.push('');
  }

  const isAbsolute = srcPath[0] === '' || (srcPath[0] && srcPath[0].charAt(0) === '/');

  if (noLeadingSlashes) {
    result.hostname = result.host = isAbsolute ? '' : srcPath.length ? srcPath.shift() : '';
    const authInHost = result.host && result.host.indexOf('@') > 0 ? result.host.split('@') : false;
    if (authInHost) {
      result.auth = authInHost.shift();
      result.host = result.hostname = authInHost.shift();
    }
  }

  mustEndAbs ||= result.host && srcPath.length;

  if (mustEndAbs && !isAbsolute) {
    srcPath.unshift('');
  }

  if (!srcPath.length) {
    result.pathname = null;
    result.path = null;
  } else {
    result.pathname = srcPath.join('/');
  }

  if (result.pathname !== null || result.search !== null) {
    result.path = (result.pathname ? result.pathname : '') + (result.search ? result.search : '');
  }
  result.auth = relative.auth || result.auth;
  result.slashes ||= relative.slashes;
  result.href = result.format();
  return result;
};

Url.prototype.parseHost = function parseHost() {
  let host = this.host;
  let port = portPattern.exec(host);
  if (port) {
    port = port[0];
    if (port !== ':') {
      this.port = port.slice(1);
    }
    host = host.slice(0, host.length - port.length);
  }
  if (host) this.hostname = host;
};

function urlToHttpOptions(url) {
  if (url === null || (typeof url !== 'object' && typeof url !== 'function')) {
    throw invalidArgType('url', 'of type object', url);
  }
  const { hostname, pathname, port, username, password, search } = url;
  const options = {
    __proto__: null,
    ...url,
    protocol: url.protocol,
    hostname: hostname && hostname[0] === '[' ? hostname.slice(1, -1) : hostname,
    hash: url.hash,
    search: search,
    pathname: pathname,
    path: `${pathname || ''}${search || ''}`,
    href: url.href
  };
  if (port !== '') {
    options.port = Number(port);
  }
  if (username || password) {
    options.auth = `${decodeURIComponent(username)}:${decodeURIComponent(password)}`;
  }
  return options;
}

export {
  Url,
  urlParse as parse,
  urlResolve as resolve,
  urlResolveObject as resolveObject,
  urlFormat as format,
  URL,
  URLSearchParams,
  domainToASCII,
  domainToUnicode,
  pathToFileURL,
  fileURLToPath,
  urlToHttpOptions
};

export default {
  Url,
  parse: urlParse,
  resolve: urlResolve,
  resolveObject: urlResolveObject,
  format: urlFormat,
  URL,
  URLSearchParams,
  domainToASCII,
  domainToUnicode,
  pathToFileURL,
  fileURLToPath,
  urlToHttpOptions
};
