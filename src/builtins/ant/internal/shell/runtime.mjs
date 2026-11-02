// @ant: compact_on_build
import ops from 'ant:internal/shell_ops';
import fs from 'node:fs';
import os from 'node:os';
import { ops as arithOps, parseArith, evalArith, ArithError, parseNumber } from 'ant:internal/shell/arith';
import { compilePattern, escapeGlob, unescapeGlob, hasGlob, matchPattern, globExpand, ereToRegExp } from 'ant:internal/shell/pattern';

const FD_OPEN_READ = 0;
const FD_OPEN_WRITE = 1;
const FD_OPEN_APPEND = 2;
const FD_OPEN_READ_WRITE = 3;
const FD_DUP = 5;
const FD_CLOSE = 6;
const FD_BYTES = 7;
const FD_HOST = 8;
const FD_CAPTURE = 9;
const FD_ADOPT = 10;

const encoder = new TextEncoder();
const EMPTY = new Uint8Array(0);

export const AT = Symbol('@');
export const ST = Symbol('*');

export class ShellExit {
  constructor(status) {
    this.status = status;
  }
}

export class ShellReturn {
  constructor(status) {
    this.status = status;
  }
}

export class ShellError {
  constructor(message, status = 1, fatal = false) {
    this.message = message;
    this.status = status;
    this.fatal = fatal;
  }
}

export function toShellError(error) {
  if (error instanceof ShellError) return error;
  if (error instanceof ArithError) return new ShellError(`${error.message}${error.token ? ` (error token is "${error.token}")` : ''}`);
  return null;
}

export class Star {
  constructor(list) {
    this.list = list;
  }
}

export class AssignWord {
  constructor(spec) {
    this.n = spec.n;
    this.i = spec.i;
    this.v = spec.v;
    this.a = spec.a;
    this.p = spec.p;
  }

  toString() {
    if (this.a) return `${this.n}=(${this.a.map(e => e[1]).join(' ')})`;
    return `${this.n}${this.i !== undefined ? `[${this.i}]` : ''}${this.p ? '+' : ''}=${this.v}`;
  }
}

const strictDecoder = new TextDecoder('utf-8', { fatal: true });
function hasRawByte(s) {
  for (let i = 0; i < s.length; i++) {
    const code = s.charCodeAt(i);
    if (code >= 0xdc80 && code <= 0xdcff) return true;
  }
  return false;
}

export function rawByte(b) {
  b &= 255;
  return String.fromCharCode(b < 0x80 ? b : 0xdc00 + b);
}

export function encodeOut(s) {
  if (typeof s !== 'string' || !hasRawByte(s)) return s;
  const out = [];
  for (let i = 0; i < s.length; i++) {
    let cp = s.charCodeAt(i);
    if (cp >= 0xdc80 && cp <= 0xdcff) {
      out.push(cp - 0xdc00);
      continue;
    }
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < s.length) {
      const lo = s.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i++;
      }
    }
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 63));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
  }
  return Uint8Array.from(out);
}

export function toBytes(s) {
  const encoded = encodeOut(s);
  return typeof encoded === 'string' ? encoder.encode(encoded) : encoded;
}

export function decodeBytes(bytes) {
  if (!bytes.length) return '';
  try {
    return strictDecoder.decode(bytes);
  } catch {}
  let out = '';
  let i = 0;
  const n = bytes.length;
  while (i < n) {
    const b = bytes[i];
    let need = 0;
    let cp = 0;
    if (b < 0x80) {
      out += String.fromCharCode(b);
      i++;
      continue;
    } else if (b >= 0xc2 && b <= 0xdf) {
      need = 1;
      cp = b & 31;
    } else if (b >= 0xe0 && b <= 0xef) {
      need = 2;
      cp = b & 15;
    } else if (b >= 0xf0 && b <= 0xf4) {
      need = 3;
      cp = b & 7;
    } else {
      out += String.fromCharCode(0xdc00 + b);
      i++;
      continue;
    }
    let ok = i + need < n;
    for (let k = 1; ok && k <= need; k++) {
      const next = bytes[i + k];
      if ((next & 0xc0) !== 0x80) ok = false;
      else cp = (cp << 6) | (next & 63);
    }
    if (ok && ((need === 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (need === 3 && (cp < 0x10000 || cp > 0x10ffff)))) ok = false;
    if (!ok) {
      out += String.fromCharCode(0xdc00 + b);
      i++;
      continue;
    }
    out += String.fromCodePoint(cp);
    i += need + 1;
  }
  return out;
}

function asBuffer(bytes) {
  if (Buffer.isBuffer(bytes)) return bytes;
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

export class Sink {
  constructor() {
    this.chunks = [];
    this.size = 0;
  }

  push(data) {
    if (typeof data === 'string') {
      if (!data) return;
      data = toBytes(data);
    }
    if (!data.length) return;
    this.chunks.push(data);
    this.size += data.length;
    if (this.mirror) this.mirror.write(data);
  }

  bytes() {
    if (this.chunks.length === 0) return Buffer.alloc(0);
    if (this.chunks.length === 1) return asBuffer(this.chunks[0]);
    const out = Buffer.alloc(this.size);
    let offset = 0;
    for (const chunk of this.chunks) {
      out.set(chunk, offset);
      offset += chunk.length;
    }
    this.chunks = [out];
    return out;
  }

  text() {
    return this.size ? decodeBytes(this.bytes()) : '';
  }
}

class Var {
  constructor(v) {
    this.v = v;
    this.a = null;
    this.k = 0;
    this.x = false;
    this.r = false;
    this.int = false;
    this.lc = false;
    this.uc = false;
    this.nref = false;
    this.t = false;
  }

  clone() {
    const copy = new Var(this.v);
    copy.a = this.k === 1 ? this.a.slice() : this.k === 2 ? new Map(this.a) : null;
    copy.k = this.k;
    copy.x = this.x;
    copy.r = this.r;
    copy.int = this.int;
    copy.lc = this.lc;
    copy.uc = this.uc;
    copy.nref = this.nref;
    copy.t = this.t;
    return copy;
  }

  isSet() {
    if (this.t) return false;
    if (this.k === 0) return this.v !== undefined;
    if (this.k === 1) return this.a[0] !== undefined;
    return this.a.has('0');
  }
}

function sharedVar(value) {
  const v = new Var(value);
  v.shared = true;
  return v;
}

const DEFAULT_VARS = new Map([
  ['IFS', sharedVar(' \t\n')],
  ['PS4', sharedVar('+ ')],
  ['OPTIND', sharedVar('1')]
]);

class Vars {
  constructor(parent, env = null) {
    this.frames = [new Map()];
    this.parent = parent;
    this.env = parent ? parent.env : env || process.env;
    this.dirty = parent ? parent.dirty : env !== null;
  }

  find(name) {
    let v = this.findRaw(name);
    for (let depth = 0; v !== null && v.nref && v.v && depth < 16; depth++) v = this.findRaw(v.v);
    return v;
  }

  findRaw(name) {
    const frames = this.frames;
    for (let i = frames.length - 1; i >= 0; i--) {
      const v = frames[i].get(name);
      if (v !== undefined) return v.t ? null : v;
    }
    if (this.parent) return this.parent.findRaw(name);
    const shared = DEFAULT_VARS.get(name);
    if (shared) return shared;
    const env = this.env[name];
    if (typeof env === 'string') {
      const v = new Var(env);
      v.x = true;
      frames[0].set(name, v);
      return v;
    }
    return null;
  }

  writable(name, depth = 0) {
    const frames = this.frames;
    for (let i = frames.length - 1; i >= 0; i--) {
      const v = frames[i].get(name);
      if (v !== undefined) {
        if (v.t) {
          const fresh = new Var(undefined);
          fresh.x = v.x;
          frames[i].set(name, fresh);
          return fresh;
        }
        if (v.nref && v.v && depth < 16) return this.writable(v.v, depth + 1);
        return v;
      }
    }
    const inherited = this.parent ? this.parent.findRaw(name) : this.findRaw(name);
    if (inherited && inherited.nref && inherited.v && depth < 16) return this.writable(inherited.v, depth + 1);
    if (inherited) {
      if (!this.parent && !inherited.shared) return inherited;
      const copy = inherited.clone();
      frames[0].set(name, copy);
      return copy;
    }
    const fresh = new Var(undefined);
    frames[0].set(name, fresh);
    return fresh;
  }

  local(name) {
    const top = this.frames[this.frames.length - 1];
    let v = top.get(name);
    if (!v || v.t) {
      v = new Var(undefined);
      top.set(name, v);
    }
    return v;
  }

  unset(name, raw = false, depth = 0) {
    const frames = this.frames;
    const ref = raw ? null : this.findRaw(name);
    if (ref && ref.nref && ref.v && depth < 16) return this.unset(ref.v, false, depth + 1);
    for (let i = frames.length - 1; i >= 0; i--) {
      const v = frames[i].get(name);
      if (v !== undefined) {
        if (v.r) return false;
        if (v.x) this.dirty = true;
        const tomb = new Var(undefined);
        tomb.t = true;
        frames[i].set(name, tomb);
        return true;
      }
    }
    const inherited = this.parent ? this.parent.findRaw(name) : this.findRaw(name);
    if (inherited) {
      if (inherited.r) return false;
      if (inherited.x) this.dirty = true;
      const tomb = new Var(undefined);
      tomb.t = true;
      frames[0].set(name, tomb);
    }
    return true;
  }

  names() {
    const set = new Set();
    let vars = this;
    const chain = [];
    while (vars) {
      chain.push(vars);
      vars = vars.parent;
    }
    for (const name of Object.keys(this.env)) set.add(name);
    for (const v of chain) for (const frame of v.frames) for (const name of frame.keys()) set.add(name);
    return [...set].filter(name => this.find(name)).sort();
  }
}

class Funcs {
  constructor(parent) {
    this.map = null;
    this.parent = parent;
  }

  get(name) {
    const f = this.map === null ? undefined : this.map.get(name);
    if (f !== undefined) return f;
    return this.parent ? this.parent.get(name) : null;
  }

  set(name, fn) {
    (this.map ??= new Map()).set(name, fn);
  }

  delete(name) {
    (this.map ??= new Map()).set(name, null);
  }

  names() {
    const set = new Set();
    for (let f = this; f; f = f.parent) if (f.map) for (const name of f.map.keys()) set.add(name);
    return [...set].filter(name => this.get(name)).sort();
  }
}

const DEFAULT_OPTS = {
  e: false, u: false, x: false, f: false, C: false, a: false, n: false, v: false,
  pipefail: false, nullglob: false, failglob: false, dotglob: false, globstar: false,
  extglob: false, nocaseglob: false, nocasematch: false, lastpipe: false, xpg_echo: false,
  expand_aliases: false, posix: false, inherit_errexit: false, shift_verbose: false,
  checkwinsize: true, cmdhist: true, extquote: true, globasciiranges: true, globskipdots: true,
  complete_fullquote: true, force_fignore: true, hostcomplete: true, progcomp: true,
  interactive_comments: true, patsub_replacement: true, promptvars: true, sourcepath: true
};

export class Root {
  constructor() {
    this.out = new Sink();
    this.err = new Sink();
    this.owned = null;
    this.jobs = null;
    this.jobCounter = 0;
    this.signal = null;
    this.start = Date.now();
    this.arithDepth = 0;
    this.abort = null;
    this.aliases = null;
    this.disabled = null;
    this.funcDepth = 0;
  }
}

let cwdCache = { physical: null, pwd: null, logical: null };

function logicalCwd() {
  const physical = process.cwd();
  const pwd = process.env.PWD;
  if (cwdCache.physical === physical && cwdCache.pwd === pwd) return cwdCache.logical;
  let logical = physical;
  if (pwd && pwd !== physical && pwd.startsWith('/')) {
    try {
      const a = fs.statSync(pwd);
      const b = fs.statSync(physical);
      if (a.dev === b.dev && a.ino === b.ino) logical = pwd;
    } catch {}
  }
  cwdCache = { physical, pwd, logical };
  return logical;
}

function resolveCwd(dir) {
  const base = logicalCwd();
  const joined = dir.startsWith('/') || (process.platform === 'win32' && /^[A-Za-z]:[\\/]/.test(dir)) ? dir : `${base}/${dir}`;
  const parts = [];
  for (const part of joined.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..') parts.pop();
    else parts.push(part);
  }
  return '/' + parts.join('/');
}

export class Ctx {
  static top(options = null) {
    const root = new Root();
    const c = new Ctx();
    c.root = root;
    if (options && options.print) {
      root.out.mirror = process.stdout;
      root.err.mirror = process.stderr;
    }
    const env = options && options.env ? Object.fromEntries(Object.entries(options.env).filter(([, v]) => v !== undefined).map(([k, v]) => [k, String(v)])) : null;
    c.vars = new Vars(null, env);
    c.funcs = new Funcs(null);
    c.fds = [{ k: 'null' }, { k: 'cap', sink: root.out }, { k: 'cap', sink: root.err }, undefined, undefined, undefined, undefined, undefined, undefined, undefined];
    c.cwd = options && options.cwd ? resolveCwd(String(options.cwd)) : logicalCwd();
    c.pos = [];
    c.arg0 = 'ant';
    c.opts = Object.create(DEFAULT_OPTS);
    c.status = 0;
    c.ls = 0;
    c.ne = 0;
    c.post = null;
    c.ln = 0;
    c.umask = typeof process.umask === 'function' ? process.umask() : 0o022;
    c.fstack = [];
    c.flines = null;
    c.limits = null;
    c.traps = null;
    c.level = 0;
    c.dirs = null;
    c.lastBg = '';
    c.job = null;
    c.us = undefined;
    c.inTrap = false;
    c.aliasing = null;
    if (c.vars.env.POSIXLY_CORRECT !== undefined) c.opts.posix = true;
    if (c.vars.env.PWD !== c.cwd) {
      const pwd = new Var(c.cwd);
      pwd.x = true;
      c.vars.frames[0].set('PWD', pwd);
      c.vars.dirty = true;
    }
    return c;
  }

  subshell() {
    const c = new Ctx();
    c.root = this.root;
    c.vars = new Vars(this.vars);
    c.funcs = new Funcs(this.funcs);
    c.fds = this.fds.slice();
    c.cwd = this.cwd;
    c.pos = this.pos;
    c.arg0 = this.arg0;
    c.opts = Object.assign(Object.create(DEFAULT_OPTS), this.opts);
    c.status = this.status;
    c.ls = 0;
    c.ne = this.ne;
    c.post = null;
    c.ln = this.ln;
    c.umask = this.umask;
    c.fstack = this.fstack.slice();
    c.flines = this.flines ? this.flines.slice() : null;
    c.limits = this.limits ? new Map(this.limits) : null;
    c.traps = null;
    c.level = this.level + 1;
    c.dirs = this.dirs ? this.dirs.slice() : null;
    c.lastBg = this.lastBg;
    c.job = this.job;
    c.us = this.us;
    c.inTrap = false;
    c.aliasing = null;
    return c;
  }
}

function write(c, fd, data) {
  const e = c.fds[fd];
  if (!e) return false;
  if (e.k === 'cap') {
    e.sink.push(data);
    return true;
  }
  if (e.k === 'host') {
    if (e.pipe) {
      if (e.broken) throw new ShellExit(141);
      e.buf = e.buf ? e.buf + data : data;
      return e.buf.length >= 65536 ? flushEntry(e) : true;
    }
    return ops.write(e.fd, encodeOut(data)).then(r => r === 0);
  }
  return false;
}

function flushEntry(e) {
  const previous = e.flushing;
  const flush = async () => {
    if (previous) await previous;
    const data = e.buf;
    e.buf = null;
    if (!data) return true;
    if (e.broken) throw new ShellExit(141);
    const r = await ops.pwrite(e.fd, encodeOut(data));
    if (r === 0) return true;
    if (/broken pipe/i.test(r)) {
      e.broken = true;
      throw new ShellExit(141);
    }
    return false;
  };
  const promise = flush();
  e.flushing = promise.catch(() => {});
  return promise;
}

export function flushPipes(c) {
  let pending = null;
  const fds = c.fds;
  for (let fd = 1; fd < fds.length; fd++) {
    const e = fds[fd];
    if (e && e.pipe && e.buf) {
      const p = flushEntry(e);
      pending = pending ? pending.then(() => p) : p;
    }
  }
  return pending;
}

export function errorMessage(c, message) {
  const r = write(c, 2, `ant:shell: ${message}\n`);
  return r;
}

function ifsOf(c) {
  const v = c.vars.find('IFS');
  return v ? (v.k === 0 ? (v.v === undefined ? ' \t\n' : v.v) : String(v.a[0] ?? '')) : ' \t\n';
}

function joinStar(c, list) {
  const ifs = ifsOf(c);
  return list.join(ifs === '' ? '' : ifs[0]);
}

function scalarOf(v) {
  if (v.k === 0) return v.v;
  if (v.k === 1) return v.a[0];
  return v.a.get('0');
}

function bashHash(key) {
  let h = 2166136261;
  const bytes = toBytes(key);
  for (let i = 0; i < bytes.length; i++) {
    h = Math.imul(h, 16777619) >>> 0;
    h = (h ^ bytes[i]) >>> 0;
  }
  return h;
}

export function assocKeys(map) {
  let buckets = 1024;
  while (map.size >= buckets * 2) buckets *= 4;
  const items = [];
  let index = 0;
  for (const key of map.keys()) items.push({ key, bucket: bashHash(key) & (buckets - 1), index: index++ });
  items.sort((a, b) => a.bucket - b.bucket || b.index - a.index);
  return items.map(item => item.key);
}

function listOf(v) {
  if (!v) return [];
  if (v.k === 0) return v.v === undefined ? [] : [v.v];
  if (v.k === 1) {
    const out = [];
    v.a.forEach(x => out.push(x));
    return out;
  }
  return assocKeys(v.a).map(key => v.a.get(key));
}

function splitRef(name) {
  if (typeof name === 'string' && name.charCodeAt(name.length - 1) === 93) {
    const open = name.indexOf('[');
    if (open > 0) {
      const inner = name.slice(open + 1, -1);
      return [name.slice(0, open), inner === '@' ? AT : inner === '*' ? ST : inner];
    }
  }
  return null;
}

function special(c, name) {
  switch (name) {
    case '?': return String(c.status);
    case '#': return String(c.pos.length);
    case '$': return String(process.pid);
    case '!': return String(c.lastBg);
    case '0': return c.arg0;
    case '-': {
      let flags = '';
      for (const k of 'aefhnuvxBC') if (k === 'h' || k === 'B' || c.opts[k]) flags += k;
      return flags + 'c';
    }
  }
  return undefined;
}

function dynamicVar(c, name) {
  switch (name) {
    case 'RANDOM': return String(Math.floor(Math.random() * 32768));
    case 'SECONDS': return String(Math.floor((Date.now() - c.root.start) / 1000));
    case 'EPOCHSECONDS': return String(Math.floor(Date.now() / 1000));
    case 'EPOCHREALTIME': return (Date.now() / 1000).toFixed(6);
    case 'LINENO': return String(c.ln);
    case '_': return c.us === undefined ? '' : String(c.us);
    case 'BASH_COMMAND': return c.command ?? '';
    case 'BASHPID': return String(process.pid);
    case 'BASH_SUBSHELL': return String(c.level);
    case 'PPID': return String(process.ppid ?? '');
    case 'UID': case 'EUID': return typeof process.getuid === 'function' ? String(process.getuid()) : '0';
    case 'HOSTNAME': return os.hostname();
    case 'OSTYPE': return process.platform === 'darwin' ? 'darwin' : process.platform === 'linux' ? 'linux-gnu' : process.platform;
    case 'BASH_VERSION': return BASH_VERSION;
    case 'SHELLOPTS': return shellopts(c);
    case 'BASHOPTS': return bashopts(c);
  }
  return undefined;
}

const DYNAMIC = new Set(['_', 'BASH_COMMAND', 'BASH_VERSION', 'BASH_VERSINFO', 'SHELLOPTS', 'BASHOPTS', 'BASH_LINENO', 'RANDOM', 'SECONDS', 'EPOCHSECONDS', 'EPOCHREALTIME', 'LINENO', 'BASHPID', 'BASH_SUBSHELL', 'PPID', 'UID', 'EUID', 'HOSTNAME', 'OSTYPE', 'FUNCNAME']);

const BASH_VERSINFO = ['5', '2', '0', '1', 'release', process.platform];
const BASH_VERSION = '5.2.0(1)-release';

function dynamicList(c, name) {
  switch (name) {
    case 'FUNCNAME': return c.fstack.slice().reverse();
    case 'BASH_LINENO': return (c.flines ?? []).slice().reverse().map(String);
    case 'BASH_VERSINFO': return BASH_VERSINFO.slice();
  }
  return null;
}

function shellopts(c) {
  const names = { allexport: 'a', braceexpand: null, errexit: 'e', hashall: null, 'interactive-comments': null, noclobber: 'C', noexec: 'n', noglob: 'f', nounset: 'u', pipefail: 'pipefail', posix: 'posix', verbose: 'v', xtrace: 'x' };
  return Object.entries(names).filter(([, key]) => key === null || c.opts[key]).map(([name]) => name).join(':');
}

export const SHOPT_NAMES = [
  'checkwinsize', 'cmdhist', 'complete_fullquote', 'dotglob', 'expand_aliases', 'extglob', 'extquote', 'failglob',
  'force_fignore', 'globasciiranges', 'globskipdots', 'globstar', 'hostcomplete', 'inherit_errexit', 'interactive_comments',
  'lastpipe', 'nocaseglob', 'nocasematch', 'nullglob', 'patsub_replacement', 'progcomp', 'promptvars', 'shift_verbose',
  'sourcepath', 'xpg_echo'
];

function bashopts(c) {
  const names = SHOPT_NAMES;
  return names.filter(name => c.opts[name]).join(':');
}

function unbound(c, name) {
  throw new ShellError(`${name}: unbound variable`, 1, true);
}

function evalIndex(c, v, idx) {
  if (v && v.k === 2) return idx;
  const n = arithString(c, idx);
  if (n < 0 && v && v.k === 1) return v.a.length + n;
  return n;
}

export function get(c, name, idx) {
  if (typeof name !== 'string') {
    if (name instanceof Literal) return name.value;
    name = String(name);
  }
  if (name === '_') return c.us === undefined ? c.vars.env._ ?? '' : String(c.us);
  if (idx === undefined) {
    const ref = splitRef(name);
    if (ref) [name, idx] = ref;
  }
  const code = name.charCodeAt(0);
  if (!(code >= 65 && code <= 90) && !(code >= 97 && code <= 122) && code !== 95) {
    if (name === '@') return c.pos.slice();
    if (name === '*') return new Star(c.pos.slice());
    if (code >= 48 && code <= 57) {
      if (name === '0') return c.arg0;
      const value = c.pos[Number(name) - 1];
      if (value === undefined) {
        if (c.opts.u) unbound(c, name);
        return '';
      }
      return value;
    }
    const s = special(c, name);
    return s === undefined ? '' : s;
  }
  const v = c.vars.find(name);
  if (idx === AT || idx === ST) {
    if (!v) {
      const list = dynamicList(c, name);
      if (list) return idx === AT ? list : new Star(list);
    }
    const list = listOf(v);
    return idx === AT ? list : new Star(list);
  }
  if (!v) {
    if (DYNAMIC.has(name)) {
      const list = dynamicList(c, name);
      if (list) return list[idx === undefined ? 0 : arithString(c, idx)] ?? '';
      return dynamicVar(c, name);
    }
    if (c.opts.u) unbound(c, name);
    return '';
  }
  let value;
  if (idx === undefined) value = scalarOf(v);
  else if (v.k === 2) value = v.a.get(idx);
  else if (v.k === 1) value = v.a[evalIndex(c, v, idx)];
  else {
    const i = arithString(c, idx);
    value = i === 0 || i === -1 ? v.v : undefined;
  }
  if (value === undefined) {
    if (c.opts.u) unbound(c, idx === undefined ? name : `${name}[${idx}]`);
    return '';
  }
  return value;
}

export function isset(c, name, idx, colon) {
  if (typeof name !== 'string') {
    if (name instanceof Literal) return !colon || name.value !== '';
    name = String(name);
  }
  if (idx === undefined) {
    const ref = splitRef(name);
    if (ref) [name, idx] = ref;
  }
  const code = name.charCodeAt(0);
  if (!(code >= 65 && code <= 90) && !(code >= 97 && code <= 122) && code !== 95) {
    if (name === '@' || name === '*') return colon ? c.pos.length > 0 && c.pos.join('') !== '' : c.pos.length > 0;
    let value;
    if (code >= 48 && code <= 57) value = name === '0' ? c.arg0 : c.pos[Number(name) - 1];
    else value = special(c, name);
    return value !== undefined && (!colon || value !== '');
  }
  const v = c.vars.find(name);
  if (!v) {
    if (DYNAMIC.has(name)) return true;
    return false;
  }
  if (idx === AT || idx === ST) {
    const list = listOf(v);
    return colon ? list.length > 0 && list.join('') !== '' : list.length > 0;
  }
  let value;
  if (idx === undefined) value = scalarOf(v);
  else if (v.k === 2) value = v.a.get(idx);
  else if (v.k === 1) value = v.a[evalIndex(c, v, idx)];
  else value = arithString(c, idx) === 0 ? v.v : undefined;
  return value !== undefined && (!colon || value !== '');
}

function applyCase(v, value) {
  if (v.lc) return value.toLowerCase();
  if (v.uc) return value.toUpperCase();
  return value;
}

export function setVar(c, name, idx, value, append = false, target = null) {
  if (idx === undefined) {
    const ref = splitRef(name);
    if (ref) [name, idx] = ref;
  }
  const v = target || c.vars.writable(name);
  if (v.r) throw new ShellError(`${name}: readonly variable`);
  if (v.int) {
    let n = arithString(c, value);
    if (append) n = arithOps.add(arithString(c, scalarOf(v) ?? '0'), n);
    value = String(n);
    append = false;
  }
  value = applyCase(v, value);
  if (idx === undefined || idx === null) {
    if (v.k === 0) v.v = append ? (v.v ?? '') + value : value;
    else if (v.k === 1) v.a[0] = append ? (v.a[0] ?? '') + value : value;
    else v.a.set('0', append ? (v.a.get('0') ?? '') + value : value);
  } else {
    if (v.k === 0) {
      if (v.v !== undefined && v.k === 0) {
        v.a = [v.v];
      } else v.a = [];
      v.k = 1;
      v.v = undefined;
    }
    if (v.k === 2) v.a.set(idx, append ? (v.a.get(idx) ?? '') + value : value);
    else {
      const i = evalIndex(c, v, idx);
      if (i < 0) throw new ShellError(`${name}[${idx}]: bad array subscript`);
      v.a[i] = append ? (v.a[i] ?? '') + value : value;
    }
  }
  if (v.x || c.opts.a) {
    v.x = true;
    c.vars.dirty = true;
  }
  return value;
}

export function setArray(c, name, elements, append, target = null) {
  const v = target || c.vars.writable(name);
  if (v.r) throw new ShellError(`${name}: readonly variable`);
  const assoc = v.k === 2;
  if (!assoc) {
    let list = append && v.k === 1 ? v.a : append && v.k === 0 && v.v !== undefined ? [v.v] : [];
    let next = list.length;
    for (const [key, value, keyAppend] of elements) {
      if (key !== null) {
        next = evalIndex(c, { k: 1, a: list }, key);
        list[next] = keyAppend ? (list[next] ?? '') + applyCase(v, value) : applyCase(v, value);
      } else list[next] = applyCase(v, v.int ? String(arithString(c, value)) : value);
      next++;
    }
    v.k = 1;
    v.a = list;
    v.v = undefined;
  } else {
    const map = append ? v.a : new Map();
    for (const [key, value, keyAppend] of elements) {
      if (key === null) throw new ShellError(`${name}: ${value}: must use subscript when assigning associative array`);
      map.set(key, keyAppend ? (map.get(key) ?? '') + value : applyCase(v, value));
    }
    v.a = map;
  }
  if (c.opts.a) v.x = true;
  return v;
}

export function assignSpec(c, spec, target = null) {
  if (spec.a) setArray(c, spec.n, spec.a, spec.p, target);
  else setVar(c, spec.n, spec.i, spec.v, spec.p, target);
}

function arithString(c, text) {
  if (typeof text === 'number') return text;
  const s = String(text).trim();
  if (s === '') return 0;
  if (/^-?(?:0|[1-9][0-9]{0,14})$/.test(s)) return Number(s);
  try {
    return evalArith(A, c, parseCached(s));
  } catch (error) {
    if (error instanceof ArithError) throw new ShellError(`${s}: ${error.message}${error.token ? ` (error token is "${error.token}")` : ''}`);
    throw error;
  }
}

const parseCache = new Map();

function parseCached(text) {
  let ast = parseCache.get(text);
  if (!ast) {
    ast = parseArith(text);
    if (parseCache.size > 1024) parseCache.clear();
    parseCache.set(text, ast);
  }
  return ast;
}

function arithVar(c, value, name) {
  if (value === undefined || value === '') return 0;
  if (/^-?(?:0|[1-9][0-9]{0,14})$/.test(value)) return Number(value);
  const s = value.trim();
  if (/^(?:[0-9]+|0[xX][0-9a-fA-F]+|[0-9]+#[0-9A-Za-z@_]+)$/.test(s)) {
    try {
      return parseNumber(s);
    } catch (error) {
      if (error instanceof ArithError) throw new ShellError(`${s}: ${error.message} (error token is "${s}")`);
      throw error;
    }
  }
  const root = c.root;
  if (++root.arithDepth > 1024) {
    root.arithDepth = 0;
    throw new ShellError(`${name}: expression recursion level exceeded`);
  }
  try {
    return arithString(c, s);
  } finally {
    root.arithDepth--;
  }
}

export const A = {
  ...arithOps,
  v(c, name) {
    const v = c.vars.find(name);
    if (!v) {
      if (DYNAMIC.has(name)) return arithVar(c, dynamicVar(c, name), name);
      if (c.opts.u) unbound(c, name);
      return 0;
    }
    return arithVar(c, scalarOf(v), name);
  },
  ve(c, name, idx) {
    const v = c.vars.find(name);
    if (!v) {
      if (c.opts.u) unbound(c, name);
      return 0;
    }
    const key = v.k === 2 ? String(idx) : idx;
    const value = v.k === 2 ? v.a.get(key) : v.k === 1 ? v.a[idx < 0 ? v.a.length + idx : idx] : idx === 0 ? v.v : undefined;
    return arithVar(c, value, name);
  },
  s(c, name, value) {
    setVar(c, name, undefined, String(value));
    return value;
  },
  se(c, name, idx, value) {
    const v = c.vars.find(name);
    setVar(c, name, v && v.k === 2 ? String(idx) : String(idx), String(value));
    return value;
  },
  isA(c, name) {
    const v = c.vars.find(name);
    return v !== null && v.k === 2;
  },
  vs(c, name, key) {
    const v = c.vars.find(name);
    return arithVar(c, v.a.get(key), name);
  },
  ss(c, name, key, value) {
    setVar(c, name, key, String(value));
    return value;
  },
  incs(c, name, key, delta, post) {
    const old = A.vs(c, name, key);
    const value = arithOps.add(old, delta);
    setVar(c, name, key, String(value));
    return post ? old : value;
  },
  bad(key) {
    throw new ShellError(`${key}: syntax error: operand expected`);
  },
  inc(c, name, delta, post) {
    const old = A.v(c, name);
    const value = arithOps.add(old, delta);
    setVar(c, name, undefined, String(value));
    return post ? old : value;
  },
  ince(c, name, idx, delta, post) {
    const old = A.ve(c, name, idx);
    const value = arithOps.add(old, delta);
    A.se(c, name, idx, value);
    return post ? old : value;
  }
};

function pushText(state, text, quoted, glob) {
  state.cur += text;
  state.pat += quoted ? escapeGlob(text) : text;
  if (glob && !quoted && !state.glob && /[*?[]|[+@!]\(/.test(text)) state.glob = true;
  if (text || quoted) state.started = true;
  state.delim = 0;
}

function endField(state) {
  state.fields.push({ text: state.cur, pat: state.glob ? state.pat : null });
  state.cur = '';
  state.pat = '';
  state.glob = false;
  state.started = false;
}

function splitInto(state, text, ifs) {
  if (ifs === '') {
    pushText(state, text, false, true);
    return;
  }
  let run = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ifs.includes(ch)) {
      if (run) {
        pushText(state, run, false, true);
        run = '';
      }
      const ws = ch === ' ' || ch === '\t' || ch === '\n';
      if (ws) {
        if (state.started) {
          endField(state);
          state.delim = 1;
        }
      } else {
        if (state.started) endField(state);
        else if (state.delim !== 1) state.fields.push({ text: '', pat: null });
        state.delim = 2;
      }
      continue;
    }
    run += ch;
  }
  if (run) pushText(state, run, false, true);
}

export function fields(c, segs, out) {
  if (segs.length === 2 && segs[0] === 4) {
    pushInterp(out, segs[1]);
    return out;
  }
  const state = { cur: '', pat: '', glob: false, started: false, delim: 0, fields: [] };
  let ifs = null;
  for (let i = 0; i < segs.length; i += 2) {
    const flag = segs[i];
    let value = segs[i + 1];
    if (flag === 0) {
      pushText(state, value, false, true);
      continue;
    }
    if (flag === 1 || flag === 4) {
      pushText(state, typeof value === 'string' ? value : String(value), true, false);
      continue;
    }
    if (flag === 3) {
      if (value instanceof Star) value = joinStar(c, value.list);
      if (Array.isArray(value)) {
        for (let j = 0; j < value.length; j++) {
          if (j > 0) endField(state);
          pushText(state, value[j], true, false);
        }
        continue;
      }
      pushText(state, value, true, false);
      continue;
    }
    if (ifs === null) ifs = ifsOf(c);
    if (value instanceof Star) value = value.list;
    if (Array.isArray(value)) {
      for (let j = 0; j < value.length; j++) {
        if (j > 0 && state.started) endField(state);
        splitInto(state, value[j], ifs);
      }
      continue;
    }
    splitInto(state, value, ifs);
  }
  if (state.started) endField(state);
  const noglob = c.opts.f;
  for (const field of state.fields) {
    if (field.pat === null || noglob || !hasGlob(field.pat, c.opts.extglob)) {
      out.push(field.text);
      continue;
    }
    const ignore = isset(c, 'GLOBIGNORE', undefined, true) ? get(c, 'GLOBIGNORE').split(':').filter(Boolean) : null;
    let matches = globExpand(c.cwd, field.pat, { dotglob: c.opts.dotglob || !!ignore, nocase: c.opts.nocaseglob, extglob: c.opts.extglob, globstar: c.opts.globstar });
    if (ignore) matches = matches.filter(m => !ignore.some(p => matchPattern(m, p, false, true)) && !/(^|\/)\.\.?$/.test(m));
    if (matches.length) {
      for (const m of matches) out.push(m);
    } else if (c.opts.failglob) {
      throw new ShellError(`no match: ${field.text}`);
    } else if (!c.opts.nullglob) {
      out.push(unescapeGlob(field.pat));
    }
  }
  return out;
}

export function pushInterp(out, value) {
  if (Array.isArray(value)) {
    for (const item of value) out.push(String(item));
  } else out.push(String(value));
}

export function str(c, segs) {
  if (segs.length === 2) {
    const v = segs[1];
    if (typeof v === 'string') return v;
  }
  let text = '';
  for (let i = 0; i < segs.length; i += 2) {
    let value = segs[i + 1];
    if (value instanceof Star) value = joinStar(c, value.list);
    else if (Array.isArray(value)) value = segs[i] === 4 || segs[i] === 1 ? String(value) : value.join(' ');
    else if (typeof value !== 'string') value = String(value);
    text += value;
  }
  return text;
}

export function pat(c, segs) {
  let text = '';
  for (let i = 0; i < segs.length; i += 2) {
    const flag = segs[i];
    let value = segs[i + 1];
    if (value instanceof Star) value = joinStar(c, value.list);
    else if (Array.isArray(value)) value = flag === 1 || flag === 4 ? String(value) : value.join(' ');
    else if (typeof value !== 'string') value = String(value);
    text += flag === 0 || flag === 2 ? value : escapeGlob(value);
  }
  return text;
}

function escapeRegex(text) {
  return text.replace(/[\\^$.*+?()[\]{}|/]/g, '\\$&');
}

export function re(c, segs) {
  let text = '';
  for (let i = 0; i < segs.length; i += 2) {
    const flag = segs[i];
    let value = segs[i + 1];
    if (value instanceof Star) value = joinStar(c, value.list);
    else if (Array.isArray(value)) value = value.join(' ');
    else if (typeof value !== 'string') value = String(value);
    text += flag === 0 || flag === 2 ? value : escapeRegex(value);
  }
  return text;
}

function mapValue(value, fn) {
  if (value instanceof Star) return new Star(value.list.map(fn));
  if (Array.isArray(value)) return value.map(fn);
  return fn(value);
}

function codepoints(s) {
  return /^[\x00-\x7f]*$/.test(s) ? null : Array.from(s);
}

export function byteLocale(c) {
  for (const name of ['LC_ALL', 'LC_CTYPE', 'LANG']) {
    const v = c.vars.find(name);
    const value = v ? scalarOf(v) : undefined;
    if (value) return !/utf-?8/i.test(value);
  }
  return true;
}

export function units(c, s) {
  if (/^[\x00-\x7f]*$/.test(s)) return null;
  if (!byteLocale(c)) return Array.from(s);
  return Array.from(toBytes(s), rawByte);
}

function trimOne(s, op, pattern, nocase, extglob) {
  if (!hasGlob(pattern, extglob)) {
    const lit = unescapeGlob(pattern);
    if (op[0] === '#') return s.startsWith(lit) ? s.slice(lit.length) : s;
    return s.endsWith(lit) && lit ? s.slice(0, s.length - lit.length) : s;
  }
  const re = compilePattern(pattern, nocase, extglob);
  const chars = codepoints(s);
  const len = chars ? chars.length : s.length;
  const slice = (a, b) => (chars ? chars.slice(a, b).join('') : s.slice(a, b));
  if (op === '#') {
    for (let i = 0; i <= len; i++) if (re.test(slice(0, i))) return slice(i, len);
  } else if (op === '##') {
    for (let i = len; i >= 0; i--) if (re.test(slice(0, i))) return slice(i, len);
  } else if (op === '%') {
    for (let i = len; i >= 0; i--) if (re.test(slice(i, len))) return slice(0, i);
  } else {
    for (let i = 0; i <= len; i++) if (re.test(slice(i, len))) return slice(0, i);
  }
  return s;
}

export function trim(c, value, op, pattern) {
  return mapValue(value, s => trimOne(s, op, pattern, c.opts.nocasematch, c.opts.extglob));
}

function replaceOne(s, op, pattern, rep, nocase, extglob) {
  if (pattern === '' && op !== '/#' && op !== '/%') return s;
  const chars = codepoints(s);
  const len = chars ? chars.length : s.length;
  const slice = (a, b) => (chars ? chars.slice(a, b).join('') : s.slice(a, b));
  if (!hasGlob(pattern, extglob) && !nocase) {
    const lit = unescapeGlob(pattern);
    if (op === '/#') return s.startsWith(lit) ? rep + s.slice(lit.length) : s;
    if (op === '/%') return s.endsWith(lit) ? s.slice(0, s.length - lit.length) + rep : s;
    if (op === '//') return s.split(lit).join(rep);
    const at = s.indexOf(lit);
    return at === -1 ? s : s.slice(0, at) + rep + s.slice(at + lit.length);
  }
  const re = compilePattern(pattern, nocase, extglob);
  if (op === '/#') {
    for (let i = len; i >= 0; i--) if (re.test(slice(0, i))) return rep + slice(i, len);
    return s;
  }
  if (op === '/%') {
    for (let i = 0; i <= len; i++) if (re.test(slice(i, len))) return slice(0, i) + rep;
    return s;
  }
  let out = '';
  let start = 0;
  let i = 0;
  while (i <= len) {
    let matched = -1;
    for (let j = len; j >= i; j--) {
      if (re.test(slice(i, j))) {
        matched = j;
        break;
      }
    }
    if (matched === -1 || (matched === i && pattern !== '')) {
      if (matched === i) {
        i++;
        continue;
      }
      i++;
      continue;
    }
    out += slice(start, i) + rep;
    start = matched;
    i = matched === i ? i + 1 : matched;
    if (op === '/') break;
  }
  return out + slice(start, len);
}

export function repl(c, value, op, pattern, rep) {
  return mapValue(value, s => replaceOne(s, op, pattern, rep, c.opts.nocasematch, c.opts.extglob));
}

export function cased(c, value, op, pattern) {
  const ascii = byteLocale(c);
  const test = ch => (!ascii || ch.charCodeAt(0) < 128) && (pattern === null || matchPattern(ch, pattern, false, true));
  return mapValue(value, s => {
    const chars = Array.from(s);
    const upper = op[0] === '^';
    const all = op.length === 2;
    for (let i = 0; i < chars.length; i++) {
      if (test(chars[i])) chars[i] = upper ? chars[i].toUpperCase() : chars[i].toLowerCase();
      if (!all) break;
    }
    return chars.join('');
  });
}

export function substr(c, name, idx, offset, length) {
  if (typeof name !== 'string') name = String(name);
  if (idx === undefined) {
    const ref = splitRef(name);
    if (ref) [name, idx] = ref;
  }
  if (name === '@' || name === '*' || idx === AT || idx === ST) {
    let list;
    let base = 0;
    if (name === '@' || name === '*') {
      list = [c.arg0, ...c.pos];
      if (offset < 0) base = 1;
    } else {
      const v = c.vars.find(name);
      if (v && v.k === 1) {
        const keys = [];
        v.a.forEach((x, i) => keys.push(i));
        let startIndex = offset < 0 ? (keys.length ? keys[keys.length - 1] + 1 + offset : 0) : offset;
        const out = [];
        for (const k of keys) if (k >= startIndex) out.push(v.a[k]);
        const sliced = length === undefined ? out : length < 0 ? out.slice(0, Math.max(0, out.length + length)) : out.slice(0, length);
        return name === '*' || idx === ST ? new Star(sliced) : sliced;
      }
      list = listOf(v);
    }
    let start = offset < 0 ? list.length + offset : offset;
    if (start < base) start = offset < 0 ? list.length + 1 : base;
    let end = length === undefined ? list.length : length < 0 ? list.length + length : start + length;
    if (length < 0 && end < start) throw new ShellError(`${length}: substring expression < 0`);
    const sliced = list.slice(start, end);
    return name === '*' || idx === ST ? new Star(sliced) : sliced;
  }
  const value = get(c, name, idx);
  const chars = units(c, value);
  const len = chars ? chars.length : value.length;
  let start = offset < 0 ? len + offset : offset;
  if (start < 0 || start > len) return '';
  let end = length === undefined ? len : length < 0 ? len + length : start + length;
  if (end < start) {
    if (length < 0) throw new ShellError(`${length}: substring expression < 0`);
    return '';
  }
  return chars ? chars.slice(start, end).join('') : value.slice(start, end);
}

export function len(c, name, idx) {
  if (typeof name !== 'string') name = String(name);
  if (idx === undefined) {
    const ref = splitRef(name);
    if (ref) [name, idx] = ref;
  }
  if (name === '@' || name === '*') return String(c.pos.length);
  if (idx === AT || idx === ST) {
    const v = c.vars.find(name);
    if (!v && dynamicList(c, name)) return String(dynamicList(c, name).length);
    return String(listOf(v).length);
  }
  const value = get(c, name, idx);
  return String(units(c, value)?.length ?? value.length);
}

export function shellQuote(value) {
  if (value === '') return "''";
  if (/[\x00-\x1f\x7f]/.test(value)) {
    let out = "$'";
    for (const ch of value) {
      const code = ch.charCodeAt(0);
      if (ch === "'") out += "\\'";
      else if (ch === '\\') out += '\\\\';
      else if (ch === '\n') out += '\\n';
      else if (ch === '\t') out += '\\t';
      else if (ch === '\r') out += '\\r';
      else if (ch === '\x1b') out += '\\E';
      else if (code < 32 || code === 127) out += '\\' + code.toString(8).padStart(3, '0');
      else out += ch;
    }
    return out + "'";
  }
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

export function attrFlags(v) {
  if (!v) return '';
  let flags = '';
  if (v.k === 1) flags += 'a';
  if (v.k === 2) flags += 'A';
  if (v.int) flags += 'i';
  if (v.lc) flags += 'l';
  if (v.nref) flags += 'n';
  if (v.r) flags += 'r';
  if (v.uc) flags += 'u';
  if (v.x) flags += 'x';
  return flags;
}

export function xform(c, name, idx, op) {
  const value = get(c, name, idx);
  switch (op) {
    case 'Q': return mapValue(value, shellQuote);
    case 'E': return mapValue(value, decodeEscapes);
    case 'P': return mapValue(value, text => promptExpand(c, text));
    case 'U': return mapValue(value, s => s.toUpperCase());
    case 'L': return mapValue(value, s => s.toLowerCase());
    case 'u': return mapValue(value, s => s.charAt(0).toUpperCase() + s.slice(1));
    case 'a': return attrFlags(c.vars.find(typeof name === 'string' ? name : String(name)));
    case 'A': {
      const v = c.vars.find(name);
      if (!v) return '';
      if (v.k === 0) return `${name}=${shellQuote(v.v ?? '')}`;
      return declareLine(name, v);
    }
    case 'K': {
      const v = c.vars.find(name);
      if (!v || v.k === 0) return mapValue(value, shellQuote);
      const out = [];
      if (v.k === 1) v.a.forEach((x, i) => out.push(`${i} ${shellQuote(x)}`));
      else for (const k of assocKeys(v.a)) out.push(`${k} ${shellQuote(v.a.get(k))}`);
      return out.join(' ');
    }
  }
  return value;
}

function promptExpand(c, text) {
  const now = new Date();
  const two = n => String(n).padStart(2, '0');
  const home = isset(c, 'HOME', undefined, true) ? get(c, 'HOME') : '';
  const cwd = c.cwd;
  const short = home && (cwd === home || cwd.startsWith(home + '/')) ? '~' + cwd.slice(home.length) : cwd;
  let user = '';
  try {
    user = os.userInfo().username;
  } catch {}
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch !== '\\' || i + 1 >= text.length) {
      out += ch;
      continue;
    }
    const n = text[++i];
    switch (n) {
      case 'a': out += '\x07'; break;
      case 'e': out += '\x1b'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 'h': out += os.hostname().split('.')[0]; break;
      case 'H': out += os.hostname(); break;
      case 'u': out += user; break;
      case 'w': out += short; break;
      case 'W': out += short === '~' ? '~' : short.split('/').pop() || '/'; break;
      case 's': out += 'ant'; break;
      case 'v': out += '5.2'; break;
      case 'V': out += '5.2.0'; break;
      case 'j': out += String(c.root.jobs ? c.root.jobs.size : 0); break;
      case 't': out += `${two(now.getHours())}:${two(now.getMinutes())}:${two(now.getSeconds())}`; break;
      case 'T': out += `${two(now.getHours() % 12 || 12)}:${two(now.getMinutes())}:${two(now.getSeconds())}`; break;
      case 'A': out += `${two(now.getHours())}:${two(now.getMinutes())}`; break;
      case '@': out += `${two(now.getHours() % 12 || 12)}:${two(now.getMinutes())} ${now.getHours() < 12 ? 'AM' : 'PM'}`; break;
      case 'd': out += now.toDateString().slice(0, 10); break;
      case '$': out += typeof process.geteuid === 'function' && process.geteuid() === 0 ? '#' : '$'; break;
      case '!': case '#': out += '1'; break;
      case '[': case ']': break;
      case '\\': out += '\\'; break;
      default:
        if (n >= '0' && n <= '7') {
          const m = /^[0-7]{1,3}/.exec(text.slice(i));
          out += rawByte(parseInt(m[0], 8));
          i += m[0].length - 1;
        } else out += '\\' + n;
    }
  }
  return out;
}

export function declareLine(name, v) {
  const flags = attrFlags(v) || '-';
  const prefix = `declare -${flags.replace(/^-$/, '-')} `;
  if (v.k === 1) {
    const items = [];
    v.a.forEach((x, i) => items.push(`[${i}]=${dquote(x)}`));
    return `${prefix}${name}=(${items.join(' ')})`;
  }
  if (v.k === 2) {
    const items = [];
    for (const k of assocKeys(v.a)) items.push(`[${shellWordKey(k)}]=${dquote(v.a.get(k))}`);
    return `${prefix}${name}=(${items.join(' ')}${items.length ? ' ' : ''})`;
  }
  if (v.v === undefined) return `${prefix}${name}`;
  return `${prefix}${name}=${dquote(v.v)}`;
}

function shellWordKey(key) {
  return /^[A-Za-z0-9_.-]+$/.test(key) ? key : dquote(key);
}

function dquote(value) {
  return `"${value.replace(/(["\\$`])/g, '\\$1')}"`;
}

export function decodeEscapes(text) {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch !== '\\' || i + 1 >= text.length) {
      out += ch;
      continue;
    }
    const n = text[++i];
    switch (n) {
      case 'a': out += '\x07'; break;
      case 'b': out += '\b'; break;
      case 'e': case 'E': out += '\x1b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case "'": out += "'"; break;
      case '"': out += '"'; break;
      case 'x': {
        const m = /^[0-9a-fA-F]{1,2}/.exec(text.slice(i + 1));
        if (!m) out += '\\x';
        else {
          out += rawByte(parseInt(m[0], 16));
          i += m[0].length;
        }
        break;
      }
      default:
        if (n >= '0' && n <= '7') {
          const m = /^[0-7]{1,3}/.exec(text.slice(i));
          out += rawByte(parseInt(m[0], 8));
          i += m[0].length - 1;
        } else out += '\\' + n;
    }
  }
  return out;
}

class Literal {
  constructor(value) {
    this.value = value;
  }

  toString() {
    return this.value;
  }
}

export function ind(c, name) {
  const raw = c.vars.findRaw(name);
  if (raw && raw.nref) return new Literal(raw.v ?? '');
  const target = get(c, name, undefined);
  if (target === '') {
    if (!isset(c, name, undefined, false)) throw new ShellError(`${name}: invalid indirect expansion`);
  }
  if (!/^([A-Za-z_][A-Za-z0-9_]*(\[.*\])?|[0-9]+|[@*#?$!-])$/.test(target)) throw new ShellError(`${target}: invalid variable name`);
  return target;
}

export function names(c, prefix, mode) {
  const list = c.vars.names().filter(name => name.startsWith(prefix));
  return mode === '@' ? list : new Star(list);
}

export function keys(c, name, idx) {
  const v = c.vars.find(name);
  let list = [];
  if (v) {
    if (v.k === 1) v.a.forEach((x, i) => list.push(String(i)));
    else if (v.k === 2) list = assocKeys(v.a);
    else if (v.v !== undefined) list = ['0'];
  }
  return idx === ST ? new Star(list) : list;
}

export function setp(c, name, idx, value) {
  if (typeof name !== 'string') name = String(name);
  if (!/^[A-Za-z_]/.test(name)) throw new ShellError(`$${name}: cannot assign in this way`);
  setVar(c, name, idx === AT || idx === ST ? undefined : idx, value);
  return value;
}

export function perr(c, name, message, colon) {
  throw new ShellError(`${name}: ${message || (colon ? 'parameter null or not set' : 'parameter not set')}`, 1, true);
}

export function tilde(c, user) {
  if (user === '') {
    const home = c.vars.find('HOME');
    if (home && scalarOf(home) !== undefined) return scalarOf(home);
    return os.homedir();
  }
  if (user === '+') return get(c, 'PWD');
  if (user === '-') return isset(c, 'OLDPWD', undefined, false) ? get(c, 'OLDPWD') : '~-';
  try {
    const info = os.userInfo();
    if (info.username === user) return info.homedir;
  } catch {}
  return '~' + user;
}

export function arith(c, text) {
  return arithString(c, text);
}

export function arithFail(c, text, message, token) {
  throw new ShellError(`${text}: ${message}${token ? ` (error token is "${token}")` : ''}`);
}

export function resolvePath(c, path) {
  if (path.startsWith('/')) return path;
  if (process.platform === 'win32' && /^[A-Za-z]:[\\/]/.test(path)) return path;
  return c.cwd.endsWith('/') ? c.cwd + path : c.cwd + '/' + path;
}

const DEV_FD = /^\/dev\/(?:fd\/([0-9]{1,3})|(stdin)|(stdout)|(stderr))$/;

function devFd(path) {
  const m = DEV_FD.exec(path);
  if (!m) return -1;
  if (m[1] !== undefined) return Number(m[1]);
  return m[2] ? 0 : m[3] ? 1 : 2;
}

function errnoText(error) {
  switch (error && error.code) {
    case 'ENOENT': return 'No such file or directory';
    case 'EACCES': return 'Permission denied';
    case 'EISDIR': return 'Is a directory';
    case 'ENOTDIR': return 'Not a directory';
    case 'EEXIST': return 'File exists';
  }
  return error && error.message ? error.message : String(error);
}

function capitalize(text) {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

function isRegularFile(path) {
  try {
    return fs.statSync(path).isFile();
  } catch {
    return false;
  }
}

function binaryTarget(value) {
  return value instanceof Uint8Array || value instanceof ArrayBuffer || ArrayBuffer.isView(value) ||
    (typeof Blob !== 'undefined' && value instanceof Blob) || (typeof Response !== 'undefined' && value instanceof Response);
}

export function RT(c, segs) {
  if (segs.length === 2 && segs[0] === 4 && binaryTarget(segs[1])) return segs[1];
  const out = fields(c, segs, []);
  if (out.length !== 1) {
    errorMessage(c, `${str(c, segs) || '""'}: ambiguous redirect`);
    return null;
  }
  return out[0];
}

async function readBytes(path) {
  return fs.promises.readFile(path);
}

function processUmask() {
  return typeof process.umask === 'function' ? process.umask() : 0o022;
}

function limitsArg(c) {
  const list = [];
  for (const [index, [soft]] of c.limits) list.push(index, soft);
  return list;
}

export function umaskArg(c) {
  return c.umask === processUmask() ? undefined : c.umask;
}

function redirFd(c, fdArg, name, fallback) {
  if (name) {
    const fd = allocFd(c);
    setVar(c, name, undefined, String(fd));
    return fd;
  }
  return fdArg < 0 ? fallback : fdArg;
}

export async function redir(c, redirs) {
  const post = takePost(c);
  const saved = c.fds.slice();
  const owned = [];
  const closed = [];
  const fills = [];
  const fail = async message => {
    for (const fd of owned) ops.close(fd);
    c.fds = saved;
    if (post) await runPost(post);
    if (message) await errorMessage(c, message);
    c.status = 1;
    return null;
  };
  for (const [op, fdArg, target, name] of redirs) {
    if (target === null) return fail(null);
    switch (op) {
      case '<': {
        const fd = redirFd(c, fdArg, name, 0);
        if (typeof target === 'object') {
          c.fds[fd] = { k: 'bytes', data: await binaryBytes(target), pos: 0 };
          break;
        }
        const dev = devFd(target);
        if (dev >= 0) {
          if (!c.fds[dev]) return fail(`${dev}: Bad file descriptor`);
          c.fds[fd] = c.fds[dev];
          break;
        }
        try {
          c.fds[fd] = { k: 'bytes', data: await readBytes(resolvePath(c, target)), pos: 0 };
        } catch (error) {
          return fail(`${target}: ${errnoText(error)}`);
        }
        break;
      }
      case '>':
      case '>|':
      case '>>':
      case '<>':
      case '&>':
      case '&>>': {
        const both = op === '&>' || op === '&>>';
        const fd = both ? 1 : redirFd(c, fdArg, name, op === '<>' ? 0 : 1);
        if (typeof target === 'object') {
          if (!(target instanceof Uint8Array || target instanceof ArrayBuffer || ArrayBuffer.isView(target))) return fail('redirection target must be a buffer');
          const sink = new Sink();
          const entry = { k: 'cap', sink };
          c.fds[fd] = entry;
          if (both) c.fds[2] = entry;
          fills.push({ sink, target });
          break;
        }
        const dev = devFd(target);
        if (dev >= 0) {
          if (!c.fds[dev]) return fail(`${dev}: Bad file descriptor`);
          c.fds[fd] = c.fds[dev];
          if (both) c.fds[2] = c.fds[dev];
          break;
        }
        const path = resolvePath(c, target);
        let kind = op === '>>' || op === '&>>' ? FD_OPEN_APPEND : op === '<>' ? FD_OPEN_READ_WRITE : FD_OPEN_WRITE;
        if ((op === '>' || op === '&>') && c.opts.C && isRegularFile(path)) return fail(`${target}: cannot overwrite existing file`);
        const opened = await ops.open(path, kind, umaskArg(c));
        if (typeof opened === 'string') return fail(`${target}: ${capitalize(opened)}`);
        owned.push(opened);
        const entry = { k: 'host', fd: opened };
        c.fds[fd] = entry;
        if (both) c.fds[2] = entry;
        break;
      }
      case '>&':
      case '<&': {
        let fd;
        if (name && target === '-') {
          const value = get(c, name);
          if (!/^[0-9]+$/.test(value)) return fail(`${name}: Bad file descriptor`);
          fd = Number(value);
        } else fd = redirFd(c, fdArg, name, op === '>&' ? 1 : 0);
        if (target === '-') {
          if (c.fds[fd]) closed.push(c.fds[fd]);
          c.fds[fd] = null;
          break;
        }
        if (/^[0-9]+$/.test(target)) {
          const src = Number(target);
          if (src > 255 || !c.fds[src]) return fail(`${target}: Bad file descriptor`);
          c.fds[fd] = c.fds[src];
          break;
        }
        if (op === '>&' && fdArg < 0 && !name) {
          const path = resolvePath(c, target);
          const opened = await ops.open(path, FD_OPEN_WRITE, umaskArg(c));
          if (typeof opened === 'string') return fail(`${target}: ${capitalize(opened)}`);
          owned.push(opened);
          const entry = { k: 'host', fd: opened };
          c.fds[1] = entry;
          c.fds[2] = entry;
          break;
        }
        return fail(`${target}: ambiguous redirect`);
      }
      case '<<':
      case '<<<': {
        const fd = redirFd(c, fdArg, name, 0);
        c.fds[fd] = { k: 'bytes', data: toBytes(target), pos: 0 };
        break;
      }
    }
  }
  return { saved, owned, closed, post, fills };
}

export function needsShellRedirs(redirs) {
  for (const [op, fd, target, name] of redirs) {
    if (name || fd > 9 || (target !== null && typeof target === 'object')) return true;
    if ((op === '>&' || op === '<&') && /^[0-9]+$/.test(target) && Number(target) > 9) return true;
    if (devFd(target) > 9) return true;
  }
  return false;
}

async function makePermanent(c, token) {
  const owned = (c.root.owned ??= new Set());
  for (const fd of token.owned) owned.add(fd);
  for (let i = 0; i < c.fds.length; i++) {
    const e = c.fds[i];
    if (e && e.k === 'host' && token.owned.includes(e.fd)) e.rootOwned = true;
  }
  for (const e of token.closed) {
    if (!e.rootOwned || c.fds.includes(e)) continue;
    if (e.pipe && e.buf) {
      try {
        await flushEntry(e);
      } catch {}
    }
    ops.close(e.fd);
    owned.delete(e.fd);
  }
}

async function binaryBytes(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  return new Uint8Array(await value.arrayBuffer());
}

export async function unredir(c, token) {
  for (const fd of token.owned) ops.close(fd);
  c.fds = token.saved;
  if (token.fills) {
    for (const { sink, target } of token.fills) {
      const bytes = sink.bytes();
      const view = target instanceof ArrayBuffer ? new Uint8Array(target) : new Uint8Array(target.buffer, target.byteOffset, target.byteLength);
      view.set(bytes.subarray(0, view.length));
    }
  }
  if (token.post) {
    const status = c.status;
    await runPost(token.post);
    c.status = status;
  }
}

function stageRedirs(plan, stage, c, redirs) {
  for (const [op, fdArg, target] of redirs) {
    if (target === null) return false;
    if (typeof target === 'object') {
      if (op === '<' && (target instanceof ArrayBuffer || ArrayBuffer.isView(target))) {
        const bytes = target instanceof ArrayBuffer ? new Uint8Array(target) : new Uint8Array(target.buffer, target.byteOffset, target.byteLength);
        ops.fd(plan, stage, FD_BYTES, fdArg < 0 ? 0 : fdArg, -1, bytes);
        continue;
      }
      errorMessage(c, 'buffer redirections inside a pipeline require a buffer source on stdin');
      return false;
    }
    switch (op) {
      case '<': {
        const fd = fdArg < 0 ? 0 : fdArg;
        const dev = devFd(target);
        if (dev >= 0) ops.fd(plan, stage, FD_DUP, fd, dev);
        else ops.fd(plan, stage, FD_OPEN_READ, fd, -1, resolvePath(c, target));
        break;
      }
      case '>':
      case '>|':
      case '>>':
      case '<>':
      case '&>':
      case '&>>': {
        const both = op === '&>' || op === '&>>';
        const fd = both ? 1 : fdArg < 0 ? (op === '<>' ? 0 : 1) : fdArg;
        const dev = devFd(target);
        if (dev >= 0) {
          ops.fd(plan, stage, FD_DUP, fd, dev);
        } else {
          const path = resolvePath(c, target);
          let kind = op === '>>' || op === '&>>' ? FD_OPEN_APPEND : op === '<>' ? FD_OPEN_READ_WRITE : FD_OPEN_WRITE;
          if ((op === '>' || op === '&>') && c.opts.C && isRegularFile(path)) {
            errorMessage(c, `${target}: cannot overwrite existing file`);
            return false;
          }
          ops.fd(plan, stage, kind, fd, -1, path);
        }
        if (both) ops.fd(plan, stage, FD_DUP, 2, 1);
        break;
      }
      case '>&':
      case '<&': {
        const fd = fdArg < 0 ? (op === '>&' ? 1 : 0) : fdArg;
        if (target === '-') ops.fd(plan, stage, FD_CLOSE, fd, -1);
        else if (/^[0-9]$/.test(target)) ops.fd(plan, stage, FD_DUP, fd, Number(target));
        else if (op === '>&' && fdArg < 0) {
          ops.fd(plan, stage, FD_OPEN_WRITE, 1, -1, resolvePath(c, target));
          ops.fd(plan, stage, FD_DUP, 2, 1);
        } else {
          errorMessage(c, `${target}: ${/^[0-9]+$/.test(target) ? 'Bad file descriptor' : 'ambiguous redirect'}`);
          return false;
        }
        break;
      }
      case '<<':
      case '<<<':
        ops.fd(plan, stage, FD_BYTES, fdArg < 0 ? 0 : fdArg, -1, encodeOut(target));
        break;
    }
  }
  return true;
}

class Captures {
  constructor(plan) {
    this.plan = plan;
    this.map = new Map();
  }

  index(sink) {
    let i = this.map.get(sink);
    if (i === undefined) {
      i = ops.capture(this.plan);
      this.map.set(sink, i);
    }
    return i;
  }

  deliver(result) {
    for (const [sink, i] of this.map) {
      const bytes = result.captures[i];
      if (bytes && bytes.length) sink.push(bytes);
    }
  }
}

function stageFds(plan, stage, s, c, caps) {
  for (let fd = 3; fd < 10; fd++) {
    const e = s.fds[fd];
    if (e === c.fds[fd]) continue;
    if (!e) ops.fd(plan, stage, FD_CLOSE, fd, -1);
    else entryFd(plan, stage, fd, e, caps);
  }
}

function baseFds(plan, fds, caps) {
  for (let fd = 0; fd < 10; fd++) {
    const e = fds[fd];
    if (e) entryFd(plan, -1, fd, e, caps);
  }
}

function entryFd(plan, stage, fd, e, caps) {
  switch (e.k) {
    case 'cap':
      ops.fd(plan, stage, FD_CAPTURE, fd, caps.index(e.sink));
      break;
    case 'host':
      ops.fd(plan, stage, FD_HOST, fd, e.fd);
      break;
    case 'stream':
      if (e.pos < e.data.length) {
        ops.fd(plan, stage, FD_BYTES, fd, e.eof ? -1 : e.fd, e.data.subarray(e.pos));
        e.data = EMPTY;
        e.pos = 0;
      } else if (e.eof) ops.fd(plan, stage, FD_BYTES, fd, -1, EMPTY);
      else ops.fd(plan, stage, FD_HOST, fd, e.fd);
      break;
    case 'adopt':
      ops.fd(plan, stage, FD_ADOPT, fd, e.fd);
      break;
    case 'bytes': {
      const data = e.pos ? e.data.subarray(e.pos) : e.data;
      e.pos = e.data.length;
      ops.fd(plan, stage, FD_BYTES, fd, -1, data);
      break;
    }
  }
}

const whichCache = new Map();

function executable(path) {
  try {
    const st = fs.statSync(path);
    if (!st.isFile()) return false;
    if (process.platform === 'win32') return true;
    fs.accessSync(path, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

export function which(c, name) {
  if (name.includes('/')) return resolvePath(c, name);
  const pathVar = get(c, 'PATH');
  const key = pathVar + '\0' + name;
  const cached = whichCache.get(key);
  if (cached && executable(cached)) return cached;
  for (let dir of pathVar.split(process.platform === 'win32' ? ';' : ':')) {
    if (dir === '') dir = '.';
    const candidate = resolvePath(c, dir.endsWith('/') ? dir + name : dir + '/' + name);
    if (executable(candidate)) {
      if (whichCache.size > 4096) whichCache.clear();
      whichCache.set(key, candidate);
      return candidate;
    }
    if (process.platform === 'win32') {
      for (const ext of ['.exe', '.cmd', '.bat', '.com']) {
        if (executable(candidate + ext)) return candidate + ext;
      }
    }
  }
  return null;
}

export function clearWhich() {
  whichCache.clear();
}

export function envList(c, extra) {
  if (!c.vars.dirty && !extra) return null;
  const out = [];
  const seen = new Set();
  if (extra) {
    for (const [name, value] of extra) {
      if (seen.has(name)) continue;
      seen.add(name);
      out.push(`${name}=${value}`);
    }
  }
  for (const name of c.vars.names()) {
    if (seen.has(name)) continue;
    const v = c.vars.find(name);
    if (!v || !v.x || v.k !== 0 || v.v === undefined) continue;
    out.push(`${name}=${v.v}`);
  }
  return out;
}

function argvStrings(argv) {
  const out = new Array(argv.length);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const s = typeof a === 'string' ? a : String(a);
    if (s.includes('\0')) throw new TypeError('ant:shell: command arguments cannot contain NUL bytes');
    out[i] = encodeOut(s);
  }
  return out;
}

function commandNotFound(name) {
  return `ant:shell: ${name}: command not found\n`;
}

function externalStage(plan, c, argv, tempEnv) {
  const name = argv[0];
  const path = name === '' ? null : which(c, name);
  if (!path) {
    ops.native(plan, '', commandNotFound(name), 127);
    return;
  }
  if (name.includes('/')) {
    let st = null;
    try {
      st = fs.statSync(path);
    } catch {}
    if (!st) {
      ops.native(plan, '', `ant:shell: ${name}: No such file or directory\n`, 127);
      return;
    }
    if (st.isDirectory()) {
      ops.native(plan, '', `ant:shell: ${name}: Is a directory\n`, 126);
      return;
    }
  }
  ops.external(plan, path, argv, envList(c, tempEnv));
}

function tempEnvOf(c, frame) {
  if (!frame || frame.size === 0) return null;
  const list = [];
  for (const [name, v] of frame) if (v.k === 0 && v.v !== undefined) list.push([name, v.v]);
  return list;
}

async function finishPlan(c, plan, caps) {
  const flushed = flushPipes(c);
  if (flushed) await flushed;
  if (c.job !== null) ops.spawned(plan).then(pids => c.job.onPids(pids));
  const result = await ops.submit(plan);
  caps.deliver(result);
  if (result.diagnostics) await write(c, 2, result.diagnostics);
  c.root.signal = result.signalCode;
  return result.statuses;
}

async function runExternal(c, argv, frame, redirs) {
  const plan = ops.plan(c.cwd, umaskArg(c), c.limits === null ? undefined : limitsArg(c));
  const caps = new Captures(plan);
  baseFds(plan, c.fds, caps);
  externalStage(plan, c, argv, tempEnvOf(c, frame));
  if (redirs && !stageRedirs(plan, 0, c, redirs)) return 1;
  const statuses = await finishPlan(c, plan, caps);
  return statuses[0];
}

export async function fillStream(c, e, delim, minBytes) {
  const flushed = flushPipes(c);
  if (flushed) await flushed;
  while (!e.eof) {
    const available = e.data.length - e.pos;
    if (delim >= 0 && available > 0 && e.data.indexOf(delim, e.pos) !== -1) return;
    if (minBytes > 0 && available >= minBytes) return;
    const chunk = await ops.pread(e.fd, 65536);
    if (typeof chunk === 'string' || chunk.length === 0) {
      e.eof = true;
      return;
    }
    if (available === 0) e.data = chunk;
    else {
      const merged = new Uint8Array(available + chunk.length);
      merged.set(e.data.subarray(e.pos));
      merged.set(chunk, available);
      e.data = merged;
    }
    e.pos = 0;
  }
}

export class IO {
  constructor(c) {
    this.c = c;
    this.outs = null;
    this.errs = null;
  }

  out(text) {
    if (this.outs === null) this.outs = text;
    else this.outs += text;
  }

  err(text) {
    if (this.errs === null) this.errs = text;
    else this.errs += text;
  }

  error(name, message) {
    this.err(`ant:shell: ${name ? name + ': ' : ''}${message}\n`);
  }

  stdin(fd = 0) {
    return this.c.fds[fd];
  }

  flushQuick() {
    let ok = true;
    let pendingOut = null;
    let pendingErr = null;
    if (this.outs !== null) {
      const r = write(this.c, 1, this.outs);
      this.outs = null;
      if (r === false) ok = false;
      else if (r !== true) pendingOut = r;
    }
    if (this.errs !== null) {
      const r = write(this.c, 2, this.errs);
      this.errs = null;
      if (r !== true && r !== false) pendingErr = r;
    }
    if (pendingOut === null && pendingErr === null) return ok;
    return (async () => {
      const outOk = pendingOut === null ? ok : await pendingOut;
      if (pendingErr !== null) await pendingErr;
      return outOk;
    })();
  }

  async flush() {
    let ok = true;
    if (this.outs !== null) {
      const r = write(this.c, 1, this.outs);
      ok = r === true || (r !== false && (await r));
      this.outs = null;
    }
    if (this.errs !== null) {
      const r = write(this.c, 2, this.errs);
      if (r !== true && r !== false) await r;
      this.errs = null;
    }
    return ok;
  }
}

export const registry = { builtins: Object.create(null), compile: null };

function pushFrame(c, frame) {
  c.vars.frames.push(frame);
}

function popFrame(c, frame) {
  const frames = c.vars.frames;
  const at = frames.lastIndexOf(frame);
  if (at > 0) frames.splice(at, 1);
}

export function tframe(c) {
  const frame = new Map();
  frame.temp = true;
  pushFrame(c, frame);
  return frame;
}

export function tset(c, frame, spec) {
  let v = frame.get(spec.n);
  if (!v) {
    const existing = c.vars.find(spec.n);
    if (existing && existing.r) throw new ShellError(`${spec.n}: readonly variable`);
    v = existing ? existing.clone() : new Var(undefined);
    v.t = false;
    v.x = true;
    frame.set(spec.n, v);
  }
  assignSpec(c, spec, v);
}

export function tpop(c, frame) {
  popFrame(c, frame);
}

function trace(c, argv, frame) {
  const ps4 = get(c, 'PS4');
  const parts = [];
  if (frame) for (const [name, v] of frame) parts.push(`${name}=${traceWord(String(v.v ?? ''))}`);
  for (const a of argv) parts.push(traceWord(String(a)));
  return write(c, 2, `${ps4}${parts.join(' ')}\n`);
}

function traceWord(word) {
  return /^[A-Za-z0-9_@%+=:,./-]+$/.test(word) ? word : shellQuote(word);
}

export function enabledBuiltin(c, name) {
  const builtin = registry.builtins[name];
  return builtin && !(c.root.disabled !== null && c.root.disabled.has(name)) ? builtin : undefined;
}

function quickBuiltin(c, builtin, argv) {
  for (let i = 1; i < argv.length; i++) {
    if (typeof argv[i] === 'string' && argv[i].includes('\0')) throw new TypeError('ant:shell: command arguments cannot contain NUL bytes');
  }
  const io = new IO(c);
  let status;
  try {
    status = builtin(c, argv, io);
  } catch (error) {
    const shellError = toShellError(error);
    if (!shellError || shellError.fatal) {
      const flushed = io.flushQuick();
      if (flushed === true || flushed === false) throw error;
      return flushed.then(() => {
        throw error;
      });
    }
    io.err(`ant:shell: ${shellError.message}\n`);
    status = shellError.status;
  }
  if (status !== null && typeof status === 'object' && typeof status.then === 'function') {
    return status.then(
      async value => {
        const ok = await io.flush();
        return !ok && !value ? 1 : value ?? 0;
      },
      async error => {
        await io.flush();
        const shellError = toShellError(error);
        if (!shellError || shellError.fatal) throw error;
        await write(c, 2, `ant:shell: ${shellError.message}\n`);
        return shellError.status;
      }
    );
  }
  const errored = io.errs !== null;
  if (status && builtin.special && errored && c.opts.posix) {
    const flushedFirst = io.flushQuick();
    if (flushedFirst === true || flushedFirst === false) throw new ShellExit(status);
    return flushedFirst.then(() => {
      throw new ShellExit(status);
    });
  }
  const flushed = io.flushQuick();
  if (flushed === true || flushed === false) return !flushed && !status ? 1 : status ?? 0;
  return flushed.then(ok => (!ok && !status ? 1 : status ?? 0));
}

export function run(c, argv, frame, redirs) {
  if (c.opts.n) throw new ShellExit(c.status);
  if (c.job !== null && c.job.killed) throw new ShellExit(128 + c.job.killed);
  if (c.root.abort !== null) throw new ShellExit(c.root.abort.status);
  if (c.post !== null || (c.traps !== null && c.traps.DEBUG && !c.inTrap)) return runTrapped(c, argv, frame, redirs);
  if (frame === null && redirs === null && argv.length !== 0 && !c.opts.x && !c.opts.expand_aliases) {
    const name = argv[0];
    if (typeof name === 'string' && name !== 'exec' && !c.funcs.get(name)) {
      const builtin = c.root.disabled === null ? registry.builtins[name] : enabledBuiltin(c, name);
      if (builtin) {
        if (name.includes('\0')) throw new TypeError('ant:shell: command arguments cannot contain NUL bytes');
        const last = argv[argv.length - 1];
        const r = quickBuiltin(c, builtin, argv);
        if (typeof r === 'number') {
          c.us = last;
          return (c.status = r);
        }
        return r.then(status => {
          c.us = last;
          return (c.status = status);
        });
      }
    }
  }
  return runSlow(c, argv, frame, redirs);
}

async function runSlow(c, argv, frame, redirs) {
  if (frame && !(frame instanceof Map)) {
    const specs = frame;
    frame = tframe(c);
    try {
      for (const spec of specs) tset(c, frame, spec);
    } catch (error) {
      popFrame(c, frame);
      throw error;
    }
  }
  try {
    if (argv.length === 0) {
      if (frame) {
        const temp = frame;
        popFrame(c, temp);
        frame = null;
        for (const [name, v] of temp) assignSpec(c, { n: name, i: undefined, v: v.v ?? '', a: null, p: false });
      }
      if (redirs) {
        const token = await redir(c, redirs);
        if (!token) return (c.status = 1);
        await unredir(c, token);
      }
      return (c.status = 0);
    }
    if (c.opts.x) {
      const r = trace(c, argv, frame);
      if (r !== true && r !== false) await r;
    }
    const name = typeof argv[0] === 'string' ? argv[0] : String(argv[0]);
    if (name.includes('\0')) throw new TypeError('ant:shell: command arguments cannot contain NUL bytes');
    if (name === 'exec' && argv.length === 1) {
      if (redirs) {
        const token = await redir(c, redirs);
        if (!token) return (c.status = 1);
        await makePermanent(c, token);
      }
      return (c.status = 0);
    }
    if (redirs && needsShellRedirs(redirs)) {
      const token = await redir(c, redirs);
      if (!token) return (c.status = 1);
      const temp = frame;
      frame = null;
      try {
        return (c.status = await runSlow(c, argv, temp, null));
      } finally {
        await unredir(c, token);
      }
    }
    if (c.opts.expand_aliases && aliasView(c) && aliasView(c).has(name) && !(c.aliasing && c.aliasing.has(name))) {
      return (c.status = await runAlias(c, name, argv, redirs));
    }
    const fn = c.funcs.get(name);
    if (fn) return (c.status = await callFunction(c, fn, argv, frame, redirs));
    const builtin = enabledBuiltin(c, name);
    if (builtin) return (c.status = await runBuiltin(c, builtin, argv, redirs));
    return (c.status = await runExternal(c, argvStrings(argv), frame, redirs));
  } finally {
    if (frame) popFrame(c, frame);
    if (argv.length !== 0) c.us = argv[argv.length - 1];
  }
}

function aliasView(c) {
  return c.root.aliasView !== undefined ? c.root.aliasView : c.root.aliases;
}

export function line(c) {
  const root = c.root;
  root.aliasView = root.aliases ? new Map(root.aliases) : null;
}

export function verbose(c, text) {
  return write(c, 2, text + '\n');
}

async function runAlias(c, name, argv, redirs) {
  const text = aliasView(c).get(name) + (argv.length > 1 ? ' ' + argv.slice(1).map(a => shellQuote(String(a))).join(' ') : '');
  const token = redirs ? await redir(c, redirs) : null;
  if (redirs && !token) return 1;
  (c.aliasing ??= new Set()).add(name);
  try {
    return await evalString(c, text, 'alias');
  } finally {
    c.aliasing.delete(name);
    if (token) await unredir(c, token);
  }
}

async function runTrapped(c, argv, frame, redirs) {
  const post = takePost(c);
  try {
    if (c.traps !== null && c.traps.DEBUG && !c.inTrap) {
      c.inTrap = true;
      const status = c.status;
      c.command = argv.map(String).join(' ');
      try {
        await evalString(c, c.traps.DEBUG, 'trap');
      } finally {
        c.inTrap = false;
      }
      c.status = status;
    }
    return (c.status = await runSlow(c, argv, frame, redirs));
  } finally {
    if (post) {
      const status = c.status;
      await runPost(post);
      c.status = status;
    }
  }
}

export async function runCommandOnly(c, argv, frame, redirs, allowBuiltins = true) {
  const name = String(argv[0]);
  const builtin = allowBuiltins ? registry.builtins[name] : null;
  if (builtin) return runBuiltin(c, builtin, argv, redirs);
  return runExternal(c, argvStrings(argv), frame, redirs);
}

export async function runBuiltin(c, builtin, argv, redirs) {
  let token = null;
  if (redirs) {
    token = await redir(c, redirs);
    if (!token) return 1;
  }
  const io = new IO(c);
  let status = 0;
  try {
    for (let i = 1; i < argv.length; i++) {
      if (typeof argv[i] === 'string' && argv[i].includes('\0')) throw new TypeError('ant:shell: command arguments cannot contain NUL bytes');
    }
    status = await builtin(c, argv, io);
  } catch (error) {
    const shellError = toShellError(error);
    if (!shellError || shellError.fatal) throw error;
    io.err(`ant:shell: ${shellError.message}\n`);
    status = shellError.status;
  } finally {
    const errored = io.errs !== null;
    const ok = await io.flush();
    if (!ok && status === 0) status = 1;
    if (token) await unredir(c, token);
    if (status && builtin.special && errored && c.opts.posix) throw new ShellExit(status);
  }
  return status === undefined ? 0 : status;
}

export async function callFunction(c, fn, argv, frame, redirs) {
  let token = null;
  if (redirs) {
    token = await redir(c, redirs);
    if (!token) return 1;
  }
  const root = c.root;
  if (++root.funcDepth > 2000) {
    root.funcDepth--;
    if (token) await unredir(c, token);
    throw new ShellError(`${fn.name}: maximum function nesting level exceeded`, 1, true);
  }
  const local = new Map();
  pushFrame(c, local);
  const savedPos = c.pos;
  const savedNe = c.ne;
  c.pos = argvStrings(argv).slice(1);
  c.fstack.push(fn.name);
  (c.flines ??= []).push(c.ln);
  let status;
  try {
    status = await fn.body(c);
  } catch (error) {
    if (!(error instanceof ShellReturn)) throw error;
    status = error.status;
  } finally {
    if (c.traps !== null && c.traps.RETURN && !c.inTrap) {
      c.inTrap = true;
      const saved = c.status;
      try {
        await evalString(c, c.traps.RETURN, 'trap');
      } catch {}
      c.inTrap = false;
      c.status = saved;
    }
    root.funcDepth--;
    popFrame(c, local);
    c.ne = savedNe;
    c.pos = savedPos;
    c.fstack.pop();
    c.flines.pop();
    if (local.savedOpts) c.opts = Object.assign(Object.create(DEFAULT_OPTS), local.savedOpts);
    if (token) await unredir(c, token);
  }
  return (c.status = status);
}

export async function runSub(s, fn) {
  let status;
  try {
    await fn(s);
    status = s.status;
  } catch (error) {
    if (error instanceof ShellExit || error instanceof ShellReturn) status = error.status;
    else {
      const shellError = toShellError(error);
      if (!shellError) throw error;
      await errorMessage(s, shellError.message);
      status = shellError.status;
    }
  }
  return runExitTrap(s, status);
}

export async function runExitTrap(c, status) {
  const trap = c.traps && c.traps.EXIT;
  if (!trap) return status;
  delete c.traps.EXIT;
  c.status = status;
  try {
    await evalString(c, trap);
  } catch (error) {
    if (error instanceof ShellExit) return error.status;
    if (!(error instanceof ShellReturn) && !(error instanceof ShellError)) throw error;
  }
  return status;
}

export async function evalString(c, text, name = 'eval') {
  if (!text.trim()) return (c.status = 0);
  let fn;
  try {
    fn = registry.compile(text, name, c.opts.extglob);
  } catch (error) {
    if (error instanceof SyntaxError) {
      await errorMessage(c, String(error.message).replace(/^ant:shell: /, ''));
      return (c.status = 2);
    }
    throw error;
  }
  await fn(R, c, []);
  return c.status;
}

export async function sub(c, fn) {
  const s = c.subshell();
  c.status = await runSub(s, fn);
  return c.status;
}

export async function cs(c, fn) {
  const s = c.subshell();
  const sink = new Sink();
  s.fds[1] = { k: 'cap', sink };
  if (!c.opts.inherit_errexit) s.opts.e = false;
  c.ls = await runSub(s, fn);
  if (!c.opts.posix) c.status = c.ls;
  let text = sink.text();
  let end = text.length;
  while (end > 0 && text.charCodeAt(end - 1) === 10) end--;
  if (end !== text.length) text = text.slice(0, end);
  if (text.includes('\0')) text = text.replace(/\0/g, '');
  return text;
}

export async function slurp(c, target) {
  try {
    const text = decodeBytes(await readBytes(resolvePath(c, target)));
    c.ls = 0;
    return text.replace(/\n+$/, '');
  } catch (error) {
    await errorMessage(c, `${target}: ${errnoText(error)}`);
    c.ls = 1;
    return '';
  }
}

async function runPost(pending) {
  for (const cleanup of pending) await cleanup();
}

function takePost(c) {
  const pending = c.post;
  c.post = null;
  return pending;
}

function freeProcsubFd(c) {
  for (let fd = 9; fd >= 3; fd--) if (!c.fds[fd]) return fd;
  return -1;
}

export async function procsub(c, dir, fn) {
  const fd = freeProcsubFd(c);
  if (fd < 0) throw new ShellError('process substitution: no free file descriptor');
  const [readFd, writeFd] = ops.pipe();
  const s = c.subshell();
  if (dir === '<') {
    const out = { k: 'host', fd: writeFd, pipe: true, buf: null };
    s.fds[1] = out;
    runSub(s, async x => {
      try {
        await fn(x);
      } finally {
        try {
          await flushEntry(out);
        } catch {}
        ops.close(writeFd);
      }
    }).catch(() => {});
    const entry = { k: 'stream', fd: readFd, data: EMPTY, pos: 0, eof: false };
    c.fds[fd] = entry;
    (c.post ??= []).push(async () => {
      ops.close(readFd);
      if (c.fds[fd] === entry) c.fds[fd] = undefined;
    });
  } else {
    s.fds[0] = { k: 'stream', fd: readFd, data: EMPTY, pos: 0, eof: false };
    const job = runSub(s, async x => {
      try {
        await fn(x);
      } finally {
        ops.close(readFd);
      }
    });
    const entry = { k: 'host', fd: writeFd, pipe: true, buf: null };
    c.fds[fd] = entry;
    (c.post ??= []).push(async () => {
      try {
        await flushEntry(entry);
      } catch {}
      ops.close(writeFd);
      if (c.fds[fd] === entry) c.fds[fd] = undefined;
      await job;
    });
  }
  return `/dev/fd/${fd}`;
}

const PSEUDO_PID_BASE = 1 << 30;

function startJob(c, s, fn, text) {
  const root = c.root;
  const n = ++root.jobCounter;
  const job = { n, text, pid: 0, pids: new Set(), done: false, status: 0, promise: null, killed: 0, onPids: null, spawned: null };
  let markSpawned;
  job.spawned = new Promise(resolve => (markSpawned = resolve));
  job.onPids = pids => {
    if (!pids) return;
    for (const pid of pids) {
      if (!pid) continue;
      job.pids.add(pid);
      if (!job.pid) {
        job.pid = pid;
        markSpawned();
      }
    }
  };
  s.job = job;
  job.promise = runSub(s, fn).then(
    status => {
      job.done = true;
      job.status = status;
      markSpawned();
      return status;
    },
    error => {
      job.done = true;
      job.status = 1;
      job.error = error;
      markSpawned();
      return 1;
    }
  );
  (root.jobs ??= new Map()).set(n, job);
  return job;
}

async function settleJobPid(c, job) {
  await Promise.race([job.spawned, new Promise(resolve => setTimeout(resolve, 0))]);
  if (!job.pid) job.pid = PSEUDO_PID_BASE + job.n;
  c.lastBg = job.pid;
  return job;
}

export async function bg(c, fn, text) {
  const job = startJob(c, c.subshell(), fn, text || '');
  await settleJobPid(c, job);
}

function allocFd(c, from = 10) {
  for (let fd = from; fd < 256; fd++) if (!c.fds[fd]) return fd;
  throw new ShellError('no free file descriptors');
}

export async function coproc(c, name, fn, text) {
  const [inRead, inWrite] = ops.pipe();
  const [outRead, outWrite] = ops.pipe();
  const s = c.subshell();
  const outEntry = { k: 'host', fd: outWrite, pipe: true, buf: null };
  s.fds[0] = { k: 'stream', fd: inRead, data: EMPTY, pos: 0, eof: false };
  s.fds[1] = outEntry;
  const job = startJob(c, s, async x => {
    try {
      await fn(x);
    } finally {
      try {
        await flushEntry(outEntry);
      } catch {}
      ops.close(outWrite);
      ops.close(inRead);
    }
  }, text || '');
  const readFd = allocFd(c, 60);
  c.fds[readFd] = { k: 'stream', fd: outRead, data: EMPTY, pos: 0, eof: false, rootOwned: true };
  const writeFd = allocFd(c, 60);
  c.fds[writeFd] = { k: 'host', fd: inWrite, pipe: true, buf: null, rootOwned: true };
  const owned = (c.root.owned ??= new Set());
  owned.add(outRead);
  owned.add(inWrite);
  await settleJobPid(c, job);
  setArray(c, name, [[null, String(readFd), 0], [null, String(writeFd), 0]], false);
  setVar(c, `${name}_PID`, undefined, String(job.pid));
  c.status = 0;
}

export function findJob(c, spec) {
  const jobs = c.root.jobs;
  if (!jobs || jobs.size === 0) return null;
  const list = [...jobs.values()];
  if (!spec.startsWith('%')) {
    const pid = Number(spec);
    return list.find(job => job.pid === pid || job.pids.has(pid)) || null;
  }
  const body = spec.slice(1);
  if (body === '' || body === '%' || body === '+') return list[list.length - 1];
  if (body === '-') return list[list.length - 2] || list[list.length - 1];
  if (/^[0-9]+$/.test(body)) return jobs.get(Number(body)) || null;
  if (body.startsWith('?')) return list.find(job => job.text.includes(body.slice(1))) || null;
  return list.find(job => job.text.startsWith(body)) || null;
}

export const SIGNALS = [
  null, 'HUP', 'INT', 'QUIT', 'ILL', 'TRAP', 'ABRT', 'EMT', 'FPE', 'KILL', 'BUS', 'SEGV', 'SYS', 'PIPE', 'ALRM', 'TERM',
  'URG', 'STOP', 'TSTP', 'CONT', 'CHLD', 'TTIN', 'TTOU', 'IO', 'XCPU', 'XFSZ', 'VTALRM', 'PROF', 'WINCH', 'INFO', 'USR1', 'USR2'
];

export function signalNumber(name) {
  const os_signals = os.constants && os.constants.signals;
  const key = 'SIG' + name;
  if (os_signals && typeof os_signals[key] === 'number') return os_signals[key];
  const index = SIGNALS.indexOf(name);
  return index > 0 ? index : 0;
}

export function signalJob(job, signal) {
  const number = signalNumber(signal);
  let delivered = false;
  for (const pid of job.pids) {
    try {
      process.kill(pid, signal === '0' ? 0 : 'SIG' + signal);
      delivered = true;
    } catch {}
  }
  if (signal !== '0' && !job.done) {
    job.killed = number || 15;
    delivered = true;
  }
  return delivered || !job.done;
}

const signalTraps = new Map();

function signalListener(signal) {
  return () => {
    const entries = signalTraps.get(signal);
    if (!entries) return;
    for (const c of entries) {
      const action = c.traps && c.traps[signal];
      if (!action) continue;
      c.trapChain = (c.trapChain || Promise.resolve()).then(async () => {
        const status = c.status;
        try {
          await evalString(c, action, 'trap');
          c.status = status;
        } catch (error) {
          if (error instanceof ShellExit) c.root.abort = { status: error.status };
        }
      });
    }
  };
}

export function setSignalTrap(c, signal, active) {
  if (signal === 'EXIT' || signal === 'ERR' || signal === 'DEBUG' || signal === 'RETURN' || signal === 'KILL' || signal === 'STOP') return;
  let entries = signalTraps.get(signal);
  if (active) {
    if (!entries) {
      entries = new Set();
      entries.listener = signalListener(signal);
      signalTraps.set(signal, entries);
      try {
        process.on('SIG' + signal, entries.listener);
      } catch {}
    }
    entries.add(c);
    (c.root.signalCtxs ??= new Set()).add(c);
  } else if (entries) {
    entries.delete(c);
    if (entries.size === 0) {
      try {
        process.removeListener('SIG' + signal, entries.listener);
      } catch {}
      signalTraps.delete(signal);
    }
  }
}

export function releaseSignalTraps(root) {
  if (!root.signalCtxs) return;
  for (const c of root.signalCtxs) for (const signal of Object.keys(c.traps || {})) setSignalTrap(c, signal, false);
  root.signalCtxs = null;
}

async function readLine(c) {
  const e = c.fds[0];
  if (!e) return null;
  if (e.k === 'stream') await fillStream(c, e, 10, 0);
  if (e.k === 'bytes' || e.k === 'stream') {
    if (e.pos >= e.data.length) return null;
    let end = e.data.indexOf(10, e.pos);
    const found = end !== -1;
    if (!found) end = e.data.length;
    const line = decodeBytes(e.data.subarray(e.pos, end));
    e.pos = found ? end + 1 : end;
    return line;
  }
  if (e.k === 'host') {
    const bytes = [];
    const buf = Buffer.alloc(1);
    for (;;) {
      let n = 0;
      try {
        n = fs.readSync(e.fd, buf, 0, 1, null);
      } catch {}
      if (n <= 0) return bytes.length ? decodeBytes(Uint8Array.from(bytes)) : null;
      if (buf[0] === 10) return decodeBytes(Uint8Array.from(bytes));
      bytes.push(buf[0]);
    }
  }
  return null;
}

export async function select(c, state) {
  const items = state.items;
  for (;;) {
    if (state.m) {
      const width = String(items.length).length;
      let menu = '';
      items.forEach((item, i) => (menu += `${String(i + 1).padStart(width)}) ${item}\n`));
      await write(c, 2, menu);
      state.m = false;
    }
    await write(c, 2, isset(c, 'PS3', undefined, false) ? get(c, 'PS3') : '#? ');
    const line = await readLine(c);
    if (line === null) {
      await write(c, 2, '\n');
      c.status = 1;
      return null;
    }
    setVar(c, 'REPLY', undefined, line);
    if (line.trim() === '') {
      state.m = true;
      continue;
    }
    const n = /^[0-9]+$/.test(line.trim()) ? Number(line.trim()) : 0;
    return n >= 1 && n <= items.length ? items[n - 1] : '';
  }
}

export function defun(c, name, body, text) {
  c.funcs.set(name, { name, body, text });
}

export function ck(c) {
  if (c.status === 0 || c.ne > 0) return;
  const trap = c.traps && c.traps.ERR;
  if (trap) return errTrap(c, trap);
  if (c.opts.e) throw new ShellExit(c.status);
}

async function errTrap(c, trap) {
  const status = c.status;
  c.ne++;
  try {
    await evalString(c, trap, 'trap');
  } finally {
    c.ne--;
  }
  c.status = status;
  if (c.opts.e) throw new ShellExit(status);
}

export function retStatus(c, argv) {
  if (argv.length < 2) return c.status;
  const s = String(argv[1]);
  if (!/^-?[0-9]+$/.test(s)) {
    errorMessage(c, `return: ${s}: numeric argument required`);
    return 2;
  }
  return Number(s) & 255;
}

export function ret(c, argv) {
  return (c.status = retStatus(c, argv));
}

export function Ret(c, argv) {
  return new ShellReturn(retStatus(c, argv));
}

export function setv(c, name, value) {
  setVar(c, name, undefined, value);
}

export function positional(c) {
  return c.pos.slice();
}

export function match(c, value, pattern) {
  return matchPattern(value, pattern, c.opts.nocasematch, c.opts.extglob);
}

export function cmatch(c, value, pattern) {
  return matchPattern(value, pattern, c.opts.nocasematch, true);
}

const regexCache = new Map();

export function rematch(c, value, source) {
  let regex = regexCache.get(source);
  if (!regex) {
    try {
      regex = ereToRegExp(source);
    } catch {
      try {
        regex = new RegExp(source);
      } catch {
        c.status = 2;
        return false;
      }
    }
    if (regexCache.size > 512) regexCache.clear();
    regexCache.set(source, regex);
  }
  if (c.opts.nocasematch && !regex.flags.includes('i')) regex = new RegExp(regex.source, regex.flags + 'i');
  const m = regex.exec(value);
  const v = c.vars.writable('BASH_REMATCH');
  v.k = 1;
  v.v = undefined;
  v.a = m ? Array.from(m, x => x ?? '') : [];
  return !!m;
}

function statOf(path, link = false) {
  try {
    return link ? fs.lstatSync(path) : fs.statSync(path);
  } catch {
    return null;
  }
}

function access(path, mode) {
  try {
    fs.accessSync(path, mode);
    return true;
  } catch {
    return false;
  }
}

function testInteger(text, inCond, c) {
  if (inCond) return arithString(c, text);
  const s = text.trim();
  if (!/^[-+]?[0-9]+$/.test(s)) throw new ShellError(`${text}: integer expression expected`, 2);
  return Number(s);
}

export function t1(c, op, operand) {
  switch (op) {
    case '-n': return operand !== '';
    case '-z': return operand === '';
    case '-v': return isset(c, operand, undefined, false) || (/\[/.test(operand) && isset(c, operand, undefined, false));
    case '-o': return !!c.opts[operand] || (operand === 'errexit' && c.opts.e) || (operand === 'nounset' && c.opts.u) || (operand === 'xtrace' && c.opts.x) || (operand === 'noglob' && c.opts.f) || (operand === 'noclobber' && c.opts.C);
    case '-t': return false;
    case '-R': return false;
  }
  const path = resolvePath(c, operand);
  if (operand === '') return false;
  switch (op) {
    case '-e': case '-a': return statOf(path) !== null;
    case '-f': return statOf(path)?.isFile() ?? false;
    case '-d': return statOf(path)?.isDirectory() ?? false;
    case '-h': case '-L': return statOf(path, true)?.isSymbolicLink() ?? false;
    case '-s': return (statOf(path)?.size ?? 0) > 0;
    case '-r': return access(path, fs.constants.R_OK);
    case '-w': return access(path, fs.constants.W_OK);
    case '-x': return access(path, fs.constants.X_OK);
    case '-b': return statOf(path)?.isBlockDevice() ?? false;
    case '-c': return statOf(path)?.isCharacterDevice() ?? false;
    case '-p': return statOf(path)?.isFIFO() ?? false;
    case '-S': return statOf(path)?.isSocket() ?? false;
    case '-g': return ((statOf(path)?.mode ?? 0) & 0o2000) !== 0;
    case '-u': return ((statOf(path)?.mode ?? 0) & 0o4000) !== 0;
    case '-k': return ((statOf(path)?.mode ?? 0) & 0o1000) !== 0;
    case '-O': {
      const st = statOf(path);
      return !!st && typeof process.geteuid === 'function' && st.uid === process.geteuid();
    }
    case '-G': {
      const st = statOf(path);
      return !!st && typeof process.getegid === 'function' && st.gid === process.getegid();
    }
    case '-N': {
      const st = statOf(path);
      return !!st && st.mtimeMs > st.atimeMs;
    }
  }
  throw new ShellError(`${op}: unary operator expected`, 2);
}

export function t2(c, op, a, b, inCond) {
  switch (op) {
    case '=': case '==': return a === b;
    case '!=': return a !== b;
    case '<': return a < b;
    case '>': return a > b;
    case '-eq': return testInteger(a, inCond, c) === testInteger(b, inCond, c);
    case '-ne': return testInteger(a, inCond, c) !== testInteger(b, inCond, c);
    case '-lt': return testInteger(a, inCond, c) < testInteger(b, inCond, c);
    case '-le': return testInteger(a, inCond, c) <= testInteger(b, inCond, c);
    case '-gt': return testInteger(a, inCond, c) > testInteger(b, inCond, c);
    case '-ge': return testInteger(a, inCond, c) >= testInteger(b, inCond, c);
    case '-nt': {
      const x = statOf(resolvePath(c, a));
      const y = statOf(resolvePath(c, b));
      return !!x && (!y || x.mtimeMs > y.mtimeMs);
    }
    case '-ot': {
      const x = statOf(resolvePath(c, a));
      const y = statOf(resolvePath(c, b));
      return !!y && (!x || x.mtimeMs < y.mtimeMs);
    }
    case '-ef': {
      const x = statOf(resolvePath(c, a));
      const y = statOf(resolvePath(c, b));
      return !!x && !!y && x.dev === y.dev && x.ino === y.ino;
    }
  }
  throw new ShellError(`${op}: binary operator expected`, 2);
}

export function now() {
  return Date.now();
}

export function time(c, start) {
  const elapsed = (Date.now() - start) / 1000;
  const minutes = Math.floor(elapsed / 60);
  const seconds = (elapsed - minutes * 60).toFixed(3);
  return write(c, 2, `\nreal\t${minutes}m${seconds}s\nuser\t0m0.000s\nsys\t0m0.000s\n`);
}

export function D(argv, assigns, redirs) {
  return { argv, assigns, redirs, js: null };
}

export function J(fn) {
  return { argv: null, assigns: null, redirs: null, js: fn };
}

export function AW(spec) {
  return new AssignWord(spec);
}

export function assign(c, spec) {
  assignSpec(c, spec);
}

function setPipeStatus(c, statuses) {
  const v = c.vars.writable('PIPESTATUS');
  v.k = 1;
  v.v = undefined;
  v.a = statuses.map(String);
}

async function evaluateBuiltinStage(s, builtin, argv) {
  const io = new IO(s);
  let status = 0;
  try {
    status = await builtin(s, argv, io);
  } catch (error) {
    if (error instanceof ShellExit || error instanceof ShellReturn) status = error.status;
    else {
      const shellError = toShellError(error);
      if (!shellError) throw error;
      io.err(`ant:shell: ${shellError.message}\n`);
      status = shellError.status;
    }
  }
  return { out: io.outs ?? '', err: io.errs ?? '', status: status ?? 0 };
}

export async function pipe(c, stages, errs) {
  const n = stages.length;
  const subs = new Array(n);
  const descs = new Array(n);
  const lastpipe = c.opts.lastpipe;
  const posts = new Array(n).fill(null);
  for (let i = 0; i < n; i++) {
    const s = lastpipe && i === n - 1 ? c : c.subshell();
    subs[i] = s;
    descs[i] = await stages[i](s);
    posts[i] = takePost(s);
  }
  const kinds = new Array(n);
  for (let i = 0; i < n; i++) {
    const d = descs[i];
    if (d.js) {
      kinds[i] = 'js';
      continue;
    }
    if (d.argv.length === 0) {
      kinds[i] = 'empty';
      continue;
    }
    const name = String(d.argv[0]);
    const s = subs[i];
    if (s.funcs.get(name)) {
      kinds[i] = 'js';
      continue;
    }
    const builtin = registry.builtins[name];
    if (builtin) {
      kinds[i] = builtin.stdin ? 'js' : 'builtin';
      continue;
    }
    kinds[i] = 'external';
  }

  const statuses = new Array(n).fill(0);
  const tasks = [];
  let input = c.fds[0];
  let i = 0;
  while (i < n) {
    let j = i + 1;
    if (kinds[i] !== 'js') while (j < n && kinds[j] !== 'js') j++;
    const last = j === n;
    let writeFd = -1;
    let next = null;
    if (!last) {
      const [readFd, fd] = ops.pipe();
      writeFd = fd;
      next = kinds[j] === 'js' ? { k: 'stream', fd: readFd, data: EMPTY, pos: 0, eof: false, pipeOwned: true } : { k: 'adopt', fd: readFd };
    }
    if (kinds[i] === 'js') {
      const index = i;
      const outEntry = last ? null : { k: 'host', fd: writeFd, pipe: true, buf: null };
      tasks.push(runJsStage(c, subs[i], descs[i], input, outEntry, errs[i]).then(status => {
        statuses[index] = status;
      }));
    } else {
      const plan = ops.plan(c.cwd, umaskArg(c), c.limits === null ? undefined : limitsArg(c));
      const caps = new Captures(plan);
      const fds = c.fds.slice();
      fds[0] = input;
      if (!last) fds[1] = { k: 'adopt', fd: writeFd };
      baseFds(plan, fds, caps);
      const failed = [];
      for (let k = i; k < j; k++) {
        const d = descs[k];
        const s = subs[k];
        const index = k - i;
        if (kinds[k] === 'empty') ops.native(plan, '', '', 0);
        else if (kinds[k] === 'builtin') {
          if (s.opts.x) await trace(s, d.argv, null);
          const r = await evaluateBuiltinStage(s, registry.builtins[String(d.argv[0])], d.argv);
          ops.native(plan, encodeOut(r.out), encodeOut(r.err), r.status & 255);
        } else {
          if (s.opts.x) await trace(s, d.argv, null);
          const env = d.assigns ? d.assigns.filter(a => !a.a).map(a => [a.n, a.v]) : null;
          externalStage(plan, s, argvStrings(d.argv), env && env.length ? env : null);
        }
        stageFds(plan, index, s, c, caps);
        if (errs[k]) ops.fd(plan, index, FD_DUP, 2, 1);
        if (d.redirs && !stageRedirs(plan, index, s, d.redirs)) failed.push(index);
      }
      const offset = i;
      tasks.push(finishPlan(c, plan, caps).then(result => {
        for (const index of failed) result[index] = 1;
        for (let k = 0; k < result.length; k++) statuses[offset + k] = result[k];
      }));
    }
    input = next;
    i = j;
  }
  const settled = await Promise.allSettled(tasks);
  for (const outcome of settled) if (outcome.status === 'rejected') throw outcome.reason;
  let status = statuses[n - 1];
  if (c.opts.pipefail) {
    for (let k = n - 1; k >= 0; k--) {
      if (statuses[k] !== 0) {
        status = statuses[k];
        break;
      }
    }
  }
  for (const post of posts) if (post) await runPost(post);
  setPipeStatus(c, statuses);
  return (c.status = status);
}

async function runJsStage(c, s, d, input, outEntry, mergeErr) {
  const body = d.js ? d.js : x => run(x, d.argv, d.assigns, d.redirs);
  try {
    if (s === c) {
      const saved = c.fds;
      c.fds = saved.slice();
      c.fds[0] = input;
      if (mergeErr) c.fds[2] = c.fds[1];
      try {
        await body(c);
        return c.status;
      } finally {
        c.fds = saved;
      }
    }
    s.fds[0] = input;
    if (outEntry) s.fds[1] = outEntry;
    if (mergeErr) s.fds[2] = s.fds[1];
    const status = await runSub(s, async x => {
      await body(x);
      if (outEntry && outEntry.buf) await flushEntry(outEntry);
    });
    return status;
  } finally {
    if (outEntry) {
      if (outEntry.buf) {
        try {
          await flushEntry(outEntry);
        } catch {}
      }
      ops.close(outEntry.fd);
    }
    if (input && input.pipeOwned) ops.close(input.fd);
  }
}

export function fatal(c, error) {
  const shellError = toShellError(error);
  if (!shellError) throw error;
  c.ne = 0;
  errorMessage(c, shellError.message);
  if (shellError.fatal) throw new ShellExit(127);
  if (c.opts.posix) throw new ShellExit(127);
  c.status = shellError.status;
}

export const R = {
  AT, ST, A,
  F: fields, S: str, Pt: pat, Re: re,
  get, isset, len, trim, repl, cased, substr, xform, ind, names, keys, setp, perr, tilde, arith, arithFail,
  cs, slurp, procsub, pushInterp, RT,
  run, pipe, D, J, redir, unredir, sub, bg, coproc, select, defun, ck, ret, Ret, setv, positional, assign, AW,
  tframe, tset, tpop, fillStream, findJob, line, verbose, match, cmatch, rematch, t1, t2, now, time, fatal
};
