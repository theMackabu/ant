// @ant: compact_on_build
import fs from 'node:fs';
import os from 'node:os';
import ops from 'ant:internal/shell_ops';
import { matchPattern } from 'ant:internal/shell/pattern';
import {
  registry, ShellExit, ShellReturn, ShellError, AssignWord, get, isset, setVar, setArray, arith, t1, t2,
  shellQuote, declareLine, decodeEscapes, evalString, which, clearWhich, resolvePath, runCommandOnly,
  rawByte, decodeBytes, fillStream, findJob, units, SHOPT_NAMES, enabledBuiltin, SIGNALS, signalNumber, signalJob, setSignalTrap
} from 'ant:internal/shell/runtime';

const encoder = new TextEncoder();

const RESERVED_WORDS = new Set([
  'if', 'then', 'else', 'elif', 'fi', 'case', 'esac', 'for', 'select', 'while', 'until', 'do', 'done',
  'in', 'function', 'time', '{', '}', '!', '[[', ']]', 'coproc'
]);

const SHOPT = SHOPT_NAMES;
const SHOPT_IGNORED = new Set(['inherit_errexit', 'sourcepath', 'checkwinsize', 'histappend', 'globskipdots', 'patsub_replacement', 'progcomp', 'promptvars', 'interactive_comments', 'cmdhist', 'complete_fullquote', 'extquote', 'force_fignore', 'hostcomplete']);

const SET_OPTIONS = {
  allexport: 'a', errexit: 'e', noglob: 'f', noclobber: 'C', nounset: 'u', xtrace: 'x', noexec: 'n', verbose: 'v', pipefail: 'pipefail', posix: 'posix'
};
const SET_IGNORED = new Set(['braceexpand', 'hashall', 'interactive-comments', 'monitor', 'notify', 'emacs', 'vi', 'ignoreeof', 'physical', 'privileged', 'functrace', 'errtrace', 'history', 'histexpand', 'keyword', 'onecmd']);

const builtins = registry.builtins;

function define(names, fn, flags = {}) {
  Object.assign(fn, flags);
  for (const name of names.split(' ')) builtins[name] = fn;
}

function argString(arg) {
  return typeof arg === 'string' ? arg : String(arg);
}

function isName(name) {
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(name);
}

function echoEscapes(text) {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch !== '\\' || i + 1 >= text.length) {
      out += ch;
      continue;
    }
    const n = text[i + 1];
    if (n === 'c') return { text: out, stop: true };
    if (n === '0') {
      const m = /^[0-7]{0,3}/.exec(text.slice(i + 2));
      out += rawByte(parseInt(m[0] || '0', 8));
      i += 1 + m[0].length;
      continue;
    }
    if (n >= '1' && n <= '7') {
      out += '\\' + n;
      i++;
      continue;
    }
    if (n === 'u') {
      const m = /^[0-9a-fA-F]{1,4}/.exec(text.slice(i + 2));
      if (m) {
        out += String.fromCodePoint(parseInt(m[0], 16));
        i += 1 + m[0].length;
        continue;
      }
    }
    const decoded = decodeEscapes('\\' + n + (n === 'x' ? text.slice(i + 2, i + 4) : ''));
    if (n === 'x') {
      const m = /^[0-9a-fA-F]{1,2}/.exec(text.slice(i + 2));
      if (m) {
        out += rawByte(parseInt(m[0], 16));
        i += 1 + m[0].length;
      } else {
        out += '\\x';
        i++;
      }
      continue;
    }
    out += decoded;
    i++;
  }
  return { text: out, stop: false };
}

define('echo', (c, argv, io) => {
  let i = 1;
  let newline = true;
  let escapes = c.opts.xpg_echo;
  while (i < argv.length && /^-[neE]+$/.test(argString(argv[i]))) {
    for (const flag of argString(argv[i]).slice(1)) {
      if (flag === 'n') newline = false;
      else if (flag === 'e') escapes = true;
      else escapes = false;
    }
    i++;
  }
  let text = '';
  for (let k = i; k < argv.length; k++) {
    if (k > i) text += ' ';
    text += argString(argv[k]);
  }
  if (escapes) {
    const r = echoEscapes(text);
    io.out(r.text);
    if (r.stop) return 0;
  } else io.out(text);
  if (newline) io.out('\n');
  return 0;
});

define(': true', () => 0);
define('false', () => 1);

function parseIntArg(text) {
  const s = text.trim();
  if (s === '') return { value: 0n, ok: true };
  if (s[0] === "'" || s[0] === '"') return { value: BigInt(s.length > 1 ? s.codePointAt(1) : 0), ok: true };
  let m = /^([+-]?)(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)$/.exec(s);
  if (m) {
    let digits = m[2];
    let value;
    if (/^0[xX]/.test(digits)) value = BigInt(digits);
    else if (digits.length > 1 && digits[0] === '0') value = BigInt('0o' + digits.slice(1));
    else value = BigInt(digits);
    return { value: m[1] === '-' ? -value : value, ok: true };
  }
  m = /^([+-]?[0-9]+)/.exec(s);
  return { value: m ? BigInt(m[1]) : 0n, ok: false };
}

function parseFloatArg(text) {
  const s = text.trim();
  if (s === '') return { value: 0, ok: true };
  if (s[0] === "'" || s[0] === '"') return { value: s.length > 1 ? s.codePointAt(1) : 0, ok: true };
  if (/^[+-]?(inf|infinity)$/i.test(s)) return { value: s[0] === '-' ? -Infinity : Infinity, ok: true };
  if (/^[+-]?nan$/i.test(s)) return { value: NaN, ok: true };
  if (/^[+-]?0[xX][0-9a-fA-F]+$/.test(s)) return { value: Number(BigInt(s.replace(/^[+-]/, ''))) * (s[0] === '-' ? -1 : 1), ok: true };
  const n = Number(s);
  if (!Number.isNaN(n)) return { value: n, ok: true };
  const m = /^[+-]?([0-9]*\.?[0-9]+([eE][+-]?[0-9]+)?)/.exec(s);
  return { value: m ? Number(m[0]) : 0, ok: false };
}

function expString(value, precision, upper) {
  let s = value.toExponential(precision);
  s = s.replace(/e([+-])(\d)$/, 'e$10$2');
  return upper ? s.toUpperCase() : s;
}

function formatFloat(conv, value, precision, alt) {
  if (!Number.isFinite(value)) {
    const s = Number.isNaN(value) ? 'nan' : value < 0 ? '-inf' : 'inf';
    return conv === conv.toUpperCase() ? s.toUpperCase() : s;
  }
  if (precision === null) precision = 6;
  switch (conv) {
    case 'f':
    case 'F':
      return value.toFixed(precision);
    case 'e':
    case 'E':
      return expString(value, precision, conv === 'E');
    case 'g':
    case 'G': {
      const p = precision === 0 ? 1 : precision;
      if (value === 0) return alt ? (0).toFixed(p - 1) : '0';
      const exp = Math.floor(Math.log10(Math.abs(Number(value.toExponential(p - 1)))));
      let s;
      if (exp < -4 || exp >= p) {
        s = expString(value, p - 1, conv === 'G');
        if (!alt) s = s.replace(/\.?0+([eE])/, '$1');
      } else {
        s = value.toFixed(Math.max(0, p - 1 - exp));
        if (!alt && s.includes('.')) s = s.replace(/\.?0+$/, '');
      }
      return s;
    }
    case 'a':
    case 'A':
      return value.toString(16);
  }
  return String(value);
}

function padUnits(c, text, width, left) {
  if (width === null) return text;
  const length = (units(c, text) ?? text).length;
  if (length >= width) return text;
  return left ? text + ' '.repeat(width - length) : ' '.repeat(width - length) + text;
}

function pad(text, width, left, zero) {
  if (width === null || text.length >= width) return text;
  if (left) return text + ' '.repeat(width - text.length);
  if (zero) {
    const sign = /^[+\- ]/.test(text) ? text[0] : '';
    const prefix = /^[+\- ]?0[xX]/.test(text) ? text.slice(0, sign.length + 2) : sign;
    return prefix + '0'.repeat(width - text.length) + text.slice(prefix.length);
  }
  return ' '.repeat(width - text.length) + text;
}

function formatEscapes(text, forB) {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch !== '\\' || i + 1 >= text.length) {
      out += ch;
      continue;
    }
    const n = text[i + 1];
    if (forB && n === 'c') return { text: out, stop: true };
    if (n >= '0' && n <= '7') {
      const max = forB && n === '0' ? 4 : 3;
      const m = new RegExp(`^[0-7]{1,${max}}`).exec(text.slice(i + 1));
      out += rawByte(parseInt(m[0], 8));
      i += m[0].length;
      continue;
    }
    if (n === 'x') {
      const m = /^[0-9a-fA-F]{1,2}/.exec(text.slice(i + 2));
      if (m) {
        out += rawByte(parseInt(m[0], 16));
        i += 1 + m[0].length;
        continue;
      }
    }
    if (n === 'u' || n === 'U') {
      const m = (n === 'u' ? /^[0-9a-fA-F]{1,4}/ : /^[0-9a-fA-F]{1,8}/).exec(text.slice(i + 2));
      if (m) {
        out += String.fromCodePoint(parseInt(m[0], 16));
        i += 1 + m[0].length;
        continue;
      }
    }
    const map = { a: '\x07', b: '\b', e: '\x1b', E: '\x1b', f: '\f', n: '\n', r: '\r', t: '\t', v: '\v', '\\': '\\', '"': '"', "'": "'", '?': '?' };
    if (map[n] !== undefined) {
      out += map[n];
      i++;
      continue;
    }
    out += '\\' + n;
    i++;
  }
  return { text: out, stop: false };
}

const startTime = Date.now();
const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

function strftime(format, d) {
  const two = n => String(n).padStart(2, '0');
  const dayOfYear = () => {
    const start = new Date(d.getFullYear(), 0, 1);
    return Math.floor((d - start) / 86400000) + 1;
  };
  const offset = () => {
    const minutes = -d.getTimezoneOffset();
    const sign = minutes >= 0 ? '+' : '-';
    const abs = Math.abs(minutes);
    return `${sign}${two(Math.floor(abs / 60))}${two(abs % 60)}`;
  };
  const zone = () => {
    try {
      return new Intl.DateTimeFormat('en-US', { timeZoneName: 'short' }).formatToParts(d).find(p => p.type === 'timeZoneName')?.value || offset();
    } catch {
      return offset();
    }
  };
  const hour12 = () => d.getHours() % 12 || 12;
  const week = first => {
    const jan1 = new Date(d.getFullYear(), 0, 1).getDay();
    const offsetDays = (7 + first - jan1) % 7;
    return two(Math.floor((dayOfYear() - 1 - offsetDays + 7) / 7));
  };
  let out = '';
  for (let i = 0; i < format.length; i++) {
    const ch = format[i];
    if (ch !== '%' || i + 1 >= format.length) {
      out += ch;
      continue;
    }
    const f = format[++i];
    switch (f) {
      case 'a': out += DAYS[d.getDay()].slice(0, 3); break;
      case 'A': out += DAYS[d.getDay()]; break;
      case 'b': case 'h': out += MONTHS[d.getMonth()].slice(0, 3); break;
      case 'B': out += MONTHS[d.getMonth()]; break;
      case 'c': out += `${DAYS[d.getDay()].slice(0, 3)} ${MONTHS[d.getMonth()].slice(0, 3)} ${String(d.getDate()).padStart(2)} ${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())} ${d.getFullYear()}`; break;
      case 'C': out += two(Math.floor(d.getFullYear() / 100)); break;
      case 'd': out += two(d.getDate()); break;
      case 'D': out += `${two(d.getMonth() + 1)}/${two(d.getDate())}/${two(d.getFullYear() % 100)}`; break;
      case 'e': out += String(d.getDate()).padStart(2); break;
      case 'F': out += `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())}`; break;
      case 'H': out += two(d.getHours()); break;
      case 'I': out += two(hour12()); break;
      case 'j': out += String(dayOfYear()).padStart(3, '0'); break;
      case 'k': out += String(d.getHours()).padStart(2); break;
      case 'l': out += String(hour12()).padStart(2); break;
      case 'm': out += two(d.getMonth() + 1); break;
      case 'M': out += two(d.getMinutes()); break;
      case 'n': out += '\n'; break;
      case 'p': out += d.getHours() < 12 ? 'AM' : 'PM'; break;
      case 'P': out += d.getHours() < 12 ? 'am' : 'pm'; break;
      case 'r': out += `${two(hour12())}:${two(d.getMinutes())}:${two(d.getSeconds())} ${d.getHours() < 12 ? 'AM' : 'PM'}`; break;
      case 'R': out += `${two(d.getHours())}:${two(d.getMinutes())}`; break;
      case 's': out += String(Math.floor(d.getTime() / 1000)); break;
      case 'S': out += two(d.getSeconds()); break;
      case 't': out += '\t'; break;
      case 'T': case 'X': out += `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`; break;
      case 'u': out += String(d.getDay() || 7); break;
      case 'U': out += week(0); break;
      case 'W': out += week(1); break;
      case 'w': out += String(d.getDay()); break;
      case 'x': out += `${two(d.getMonth() + 1)}/${two(d.getDate())}/${two(d.getFullYear() % 100)}`; break;
      case 'y': out += two(d.getFullYear() % 100); break;
      case 'Y': out += String(d.getFullYear()); break;
      case 'z': out += offset(); break;
      case 'Z': out += zone(); break;
      case '%': out += '%'; break;
      default: out += '%' + f;
    }
  }
  return out;
}

function printfFormat(c, format, args, io) {
  let out = '';
  let argi = 0;
  let status = 0;
  const next = () => (argi < args.length ? args[argi++] : null);
  const fmt = formatEscapes(format, false).text;
  for (;;) {
    let consumed = false;
    for (let i = 0; i < fmt.length; i++) {
      const ch = fmt[i];
      if (ch !== '%') {
        out += ch;
        continue;
      }
      if (fmt[i + 1] === '%') {
        out += '%';
        i++;
        continue;
      }
      const t = /^%(-?)([0-9]+)?(?:\.([0-9]+))?\(([^)]*)\)T/.exec(fmt.slice(i));
      if (t) {
        i += t[0].length - 1;
        consumed = true;
        const raw = next();
        const value = raw === null || raw === '' || raw === '-1' ? Date.now() / 1000 : raw === '-2' ? startTime / 1000 : Number(parseIntArg(raw).value);
        let text = strftime(t[4] || '%X', new Date(value * 1000));
        if (t[3] !== undefined) text = text.slice(0, Number(t[3]));
        out += pad(text, t[2] === undefined ? null : Number(t[2]), t[1] === '-', false);
        continue;
      }
      const m = /^%([-+ #0]*)(\*|[0-9]+)?(?:\.(\*|[0-9]*))?([hlLqjzt]*)([diouxXeEfFgGaAcsbq])/.exec(fmt.slice(i));
      if (!m) {
        io.error('printf', `${fmt.slice(i, i + 2)}: invalid format character`);
        return { out, status: 1, stop: true };
      }
      i += m[0].length - 1;
      consumed = true;
      const flags = m[1];
      let width = m[2] === undefined ? null : m[2] === '*' ? Number(parseIntArg(next() ?? '0').value) : Number(m[2]);
      let precision = m[3] === undefined ? null : m[3] === '*' ? Number(parseIntArg(next() ?? '0').value) : Number(m[3] || '0');
      let left = flags.includes('-');
      if (width !== null && width < 0) {
        left = true;
        width = -width;
      }
      const zero = flags.includes('0') && !left;
      const conv = m[5];
      const raw = next();
      const arg = raw === null ? '' : raw;
      let text;
      switch (conv) {
        case 'd':
        case 'i': {
          const r = parseIntArg(arg);
          if (!r.ok) {
            io.error('printf', `${arg}: invalid number`);
            status = 1;
          }
          let v = r.value;
          text = (v < 0n ? -v : v).toString();
          if (precision !== null) text = text.padStart(precision, '0');
          text = (v < 0n ? '-' : flags.includes('+') ? '+' : flags.includes(' ') ? ' ' : '') + text;
          text = pad(text, width, left, zero && precision === null);
          break;
        }
        case 'o':
        case 'u':
        case 'x':
        case 'X': {
          const r = parseIntArg(arg);
          if (!r.ok) {
            io.error('printf', `${arg}: invalid number`);
            status = 1;
          }
          let v = BigInt.asUintN(64, r.value);
          text = v.toString(conv === 'o' ? 8 : conv === 'u' ? 10 : 16);
          if (conv === 'X') text = text.toUpperCase();
          if (precision !== null) text = text.padStart(precision, '0');
          if (flags.includes('#') && v !== 0n) {
            if (conv === 'o' && !text.startsWith('0')) text = '0' + text;
            if (conv === 'x') text = '0x' + text;
            if (conv === 'X') text = '0X' + text;
          }
          text = pad(text, width, left, zero && precision === null);
          break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
          const r = parseFloatArg(arg);
          if (!r.ok) {
            io.error('printf', `${arg}: invalid number`);
            status = 1;
          }
          text = formatFloat(conv, r.value, precision, flags.includes('#'));
          if (r.value >= 0 && !text.startsWith('-')) {
            if (flags.includes('+')) text = '+' + text;
            else if (flags.includes(' ')) text = ' ' + text;
          }
          text = pad(text, width, left, zero && Number.isFinite(r.value));
          break;
        }
        case 'c':
          text = padUnits(c, (units(c, arg) ?? Array.from(arg))[0] ?? '', width, left);
          break;
        case 's':
          text = precision !== null ? (units(c, arg) ?? Array.from(arg)).slice(0, precision).join('') : arg;
          text = padUnits(c, text, width, left);
          break;
        case 'b': {
          const r = formatEscapes(arg, true);
          text = precision !== null ? r.text.slice(0, precision) : r.text;
          text = pad(text, width, left, false);
          if (r.stop) return { out: out + text, status, stop: true };
          break;
        }
        case 'q':
          text = pad(raw === null ? "''" : bashQuote(arg), width, left, false);
          break;
      }
      out += text;
    }
    if (!consumed || argi >= args.length) break;
  }
  return { out, status, stop: false };
}

function bashQuote(value) {
  if (value === '') return "''";
  if (/^[A-Za-z0-9_@%+=:,./-]+$/.test(value)) return value;
  if (/[\x00-\x1f\x7f]/.test(value)) return shellQuote(value);
  return value.replace(/([^A-Za-z0-9_@%+=:,./-])/g, '\\$1');
}

define('printf', (c, argv, io) => {
  let args = argv.slice(1).map(argString);
  let target = null;
  if (args[0] === '-v') {
    target = args[1];
    args = args.slice(2);
    if (!target || !/^[A-Za-z_][A-Za-z0-9_]*(\[.*\])?$/.test(target)) {
      io.error('printf', `\`${target ?? ''}': not a valid identifier`);
      return 2;
    }
  }
  if (args[0] === '--') args = args.slice(1);
  if (args.length === 0) {
    io.err('printf: usage: printf [-v var] format [arguments]\n');
    return 2;
  }
  const r = printfFormat(c, args[0], args.slice(1), io);
  if (target) setVar(c, target, undefined, r.out);
  else io.out(r.out);
  return r.status;
});

function normalizeLogical(path) {
  const absolute = path.startsWith('/');
  const parts = [];
  for (const part of path.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..') {
      parts.pop();
      continue;
    }
    parts.push(part);
  }
  return (absolute ? '/' : '') + parts.join('/') || '/';
}

function exportedUpdate(c, name, value) {
  const v = c.vars.find(name);
  setVar(c, name, undefined, value);
  if (v && v.x) c.vars.dirty = true;
}

function changeDirectory(c, io, target, physical, print) {
  let path;
  if (target.startsWith('/')) path = target;
  else {
    let base = c.cwd;
    if (!/^\.\.?(\/|$)/.test(target)) {
      const cdpath = isset(c, 'CDPATH', undefined, true) ? get(c, 'CDPATH') : '';
      for (const dir of cdpath.split(':')) {
        if (!dir) continue;
        const candidate = resolvePath(c, dir.endsWith('/') ? dir + target : dir + '/' + target);
        try {
          if (fs.statSync(candidate).isDirectory()) {
            path = candidate;
            print = true;
            break;
          }
        } catch {}
      }
    }
    if (!path) path = base.endsWith('/') ? base + target : base + '/' + target;
  }
  path = physical ? path : normalizeLogical(path);
  let st;
  try {
    st = fs.statSync(path);
  } catch (error) {
    io.error('cd', `${target}: ${error.code === 'ENOTDIR' ? 'Not a directory' : error.code === 'EACCES' ? 'Permission denied' : 'No such file or directory'}`);
    return 1;
  }
  if (!st.isDirectory()) {
    io.error('cd', `${target}: Not a directory`);
    return 1;
  }
  if (physical) {
    try {
      path = fs.realpathSync(path);
    } catch {}
  }
  const old = c.cwd;
  c.cwd = path;
  exportedUpdate(c, 'OLDPWD', old);
  exportedUpdate(c, 'PWD', path);
  if (print) io.out(path + '\n');
  return 0;
}

define('cd', (c, argv, io) => {
  let physical = false;
  let i = 1;
  for (; i < argv.length; i++) {
    const a = argString(argv[i]);
    if (a === '--') {
      i++;
      break;
    }
    if (!/^-[LPe@]+$/.test(a)) break;
    if (a.includes('P')) physical = true;
    if (a.includes('L')) physical = false;
  }
  if (argv.length - i > 1) {
    io.error('cd', 'too many arguments');
    return 1;
  }
  let target = i < argv.length ? argString(argv[i]) : null;
  let print = false;
  if (target === null) {
    if (!isset(c, 'HOME', undefined, false)) {
      const home = process.platform === 'win32' ? os.homedir() : null;
      if (!home) {
        io.error('cd', 'HOME not set');
        return 1;
      }
      target = home;
    } else target = get(c, 'HOME');
    if (target === '') return 0;
  } else if (target === '-') {
    if (!isset(c, 'OLDPWD', undefined, true)) {
      io.error('cd', 'OLDPWD not set');
      return 1;
    }
    target = get(c, 'OLDPWD');
    print = true;
  }
  return changeDirectory(c, io, target, physical, print);
});

define('pwd', (c, argv, io) => {
  const physical = argv.slice(1).some(a => argString(a) === '-P');
  let path = c.cwd;
  if (physical) {
    try {
      path = fs.realpathSync(path);
    } catch {}
  }
  io.out(path + '\n');
  return 0;
});

define('exit', (c, argv, io) => {
  let status = c.status;
  if (argv.length > 1) {
    const s = argString(argv[1]).trim();
    if (!/^[-+]?[0-9]+$/.test(s)) {
      io.error('exit', `${argString(argv[1])}: numeric argument required`);
      status = 2;
    } else status = Number(BigInt.asUintN(8, BigInt(s)));
  }
  throw new ShellExit(status);
});

define('return', (c, argv, io) => {
  let status = c.status;
  if (argv.length > 1) {
    const s = argString(argv[1]);
    if (!/^-?[0-9]+$/.test(s)) {
      io.error('return', `${s}: numeric argument required`);
      status = 2;
    } else status = Number(s) & 255;
  }
  throw new ShellReturn(status);
});

define('break continue', (c, argv, io) => {
  io.error(argString(argv[0]), "only meaningful in a `for', `while', or `until' loop");
  return 0;
});

function splitAssignText(text) {
  const m = /^([A-Za-z_][A-Za-z0-9_]*)(\[[^\]]*\])?(\+?)=([^]*)$/.exec(text);
  if (!m) return null;
  return { n: m[1], i: m[2] ? m[2].slice(1, -1) : undefined, v: m[4], a: null, p: m[3] === '+' };
}

function parseFlags(argv, allowed) {
  const on = new Set();
  const off = new Set();
  let i = 1;
  for (; i < argv.length; i++) {
    const a = argv[i];
    if (typeof a !== 'string') break;
    if (a === '--') {
      i++;
      break;
    }
    if (!/^[-+][A-Za-z]+$/.test(a)) break;
    for (const flag of a.slice(1)) {
      if (!allowed.includes(flag)) return { error: `${a[0]}${flag}: invalid option` };
      (a[0] === '-' ? on : off).add(flag);
    }
  }
  return { on, off, rest: argv.slice(i) };
}

function printDeclarations(c, io, filter) {
  for (const name of c.vars.names()) {
    const v = c.vars.find(name);
    if (!v || !filter(v)) continue;
    io.out(declareLine(name, v) + '\n');
  }
}

function applyAttributes(c, v, on, off, name) {
  if (on.has('a') && v.k === 0) {
    v.a = v.v === undefined ? [] : [v.v];
    v.v = undefined;
    v.k = 1;
  }
  if (on.has('A') && v.k !== 2) {
    if (v.k === 1 && v.a.length) throw new ShellError(`${name}: cannot convert indexed to associative array`);
    v.a = new Map();
    if (v.k === 0 && v.v !== undefined) v.a.set('0', v.v);
    v.v = undefined;
    v.k = 2;
  }
  if (on.has('i')) v.int = true;
  if (off.has('i')) v.int = false;
  if (on.has('l')) {
    v.lc = true;
    v.uc = false;
  }
  if (on.has('u')) {
    v.uc = true;
    v.lc = false;
  }
  if (off.has('l')) v.lc = false;
  if (off.has('u')) v.uc = false;
  if (on.has('x')) {
    v.x = true;
    c.vars.dirty = true;
  }
  if (off.has('x')) {
    if (v.x) c.vars.dirty = true;
    v.x = false;
  }
}

function declare(c, argv, io, mode) {
  const allowed = mode === 'export' ? 'fnp' : mode === 'readonly' ? 'aAfp' : 'aAfFgilnprtux';
  const parsed = parseFlags(argv, allowed);
  if (parsed.error) {
    io.error(argString(argv[0]), parsed.error);
    return 2;
  }
  const { on, off, rest } = parsed;
  if (mode === 'export' && !on.has('n')) on.add('x');
  if (mode === 'export' && on.has('n')) {
    on.delete('n');
    on.delete('x');
    off.add('x');
  }
  if (mode === 'readonly') on.add('r');
  if (on.has('f') || on.has('F')) {
    const list = rest.length ? rest.map(argString) : c.funcs.names();
    let status = 0;
    for (const name of list) {
      const fn = c.funcs.get(name);
      if (!fn) {
        status = 1;
        continue;
      }
      if (on.has('F')) io.out(rest.length ? `${name}\n` : `declare -f ${name}\n`);
      else io.out(`${fn.text}\n`);
    }
    return status;
  }
  const local = mode === 'local' || (mode === 'declare' && c.fstack.length > 0 && !on.has('g'));
  if (mode === 'local' && c.fstack.length === 0) {
    io.error('local', 'can only be used in a function');
    return 1;
  }
  if (rest.length === 0 || on.has('p')) {
    if (rest.length) {
      let status = 0;
      for (const arg of rest) {
        const name = argString(arg);
        const v = c.vars.findRaw(name);
        if (!v) {
          io.error(argString(argv[0]), `${name}: not found`);
          status = 1;
          continue;
        }
        io.out(declareLine(name, v) + '\n');
      }
      return status;
    }
    const filter = v => (!on.has('x') || v.x) && (!on.has('r') || v.r) && (!on.has('a') || v.k === 1) && (!on.has('A') || v.k === 2) && (!on.has('i') || v.int);
    printDeclarations(c, io, filter);
    return 0;
  }
  let status = 0;
  for (const arg of rest) {
    if (arg === '-' && local) {
      const frame = c.vars.frames[c.vars.frames.length - 1];
      if (!frame.savedOpts) frame.savedOpts = Object.assign({}, c.opts);
      continue;
    }
    let spec = arg instanceof AssignWord ? arg : splitAssignText(argString(arg));
    const name = spec ? spec.n : argString(arg);
    if (!isName(name)) {
      io.error(argString(argv[0]), `\`${argString(arg)}': not a valid identifier`);
      status = 1;
      continue;
    }
    if (on.has('n') || off.has('n')) {
      let raw = local ? c.vars.local(name) : c.vars.findRaw(name);
      if (!raw) {
        raw = c.vars.frames[0].get(name);
        if (!raw || raw.t) {
          c.vars.writable(name);
          raw = c.vars.findRaw(name);
        }
      }
      if (off.has('n')) {
        raw.nref = false;
        continue;
      }
      if (spec && spec.v !== undefined && spec.v !== null && spec.v !== '' && !/^[A-Za-z_][A-Za-z0-9_]*(\[.*\])?$/.test(spec.v)) {
        io.error(argString(argv[0]), `\`${spec.v}': invalid variable name for name reference`);
        status = 1;
        continue;
      }
      if (spec && spec.v === name) {
        io.error(argString(argv[0]), `${name}: nameref variable self references not allowed`);
        status = 1;
        continue;
      }
      raw.nref = true;
      raw.k = 0;
      if (spec) raw.v = spec.v;
      if (on.has('r')) raw.r = true;
      continue;
    }
    let v;
    if (local) {
      const existing = c.vars.find(name);
      v = c.vars.local(name);
      if (!spec && existing && v !== existing && v.v === undefined && v.k === 0 && !on.has('a') && !on.has('A')) {
        if (existing.x) v.x = true;
      }
    } else v = c.vars.writable(name);
    if (v.r && (spec || on.size > 1 || !on.has('r'))) {
      if (spec || !on.has('r')) {
        io.error(argString(argv[0]), `${name}: readonly variable`);
        status = 1;
        continue;
      }
    }
    try {
      applyAttributes(c, v, on, off, name);
      if (spec) {
        if (spec.a) setArray(c, name, spec.a, spec.p, v);
        else setVar(c, name, spec.i, spec.v, spec.p, v);
        if (v.x) c.vars.dirty = true;
      }
    } catch (error) {
      if (!(error instanceof ShellError)) throw error;
      io.error(argString(argv[0]), error.message);
      status = 1;
      continue;
    }
    if (on.has('r')) v.r = true;
  }
  return status;
}

define('declare typeset', (c, argv, io) => declare(c, argv, io, 'declare'));
define('local', (c, argv, io) => declare(c, argv, io, 'local'));
define('export', (c, argv, io) => declare(c, argv, io, 'export'));
define('readonly', (c, argv, io) => declare(c, argv, io, 'readonly'));

define('unset', (c, argv, io) => {
  const parsed = parseFlags(argv, 'fvn');
  if (parsed.error) {
    io.error('unset', parsed.error);
    return 2;
  }
  let status = 0;
  for (const arg of parsed.rest) {
    const name = argString(arg);
    if (parsed.on.has('f')) {
      c.funcs.delete(name);
      continue;
    }
    const m = /^([A-Za-z_][A-Za-z0-9_]*)\[(.*)\]$/.exec(name);
    if (m) {
      const v = c.vars.find(m[1]);
      if (v && v.r) {
        io.error('unset', `${m[1]}: cannot unset: readonly variable`);
        status = 1;
        continue;
      }
      if (v) {
        const own = c.vars.writable(m[1]);
        if (m[2] === '@' || m[2] === '*') c.vars.unset(m[1]);
        else if (own.k === 2) own.a.delete(m[2]);
        else if (own.k === 1) {
          let index = arith(c, m[2]);
          if (index < 0) index += own.a.length;
          delete own.a[index];
        } else if (arith(c, m[2]) === 0) own.v = undefined;
      }
      continue;
    }
    if (!isName(name)) {
      io.error('unset', `\`${name}': not a valid identifier`);
      status = 1;
      continue;
    }
    const v = c.vars.find(name);
    if (!v && !parsed.on.has('v') && c.funcs.get(name)) {
      c.funcs.delete(name);
      continue;
    }
    if (!c.vars.unset(name, parsed.on.has('n'))) {
      io.error('unset', `${name}: cannot unset: readonly variable`);
      status = 1;
    }
  }
  return status;
});

function optionsString(c) {
  const lines = [];
  const names = ['allexport', 'braceexpand', 'errexit', 'hashall', 'interactive-comments', 'noclobber', 'noexec', 'noglob', 'nounset', 'pipefail', 'posix', 'verbose', 'xtrace'];
  for (const name of names) {
    const key = SET_OPTIONS[name];
    const on = key ? !!c.opts[key] : name === 'braceexpand' || name === 'hashall' || name === 'interactive-comments';
    lines.push(`${name.padEnd(15)}\t${on ? 'on' : 'off'}`);
  }
  return lines.join('\n') + '\n';
}

define('set', (c, argv, io) => {
  if (argv.length === 1) {
    for (const name of c.vars.names()) {
      const v = c.vars.find(name);
      if (!v) continue;
      if (v.k === 0) {
        if (v.v !== undefined) io.out(`${name}=${shellQuote(v.v).replace(/^'([A-Za-z0-9_@%+=:,./-]*)'$/, '$1')}\n`);
      } else io.out(declareLine(name, v).replace(/^declare -\S+ /, '') + '\n');
    }
    return 0;
  }
  let i = 1;
  let setPositional = false;
  for (; i < argv.length; i++) {
    const a = argString(argv[i]);
    if (a === '--') {
      i++;
      setPositional = true;
      break;
    }
    if (a === '-') {
      c.opts.x = false;
      c.opts.v = false;
      i++;
      setPositional = i < argv.length;
      break;
    }
    if (!/^[-+]/.test(a) || a.length < 2) {
      setPositional = true;
      break;
    }
    const enable = a[0] === '-';
    for (let k = 1; k < a.length; k++) {
      const flag = a[k];
      if (flag === 'o') {
        const name = i + 1 < argv.length ? argString(argv[i + 1]) : null;
        if (name === null || /^[-+]/.test(name)) {
          if (enable) io.out(optionsString(c));
          else for (const [option, key] of Object.entries(SET_OPTIONS)) io.out(`set ${c.opts[key] ? '-' : '+'}o ${option}\n`);
          continue;
        }
        i++;
        if (SET_OPTIONS[name]) {
          c.opts[SET_OPTIONS[name]] = enable;
          if (name === 'posix') c.opts.expand_aliases = c.opts.inherit_errexit = c.opts.shift_verbose = enable;
        } else if (!SET_IGNORED.has(name)) {
          io.error('set', `${name}: invalid option name`);
          return 2;
        }
        continue;
      }
      if ('aefuxCnvhBHmbPTEk'.includes(flag)) {
        if ('aefuxCnv'.includes(flag)) c.opts[flag] = enable;
        continue;
      }
      io.error('set', `${a[0]}${flag}: invalid option`);
      return 2;
    }
  }
  if (setPositional) c.pos = argv.slice(i).map(argString);
  return 0;
});

define('shift', (c, argv, io) => {
  let n = 1;
  if (argv.length > 1) {
    const s = argString(argv[1]);
    if (!/^[0-9]+$/.test(s)) {
      io.error('shift', `${s}: numeric argument required`);
      return 1;
    }
    n = Number(s);
  }
  if (n > c.pos.length) {
    if (c.opts.shift_verbose) io.error('shift', `${n}: shift count out of range`);
    return 1;
  }
  c.pos = c.pos.slice(n);
  return 0;
});

const UNARY = new Set(['-a', '-b', '-c', '-d', '-e', '-f', '-g', '-h', '-k', '-p', '-r', '-s', '-t', '-u', '-w', '-x', '-G', '-L', '-N', '-O', '-S', '-z', '-n', '-o', '-v', '-R']);
const BINARY = new Set(['=', '==', '!=', '<', '>', '-eq', '-ne', '-lt', '-le', '-gt', '-ge', '-nt', '-ot', '-ef']);

function testEval(c, args) {
  const unary = (op, x) => (op === '-t' ? false : t1(c, op, x));
  const binary = (a, op, b) => t2(c, op, a, b, false);
  const n = args.length;
  if (n === 0) return false;
  if (n === 1) return args[0] !== '';
  if (n === 2) {
    if (args[0] === '!') return args[1] === '';
    if (UNARY.has(args[0])) return unary(args[0], args[1]);
    throw new ShellError(`${args[0]}: unary operator expected`, 2);
  }
  if (n === 3) {
    if (BINARY.has(args[1])) return binary(args[0], args[1], args[2]);
    if (args[1] === '-a') return args[0] !== '' && args[2] !== '';
    if (args[1] === '-o') return args[0] !== '' || args[2] !== '';
    if (args[0] === '!') return !testEval(c, args.slice(1));
    if (args[0] === '(' && args[2] === ')') return args[1] !== '';
    throw new ShellError(`${args[1]}: binary operator expected`, 2);
  }
  if (n === 4) {
    if (args[0] === '!') return !testEval(c, args.slice(1));
    if (args[0] === '(' && args[3] === ')') return testEval(c, args.slice(1, 3));
  }
  let i = 0;
  const orExpr = () => {
    let v = andExpr();
    while (args[i] === '-o') {
      i++;
      const r = andExpr();
      v = v || r;
    }
    return v;
  };
  const andExpr = () => {
    let v = notExpr();
    while (args[i] === '-a') {
      i++;
      const r = notExpr();
      v = v && r;
    }
    return v;
  };
  const notExpr = () => {
    if (args[i] === '!') {
      i++;
      return !notExpr();
    }
    return primary();
  };
  const primary = () => {
    if (i >= n) throw new ShellError('argument expected', 2);
    if (args[i] === '(') {
      i++;
      const v = orExpr();
      if (args[i] !== ')') throw new ShellError("`)' expected", 2);
      i++;
      return v;
    }
    if (i + 2 < n + 1 && BINARY.has(args[i + 1]) && i + 2 < n) {
      const v = binary(args[i], args[i + 1], args[i + 2]);
      i += 3;
      return v;
    }
    if (UNARY.has(args[i]) && i + 1 < n) {
      const v = unary(args[i], args[i + 1]);
      i += 2;
      return v;
    }
    return args[i++] !== '';
  };
  const v = orExpr();
  if (i < n) throw new ShellError(`${args[i]}: too many arguments`, 2);
  return v;
}

function runTest(c, args, io, name) {
  try {
    return testEval(c, args) ? 0 : 1;
  } catch (error) {
    if (!(error instanceof ShellError)) throw error;
    io.error(name, error.message);
    return 2;
  }
}

define('test', (c, argv, io) => runTest(c, argv.slice(1).map(argString), io, 'test'));
define('[', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  if (args[args.length - 1] !== ']') {
    io.error('[', "missing `]'");
    return 2;
  }
  return runTest(c, args.slice(0, -1), io, '[');
});

class Reader {
  constructor(entry) {
    this.entry = entry;
    this.eof = !entry || (entry.k !== 'bytes' && entry.k !== 'host' && entry.k !== 'stream');
  }

  byte() {
    const e = this.entry;
    if (this.eof) return -1;
    if (e.k === 'bytes' || e.k === 'stream') {
      if (e.pos >= e.data.length) {
        this.eof = true;
        return -1;
      }
      return e.data[e.pos++];
    }
    const buf = Buffer.alloc(1);
    let n = 0;
    try {
      n = fs.readSync(e.fd, buf, 0, 1, null);
    } catch {
      n = 0;
    }
    if (n <= 0) {
      this.eof = true;
      return -1;
    }
    return buf[0];
  }

  until(delim, maxChars, raw) {
    const e = this.entry;
    if (this.eof) return null;
    if ((e.k === 'bytes' || e.k === 'stream') && maxChars < 0 && delim >= 0) {
      if (e.pos >= e.data.length) {
        this.eof = true;
        return null;
      }
      let end = e.data.indexOf(delim, e.pos);
      const found = end !== -1;
      if (!found) end = e.data.length;
      if (!raw && e.data.subarray(e.pos, end).indexOf(92) !== -1) return this.slow(delim, maxChars, raw);
      const text = decodeBytes(e.data.subarray(e.pos, end));
      e.pos = found ? end + 1 : end;
      if (!found) this.eof = true;
      return { chars: Array.from(text), escaped: null, found };
    }
    return this.slow(delim, maxChars, raw);
  }

  slow(delim, maxChars, raw) {
    const bytes = [];
    const escaped = new Set();
    let found = false;
    let chars = 0;
    for (;;) {
      if (maxChars >= 0 && chars >= maxChars) break;
      const b = this.byte();
      if (b < 0) break;
      if (b === delim && !(maxChars >= 0 && delim === -2)) {
        found = true;
        break;
      }
      if (!raw && b === 92) {
        const n = this.byte();
        if (n < 0) break;
        if (n === 10) continue;
        escaped.add(bytes.length);
        bytes.push(n);
        chars++;
        continue;
      }
      bytes.push(b);
      if ((b & 0xc0) !== 0x80) chars++;
    }
    if (!found && bytes.length === 0 && this.eof) return null;
    const text = decodeBytes(Uint8Array.from(bytes));
    const list = Array.from(text);
    let esc = null;
    if (escaped.size) {
      esc = new Set();
      let byteIndex = 0;
      list.forEach((ch, index) => {
        if (escaped.has(byteIndex)) esc.add(index);
        byteIndex += encoder.encode(ch).length;
      });
    }
    return { chars: list, escaped: esc, found };
  }
}

function ifsValue(c) {
  if (!isset(c, 'IFS', undefined, false)) return ' \t\n';
  return get(c, 'IFS');
}

function splitRead(chars, escaped, ifs, count) {
  const isWs = ch => ch === ' ' || ch === '\t' || ch === '\n';
  const isIfs = (ch, i) => !(escaped && escaped.has(i)) && ifs.includes(ch);
  const isIfsWs = (ch, i) => isIfs(ch, i) && isWs(ch);
  const fields = [];
  let i = 0;
  const n = chars.length;
  while (i < n && isIfsWs(chars[i], i)) i++;
  while (i < n) {
    if (count > 0 && fields.length === count - 1) {
      let end = n;
      while (end > i && isIfsWs(chars[end - 1], end - 1)) end--;
      fields.push(chars.slice(i, end).join(''));
      return fields;
    }
    let field = '';
    while (i < n && !isIfs(chars[i], i)) field += chars[i++];
    fields.push(field);
    if (i >= n) break;
    let sawNonWs = false;
    while (i < n && isIfs(chars[i], i)) {
      if (!isWs(chars[i])) {
        if (sawNonWs) break;
        sawNonWs = true;
      }
      i++;
      if (sawNonWs && i < n && isIfs(chars[i], i) && !isWs(chars[i])) break;
    }
  }
  return fields;
}

define('read', (c, argv, io) => {
  let raw = false;
  let arrayName = null;
  let delim = 10;
  let count = -1;
  let fd = 0;
  let exact = false;
  let timeout = -1;
  let i = 1;
  for (; i < argv.length; i++) {
    const a = argString(argv[i]);
    if (a === '--') {
      i++;
      break;
    }
    if (!a.startsWith('-') || a === '-') break;
    for (let k = 1; k < a.length; k++) {
      const flag = a[k];
      const value = () => {
        const rest = a.slice(k + 1);
        k = a.length;
        if (rest) return rest;
        return argString(argv[++i] ?? '');
      };
      switch (flag) {
        case 'r': raw = true; break;
        case 's': case 'e': break;
        case 'a': arrayName = value(); break;
        case 'd': {
          const d = value();
          delim = d === '' ? 0 : encoder.encode(d)[0];
          break;
        }
        case 'n': count = Number(value()); break;
        case 'N': count = Number(value()); exact = true; break;
        case 'p': value(); break;
        case 't': timeout = Number(value()); break;
        case 'u': fd = Number(value()); break;
        case 'i': value(); break;
        default:
          io.error('read', `-${flag}: invalid option`);
          return 2;
      }
    }
  }
  const names = argv.slice(i).map(argString);
  for (const name of names) {
    if (!isName(name)) {
      io.error('read', `\`${name}': not a valid identifier`);
      return 1;
    }
  }
  if (fd < 0 || fd > 255 || !c.fds[fd] && fd !== 0) {
    io.error('read', `${fd}: invalid file descriptor: Bad file descriptor`);
    return 1;
  }
  const entry = c.fds[fd];
  const options = { exact, delim, count, raw, arrayName, names };
  if (timeout === 0) {
    const ready = entry && ((entry.k === 'bytes' && entry.pos < entry.data.length) || (entry.k === 'stream' && (entry.pos < entry.data.length || entry.eof)));
    return ready ? 0 : 1;
  }
  if (entry && entry.k === 'stream' && !entry.eof && !(count < 0 && !exact && delim >= 0 && entry.data.indexOf(delim, entry.pos) !== -1)) {
    const filled = fillStream(c, entry, exact ? -1 : delim, count >= 0 ? count * 4 : 0);
    if (timeout > 0 && Number.isFinite(timeout)) {
      let timer;
      const expired = new Promise(resolve => (timer = setTimeout(() => resolve(false), timeout * 1000)));
      return Promise.race([filled.then(() => true), expired]).then(ok => {
        clearTimeout(timer);
        if (!ok) {
          if (names.length) for (const name of names) setVar(c, name, undefined, '');
          else if (!arrayName) setVar(c, 'REPLY', undefined, '');
          return 142;
        }
        return finishRead(c, entry, options);
      });
    }
    return filled.then(() => finishRead(c, entry, options));
  }
  return finishRead(c, entry, options);
}, { stdin: true });

function finishRead(c, entry, { exact, delim, count, raw, arrayName, names }) {
  const reader = new Reader(entry);
  const line = reader.until(exact ? -2 : delim, count, raw);
  if (line === null) {
    if (arrayName) setArray(c, arrayName, [], false);
    else if (names.length === 0) setVar(c, 'REPLY', undefined, '');
    else for (const name of names) setVar(c, name, undefined, '');
    return 1;
  }
  const status = line.found || exact ? 0 : 1;
  const ifs = ifsValue(c);
  if (arrayName) {
    const parts = splitRead(line.chars, line.escaped, ifs, 0);
    setArray(c, arrayName, parts.map(p => [null, p, 0]), false);
    return status;
  }
  if (names.length === 0) {
    setVar(c, 'REPLY', undefined, line.chars.join(''));
    return status;
  }
  const parts = exact ? [line.chars.join('')] : splitRead(line.chars, line.escaped, ifs, names.length);
  names.forEach((name, index) => setVar(c, name, undefined, parts[index] ?? ''));
  return status;
}

define('mapfile readarray', (c, argv, io) => {
  let strip = false;
  let delim = 10;
  let limit = 0;
  let origin = 0;
  let skip = 0;
  let fd = 0;
  let i = 1;
  for (; i < argv.length; i++) {
    const a = argString(argv[i]);
    if (!a.startsWith('-') || a === '-') break;
    if (a === '--') {
      i++;
      break;
    }
    const flag = a[1];
    if (flag === 't') {
      strip = true;
      continue;
    }
    if (flag === 'C' || flag === 'c') {
      if (a.length === 2) i++;
      continue;
    }
    const value = a.length > 2 ? a.slice(2) : argString(argv[++i] ?? '');
    switch (flag) {
      case 'd': delim = value === '' ? 0 : encoder.encode(value)[0]; break;
      case 'n': limit = Number(value); break;
      case 'O': origin = Number(value); break;
      case 's': skip = Number(value); break;
      case 'u': fd = Number(value); break;
      default:
        io.error(argString(argv[0]), `-${flag}: invalid option`);
        return 2;
    }
  }
  const name = argv[i] !== undefined ? argString(argv[i]) : 'MAPFILE';
  const entry = c.fds[fd];
  if (entry && entry.k === 'stream' && !entry.eof) {
    return fillStream(c, entry, -1, 0).then(() => finishMapfile(c, entry, { name, strip, delim, limit, origin, skip }));
  }
  return finishMapfile(c, entry, { name, strip, delim, limit, origin, skip });
}, { stdin: true });

function finishMapfile(c, entry, { name, strip, delim, limit, origin, skip }) {
  const reader = new Reader(entry);
  const items = [];
  let index = origin;
  let read = 0;
  for (;;) {
    if (limit && items.length >= limit) break;
    const line = reader.until(delim, -1, true);
    if (line === null) break;
    read++;
    if (read <= skip) continue;
    let text = line.chars.join('');
    if (!strip && line.found) text += String.fromCharCode(delim);
    items.push([String(index++), text, 0]);
  }
  const v = c.vars.writable(name);
  if (origin === 0) {
    v.k = 1;
    v.a = [];
    v.v = undefined;
  }
  setArray(c, name, items, true);
  return 0;
}

define('eval', async (c, argv) => evalString(c, argv.slice(1).map(argString).join(' ')), { stdin: true });

async function sourceFile(c, argv, io) {
  if (argv.length < 2) {
    io.error(argString(argv[0]), 'filename argument required');
    return 2;
  }
  const name = argString(argv[1]);
  let path = null;
  if (name.includes('/')) path = resolvePath(c, name);
  else {
    for (const dir of get(c, 'PATH').split(':')) {
      const candidate = resolvePath(c, (dir || '.') + '/' + name);
      try {
        if (fs.statSync(candidate).isFile()) {
          path = candidate;
          break;
        }
      } catch {}
    }
    if (!path) path = resolvePath(c, name);
  }
  let text;
  try {
    text = decodeBytes(await fs.promises.readFile(path));
  } catch {
    io.error(argString(argv[0]), `${name}: No such file or directory`);
    return 1;
  }
  const savedPos = c.pos;
  if (argv.length > 2) c.pos = argv.slice(2).map(argString);
  try {
    await io.flush();
    return await evalString(c, text, name);
  } catch (error) {
    if (error instanceof ShellReturn) return (c.status = error.status);
    throw error;
  } finally {
    if (argv.length > 2) c.pos = savedPos;
  }
}

define('source .', sourceFile, { stdin: true });

define('exec', async (c, argv, io) => {
  let i = 1;
  while (i < argv.length && /^-[cl]$/.test(argString(argv[i]))) i++;
  if (argString(argv[i] ?? '') === '-a') i += 2;
  if (i >= argv.length) return 0;
  await io.flush();
  const status = await runCommandOnly(c, argv.slice(i), null, null, false);
  throw new ShellExit(status);
}, { stdin: true });

define('wait', async (c, argv, io) => {
  const jobs = c.root.jobs ?? new Map();
  let args = argv.slice(1).map(argString);
  let next = false;
  while (args[0] === '-n' || args[0] === '-f') {
    if (args[0] === '-n') next = true;
    args = args.slice(1);
  }
  if (next) {
    const pending = [...jobs.values()].filter(job => !job.done);
    const done = [...jobs.values()].filter(job => job.done);
    if (done.length) {
      jobs.delete(done[0].n);
      return done[0].status;
    }
    if (!pending.length) return 127;
    const job = await Promise.race(pending.map(job => job.promise.then(() => job)));
    jobs.delete(job.n);
    return job.status;
  }
  if (args.length === 0) {
    for (const job of [...jobs.values()]) await job.promise;
    jobs.clear();
    return 0;
  }
  let status = 0;
  for (const spec of args) {
    const job = findJob(c, spec);
    if (!job) {
      io.error('wait', /^[0-9]+$/.test(spec) ? `pid ${spec} is not a child of this shell` : `${spec}: no such job`);
      status = 127;
      continue;
    }
    status = await job.promise;
    jobs.delete(job.n);
  }
  return status;
}, { stdin: true });

function jobState(job) {
  if (!job.done) return 'Running';
  return job.status === 0 ? 'Done' : job.status > 128 && SIGNALS[job.status - 128] ? signalDescription(SIGNALS[job.status - 128]) : `Exit ${job.status}`;
}

function signalDescription(name) {
  return { TERM: 'Terminated', KILL: 'Killed', INT: 'Interrupt', HUP: 'Hangup', QUIT: 'Quit', PIPE: 'Broken pipe', USR1: 'User defined signal 1', USR2: 'User defined signal 2', ALRM: 'Alarm clock' }[name] || name;
}

define('jobs', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  const jobs = [...(c.root.jobs ?? new Map()).values()];
  const pidsOnly = args.includes('-p');
  const long = args.includes('-l');
  const running = args.includes('-r');
  const stopped = args.includes('-s');
  const specs = args.filter(a => !a.startsWith('-'));
  const list = specs.length ? specs.map(spec => findJob(c, spec)).filter(Boolean) : jobs;
  list.forEach(job => {
    if (running && job.done) return;
    if (stopped) return;
    if (pidsOnly) {
      io.out(`${job.pid}\n`);
      return;
    }
    const index = jobs.indexOf(job);
    const mark = index === jobs.length - 1 ? '+' : index === jobs.length - 2 ? '-' : ' ';
    const state = jobState(job);
    io.out(`[${job.n}]${mark}  ${long ? job.pid + ' ' : ''}${state.padEnd(24)}${job.text}${job.done ? '' : ' &'}\n`);
  });
  for (const job of jobs) if (job.done && !specs.length) c.root.jobs.delete(job.n);
  return 0;
});

define('disown', (c, argv, io) => {
  const args = argv.slice(1).map(argString).filter(a => !a.startsWith('-'));
  const jobs = c.root.jobs;
  if (!jobs) return args.length ? 1 : 0;
  if (argv.slice(1).map(argString).includes('-a')) {
    jobs.clear();
    return 0;
  }
  const specs = args.length ? args : ['%%'];
  let status = 0;
  for (const spec of specs) {
    const job = findJob(c, spec);
    if (!job) {
      io.error('disown', `${spec}: no such job`);
      status = 1;
      continue;
    }
    jobs.delete(job.n);
  }
  return status;
});

define('fg bg', (c, argv, io) => {
  io.error(argString(argv[0]), 'no job control');
  return 1;
});

define('suspend', (c, argv, io) => {
  io.error('suspend', 'cannot suspend a shell without job control');
  return 1;
});

function parseSignal(spec) {
  const s = spec.toUpperCase().replace(/^SIG/, '');
  if (/^[0-9]+$/.test(s)) {
    const n = Number(s);
    if (n === 0) return '0';
    return SIGNALS[n] || null;
  }
  return SIGNALS.includes(s) ? s : null;
}

define('kill', (c, argv, io) => {
  let args = argv.slice(1).map(argString);
  let signal = 'TERM';
  if (args[0] === '-l' || args[0] === '-L') {
    if (args.length > 1) {
      for (const spec of args.slice(1)) {
        const n = Number(spec);
        if (/^[0-9]+$/.test(spec)) {
          const name = SIGNALS[n > 128 ? n - 128 : n];
          if (!name) {
            io.error('kill', `${spec}: invalid signal specification`);
            return 1;
          }
          io.out(name + '\n');
        } else {
          const name = parseSignal(spec);
          if (!name) {
            io.error('kill', `${spec}: invalid signal specification`);
            return 1;
          }
          io.out(signalNumber(name) + '\n');
        }
      }
      return 0;
    }
    const names = SIGNALS.slice(1);
    let line = '';
    names.forEach((name, i) => {
      line += `${String(i + 1).padStart(2)}) SIG${name}`;
      line += (i + 1) % 5 === 0 || i === names.length - 1 ? '\n' : '\t';
    });
    io.out(line);
    return 0;
  }
  if (args[0] === '-s' || args[0] === '-n') {
    signal = parseSignal(args[1] || '');
    args = args.slice(2);
  } else if (args[0] && /^-/.test(args[0]) && args[0] !== '--') {
    signal = parseSignal(args[0].slice(1));
    args = args.slice(1);
  }
  if (args[0] === '--') args = args.slice(1);
  if (!signal) {
    io.error('kill', `${argString(argv[1])}: invalid signal specification`);
    return 1;
  }
  if (!args.length) {
    io.err('kill: usage: kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]\n');
    return 2;
  }
  let status = 0;
  for (const target of args) {
    const job = target.startsWith('%') || /^[0-9]+$/.test(target) ? findJob(c, target) : null;
    if (target.startsWith('%') && !job) {
      io.error('kill', `${target}: no such job`);
      status = 1;
      continue;
    }
    if (job) {
      if (!signalJob(job, signal) && signal !== '0') {
        io.error('kill', `${target}: no such process`);
        status = 1;
      }
      continue;
    }
    if (!/^-?[0-9]+$/.test(target)) {
      io.error('kill', `${target}: arguments must be process or job IDs`);
      status = 1;
      continue;
    }
    try {
      process.kill(Number(target), signal === '0' ? 0 : 'SIG' + signal);
    } catch (error) {
      io.error('kill', `(${target}) - ${error && error.code === 'EPERM' ? 'Operation not permitted' : 'No such process'}`);
      status = 1;
    }
  }
  return status;
});

function describe(c, name) {
  if (RESERVED_WORDS.has(name)) return { kind: 'keyword', text: `${name} is a shell keyword` };
  const fn = c.funcs.get(name);
  if (fn) return { kind: 'function', text: `${name} is a function\n${fn.text}` };
  if (enabledBuiltin(c, name)) return { kind: 'builtin', text: `${name} is a shell builtin` };
  const path = which(c, name);
  if (path && (!name.includes('/') || fs.existsSync(path))) return { kind: 'file', text: `${name} is ${path}`, path };
  return null;
}

define('type', (c, argv, io) => {
  const parsed = parseFlags(argv, 'afptP');
  if (parsed.error) {
    io.error('type', parsed.error);
    return 2;
  }
  let status = 0;
  for (const arg of parsed.rest) {
    const name = argString(arg);
    const d = parsed.on.has('P') ? (which(c, name) ? { kind: 'file', path: which(c, name) } : null) : describe(c, name);
    if (!d) {
      if (!parsed.on.has('t') && !parsed.on.has('p') && !parsed.on.has('P')) io.error('type', `${name}: not found`);
      status = 1;
      continue;
    }
    if (parsed.on.has('t')) io.out(d.kind + '\n');
    else if (parsed.on.has('p') || parsed.on.has('P')) {
      if (d.kind === 'file') io.out(d.path + '\n');
    } else io.out(d.text + '\n');
  }
  return status;
});

define('command', async (c, argv, io) => {
  let i = 1;
  let verbose = '';
  for (; i < argv.length; i++) {
    const a = argString(argv[i]);
    if (a === '--') {
      i++;
      break;
    }
    if (!/^-[pvV]+$/.test(a)) break;
    if (a.includes('v')) verbose = 'v';
    if (a.includes('V')) verbose = 'V';
  }
  if (i >= argv.length) return 0;
  if (verbose) {
    let status = 0;
    for (const arg of argv.slice(i)) {
      const name = argString(arg);
      const d = describe(c, name);
      if (!d) {
        if (verbose === 'V') io.error('command', `${name}: not found`);
        status = 1;
        continue;
      }
      if (verbose === 'V') io.out(d.text + '\n');
      else io.out((d.kind === 'file' ? d.path : name) + '\n');
    }
    return status;
  }
  await io.flush();
  return runCommandOnly(c, argv.slice(i), null, null, true);
}, { stdin: true });

define('builtin', async (c, argv, io) => {
  if (argv.length < 2) return 0;
  const name = argString(argv[1]);
  const fn = builtins[name];
  if (!fn) {
    io.error('builtin', `${name}: not a shell builtin`);
    return 1;
  }
  return fn(c, argv.slice(1), io);
}, { stdin: true });

define('let', (c, argv, io) => {
  if (argv.length < 2) {
    io.error('let', 'expression expected');
    return 1;
  }
  let value = 0;
  for (const arg of argv.slice(1)) value = arith(c, argString(arg));
  return value === 0 ? 1 : 0;
});

define('getopts', (c, argv, io) => {
  if (argv.length < 3) {
    io.err('getopts: usage: getopts optstring name [arg ...]\n');
    return 2;
  }
  const spec = argString(argv[1]);
  const name = argString(argv[2]);
  const silent = spec.startsWith(':') || get(c, 'OPTERR') === '0';
  const optstring = silent ? spec.slice(1) : spec;
  const args = argv.length > 3 ? argv.slice(3).map(argString) : c.pos;
  let optind = Number(get(c, 'OPTIND')) || 1;
  const state = c.root.getopts && c.root.getopts.optind === optind ? c.root.getopts : { optind, offset: 0 };
  const finish = () => {
    setVar(c, name, undefined, '?');
    c.root.getopts = null;
    return 1;
  };
  if (optind > args.length) return finish();
  const arg = args[optind - 1];
  if (state.offset === 0) {
    if (arg === '--') {
      setVar(c, 'OPTIND', undefined, String(optind + 1));
      return finish();
    }
    if (!arg.startsWith('-') || arg === '-') return finish();
    state.offset = 1;
  }
  const ch = arg[state.offset];
  state.offset++;
  const advance = () => {
    if (state.offset >= arg.length) {
      optind++;
      state.offset = 0;
    }
    state.optind = optind;
    setVar(c, 'OPTIND', undefined, String(optind));
  };
  const at = optstring.indexOf(ch);
  if (at === -1 || ch === ':') {
    if (silent) {
      setVar(c, 'OPTARG', undefined, ch);
    } else {
      io.err(`ant:shell: illegal option -- ${ch}\n`);
      c.vars.unset('OPTARG');
    }
    setVar(c, name, undefined, '?');
    advance();
    c.root.getopts = state;
    return 0;
  }
  if (optstring[at + 1] === ':') {
    let value;
    if (state.offset < arg.length) {
      value = arg.slice(state.offset);
      optind++;
    } else {
      optind++;
      if (optind > args.length) {
        state.offset = 0;
        state.optind = optind;
        setVar(c, 'OPTIND', undefined, String(optind));
        if (silent) {
          setVar(c, name, undefined, ':');
          setVar(c, 'OPTARG', undefined, ch);
        } else {
          io.err(`ant:shell: option requires an argument -- ${ch}\n`);
          setVar(c, name, undefined, '?');
          c.vars.unset('OPTARG');
        }
        c.root.getopts = state;
        return 0;
      }
      value = args[optind - 1];
      optind++;
    }
    state.offset = 0;
    state.optind = optind;
    setVar(c, 'OPTIND', undefined, String(optind));
    setVar(c, 'OPTARG', undefined, value);
    setVar(c, name, undefined, ch);
    c.root.getopts = state;
    return 0;
  }
  setVar(c, name, undefined, ch);
  c.vars.unset('OPTARG');
  advance();
  c.root.getopts = state;
  return 0;
});

function signalName(spec) {
  const s = spec.toUpperCase().replace(/^SIG/, '');
  if (s === '0' || s === 'EXIT') return 'EXIT';
  if (/^[0-9]+$/.test(s)) return SIGNALS[Number(s)] ?? null;
  if (s === 'ERR' || s === 'DEBUG' || s === 'RETURN' || SIGNALS.includes(s)) return s;
  return null;
}

define('trap', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  if (args[0] === '--') args.shift();
  if (args.length === 0 || args[0] === '-p') {
    const filter = args.length > 1 ? new Set(args.slice(1).map(signalName)) : null;
    for (const [sig, action] of Object.entries(c.traps ?? {})) {
      if (filter && !filter.has(sig)) continue;
      io.out(`trap -- ${shellQuote(action)} ${sig}\n`);
    }
    return 0;
  }
  if (args[0] === '-l') {
    io.out(SIGNALS.slice(1).map((s, i) => `${String(i + 1).padStart(2)}) SIG${s}`).join('\t') + '\n');
    return 0;
  }
  let action = args[0];
  let sigs = args.slice(1);
  if (sigs.length === 0 || /^[0-9]+$/.test(action)) {
    sigs = args;
    action = '-';
  }
  let status = 0;
  for (const spec of sigs) {
    const sig = signalName(spec);
    if (!sig) {
      io.error('trap', `${spec}: invalid signal specification`);
      status = 1;
      continue;
    }
    if (action === '-') {
      if (c.traps) delete c.traps[sig];
      setSignalTrap(c, sig, false);
    } else {
      (c.traps ??= Object.create(null))[sig] = action;
      setSignalTrap(c, sig, true);
    }
  }
  return status;
});

define('shopt', (c, argv, io) => {
  const parsed = parseFlags(argv, 'pqsuo');
  if (parsed.error) {
    io.error('shopt', parsed.error);
    return 2;
  }
  const { on, rest } = parsed;
  const names = rest.map(argString);
  if (on.has('o')) {
    let status = 0;
    for (const name of names) {
      if (!SET_OPTIONS[name]) {
        if (SET_IGNORED.has(name)) continue;
        io.error('shopt', `${name}: invalid option name`);
        status = 1;
        continue;
      }
      if (on.has('s')) c.opts[SET_OPTIONS[name]] = true;
      else if (on.has('u')) c.opts[SET_OPTIONS[name]] = false;
      else if (!c.opts[SET_OPTIONS[name]]) status = 1;
    }
    return status;
  }
  if (!names.length) {
    for (const name of SHOPT) {
      const value = !!c.opts[name];
      if (on.has('s') && !value) continue;
      if (on.has('u') && value) continue;
      io.out(on.has('p') ? `shopt ${value ? '-s' : '-u'} ${name}\n` : `${name.padEnd(15)}\t${value ? 'on' : 'off'}\n`);
    }
    return 0;
  }
  let status = 0;
  for (const name of names) {
    if (!SHOPT.includes(name)) {
      if (SHOPT_IGNORED.has(name)) continue;
      io.error('shopt', `${name}: invalid shell option name`);
      status = 1;
      continue;
    }
    if (on.has('s')) c.opts[name] = true;
    else if (on.has('u')) c.opts[name] = false;
    else {
      const value = !!c.opts[name];
      if (!value) status = 1;
      if (!on.has('q')) io.out(on.has('p') ? `shopt ${value ? '-s' : '-u'} ${name}\n` : `${name.padEnd(15)}\t${value ? 'on' : 'off'}\n`);
    }
  }
  return status;
});

function homeAbbrev(c, path) {
  const home = isset(c, 'HOME', undefined, true) ? get(c, 'HOME') : '';
  if (home && (path === home || path.startsWith(home + '/'))) return '~' + path.slice(home.length);
  return path;
}

function printDirs(c, io, long) {
  const list = [c.cwd, ...(c.dirs ?? [])].map(p => (long ? p : homeAbbrev(c, p)));
  io.out(list.join(' ') + '\n');
}

define('dirs', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  if (args.includes('-c')) {
    c.dirs = [];
    return 0;
  }
  if (args.includes('-v') || args.includes('-p')) {
    [c.cwd, ...(c.dirs ?? [])].forEach((p, i) => io.out(args.includes('-v') ? `${String(i).padStart(2)}  ${homeAbbrev(c, p)}\n` : `${homeAbbrev(c, p)}\n`));
    return 0;
  }
  printDirs(c, io, args.includes('-l'));
  return 0;
});

define('pushd', (c, argv, io) => {
  c.dirs ??= [];
  const args = argv.slice(1).map(argString).filter(a => a !== '-n');
  const old = c.cwd;
  if (args.length === 0) {
    if (!c.dirs.length) {
      io.error('pushd', 'no other directory');
      return 1;
    }
    const target = c.dirs.shift();
    const status = changeDirectory(c, io, target, false, false);
    if (status) {
      c.dirs.unshift(target);
      return status;
    }
    c.dirs.unshift(old);
  } else {
    const status = changeDirectory(c, io, args[0], false, false);
    if (status) return status;
    c.dirs.unshift(old);
  }
  printDirs(c, io, false);
  return 0;
});

define('popd', (c, argv, io) => {
  c.dirs ??= [];
  if (!c.dirs.length) {
    io.error('popd', 'directory stack empty');
    return 1;
  }
  const target = c.dirs.shift();
  const status = changeDirectory(c, io, target, false, false);
  if (status) {
    c.dirs.unshift(target);
    return status;
  }
  printDirs(c, io, false);
  return 0;
});

define('hash', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  if (args.includes('-r')) {
    clearWhich();
    return 0;
  }
  let status = 0;
  for (const name of args) {
    if (name.startsWith('-')) continue;
    if (!which(c, name)) {
      io.error('hash', `${name}: not found`);
      status = 1;
    }
  }
  return status;
});

define('umask', (c, argv, io) => {
  const arg = argv[1] !== undefined ? argString(argv[1]) : null;
  if (arg === null || arg === '-S' || arg === '-p') {
    const mask = c.umask;
    if (arg === '-S') {
      const perms = shift => {
        const bits = ~(mask >> shift) & 7;
        return (bits & 4 ? 'r' : '') + (bits & 2 ? 'w' : '') + (bits & 1 ? 'x' : '');
      };
      io.out(`u=${perms(6)},g=${perms(3)},o=${perms(0)}\n`);
    } else io.out((arg === '-p' ? 'umask ' : '') + mask.toString(8).padStart(4, '0') + '\n');
    return 0;
  }
  if (!/^[0-7]+$/.test(arg)) {
    io.error('umask', `${arg}: octal number out of range`);
    return 1;
  }
  c.umask = parseInt(arg, 8) & 0o777;
  return 0;
});

define('alias', (c, argv, io) => {
  const aliases = (c.root.aliases ??= new Map());
  const args = argv.slice(1).map(argString).filter(a => a !== '-p');
  if (!args.length) {
    for (const [name, value] of [...aliases].sort()) io.out(`${c.opts.posix ? '' : 'alias '}${name}=${shellQuote(value)}\n`);
    return 0;
  }
  let status = 0;
  for (const arg of args) {
    const eq = arg.indexOf('=');
    if (eq > 0) {
      aliases.set(arg.slice(0, eq), arg.slice(eq + 1));
      continue;
    }
    if (aliases.has(arg)) io.out(`${c.opts.posix ? '' : 'alias '}${arg}=${shellQuote(aliases.get(arg))}\n`);
    else {
      io.error('alias', `${arg}: not found`);
      status = 1;
    }
  }
  return status;
});

define('unalias', (c, argv, io) => {
  const aliases = c.root.aliases;
  const args = argv.slice(1).map(argString);
  if (args[0] === '-a') {
    if (aliases) aliases.clear();
    return 0;
  }
  let status = 0;
  for (const name of args) {
    if (!aliases || !aliases.delete(name)) {
      io.error('unalias', `${name}: not found`);
      status = 1;
    }
  }
  return status;
});

define('times', (c, argv, io) => {
  io.out('0m0.000s 0m0.000s\n0m0.000s 0m0.000s\n');
  return 0;
});

define('caller', (c, argv, io) => {
  const depth = c.fstack.length;
  const lines = c.flines ?? [];
  if (argv.length < 2) {
    if (depth === 0) return 1;
    io.out(`${lines[depth - 1] ?? c.ln} ${depth > 1 ? c.fstack[depth - 2] : 'main'}\n`);
    return 0;
  }
  const frame = Number(argString(argv[1]));
  if (!Number.isInteger(frame) || frame < 0 || frame >= depth) return 1;
  const at = depth - 1 - frame;
  io.out(`${lines[at] ?? c.ln} ${at > 0 ? c.fstack[at - 1] : 'main'} ant\n`);
  return 0;
});

const ULIMITS = (process.platform === 'linux'
  ? [
      ['R', 10, 'real-time non-blocking time', 'microseconds', 1],
      ['c', 0, 'core file size', 'blocks', 512],
      ['d', 1, 'data seg size', 'kbytes', 1024],
      ['e', 11, 'scheduling priority', '', 1],
      ['f', 2, 'file size', 'blocks', 512],
      ['i', 12, 'pending signals', '', 1],
      ['l', 8, 'max locked memory', 'kbytes', 1024],
      ['m', 9, 'max memory size', 'kbytes', 1024],
      ['n', 3, 'open files', '', 1],
      ['p', -1, 'pipe size', '512 bytes', 1],
      ['q', 13, 'POSIX message queues', 'bytes', 1],
      ['r', 14, 'real-time priority', '', 1],
      ['s', 4, 'stack size', 'kbytes', 1024],
      ['t', 5, 'cpu time', 'seconds', 1],
      ['u', 6, 'max user processes', '', 1],
      ['v', 7, 'virtual memory', 'kbytes', 1024],
      ['x', 15, 'file locks', '', 1]
    ]
  : [
      ['c', 0, 'core file size', 'blocks', 512],
      ['d', 1, 'data seg size', 'kbytes', 1024],
      ['f', 2, 'file size', 'blocks', 512],
      ['l', 8, 'max locked memory', 'kbytes', 1024],
      ['m', 9, 'max memory size', 'kbytes', 1024],
      ['n', 3, 'open files', '', 1],
      ['p', -1, 'pipe size', '512 bytes', 1],
      ['s', 4, 'stack size', 'kbytes', 1024],
      ['t', 5, 'cpu time', 'seconds', 1],
      ['u', 6, 'max user processes', '', 1],
      ['v', 7, 'virtual memory', 'kbytes', 1024]
    ]);

function currentLimit(c, index) {
  if (index < 0) return [process.platform === 'linux' ? 8 : 1, process.platform === 'linux' ? 8 : 1];
  if (c.limits && c.limits.has(index)) return c.limits.get(index);
  return ops.rlimit(index) || null;
}

define('ulimit', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  let hard = false;
  let soft = false;
  let all = false;
  const selected = [];
  let value = null;
  for (const arg of args) {
    if (/^-[A-Za-z]+$/.test(arg)) {
      for (const flag of arg.slice(1)) {
        if (flag === 'H') hard = true;
        else if (flag === 'S') soft = true;
        else if (flag === 'a') all = true;
        else {
          const entry = ULIMITS.find(u => u[0] === flag);
          if (!entry) {
            io.error('ulimit', `-${flag}: invalid option`);
            return 2;
          }
          selected.push(entry);
        }
      }
    } else value = arg;
  }
  const show = (n, scale) => (n < 0 ? 'unlimited' : String(Math.floor(n / scale)));
  if (all) {
    for (const [flag, index, label, unit, scale] of ULIMITS) {
      const limit = currentLimit(c, index);
      if (!limit) continue;
      const paren = `(${unit ? unit + ', ' : ''}-${flag})`;
      io.out(`${label.padEnd(20)}${paren.padStart(20)} ${show(hard ? limit[1] : limit[0], scale)}\n`);
    }
    return 0;
  }
  const list = selected.length ? selected : [ULIMITS.find(u => u[0] === 'f')];
  for (const [flag, index, , , scale] of list) {
    const limit = currentLimit(c, index);
    if (!limit) {
      io.error('ulimit', `-${flag}: cannot get limit`);
      return 1;
    }
    if (value === null) {
      io.out(show(hard ? limit[1] : limit[0], scale) + '\n');
      continue;
    }
    if (index < 0) {
      io.error('ulimit', `-${flag}: cannot modify limit: Invalid argument`);
      return 1;
    }
    let n;
    if (value === 'unlimited') n = -1;
    else if (value === 'hard') n = limit[1];
    else if (value === 'soft') n = limit[0];
    else if (/^[0-9]+$/.test(value)) n = Number(value) * scale;
    else {
      io.error('ulimit', `${value}: invalid number`);
      return 1;
    }
    const setHard = hard || !soft;
    const setSoft = soft || !hard;
    const [, currentHard] = limit;
    const exceeds = (a, b) => b >= 0 && (a < 0 || a > b);
    if (setHard && exceeds(n, currentHard)) {
      io.error('ulimit', `${ULIMITS.find(u => u[0] === flag)[2]}: cannot modify limit: Operation not permitted`);
      return 1;
    }
    const nextHard = setHard ? n : currentHard;
    const nextSoft = setSoft ? n : exceeds(limit[0], nextHard) ? nextHard : limit[0];
    if (exceeds(nextSoft, nextHard)) {
      io.error('ulimit', `${ULIMITS.find(u => u[0] === flag)[2]}: cannot modify limit: Invalid argument`);
      return 1;
    }
    (c.limits ??= new Map()).set(index, [nextSoft, nextHard]);
  }
  return 0;
});

const KEYWORD_LIST = ['!', 'case', 'coproc', 'do', 'done', 'elif', 'else', 'esac', 'fi', 'for', 'function', 'if', 'in', 'select', 'then', 'time', 'until', 'while', '{', '}', '[[', ']]'];

function commandNames(c) {
  const names = new Set([...Object.keys(builtins), ...c.funcs.names(), ...KEYWORD_LIST, ...(c.root.aliases ? c.root.aliases.keys() : [])]);
  for (const dir of get(c, 'PATH').split(':')) {
    try {
      for (const name of fs.readdirSync(dir || '.')) names.add(name);
    } catch {}
  }
  return [...names];
}

define('compgen', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  const sources = [];
  let words = null;
  let prefix = '';
  let filter = null;
  let suffix = '';
  let pre = '';
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '--') {
      prefix = args[i + 1] ?? '';
      break;
    }
    if (!a.startsWith('-')) {
      prefix = a;
      break;
    }
    for (let k = 1; k < a.length; k++) {
      const flag = a[k];
      const value = () => (k + 1 < a.length ? a.slice(k + 1) : args[++i] ?? '');
      switch (flag) {
        case 'W': words = value(); k = a.length; break;
        case 'A': sources.push({ action: 'alias', alias: 'a', builtin: 'b', command: 'c', directory: 'd', export: 'e', file: 'f', function: 'F', keyword: 'k', user: 'u', variable: 'v', arrayvar: 'arr', enabled: 'b', setopt: 'o', shopt: 's' }[value()]); k = a.length; break;
        case 'X': filter = value(); k = a.length; break;
        case 'S': suffix = value(); k = a.length; break;
        case 'P': pre = value(); k = a.length; break;
        case 'G': sources.push({ glob: value() }); k = a.length; break;
        default: sources.push(flag);
      }
    }
  }
  let out = [];
  for (const source of sources) {
    const key = typeof source === 'object' ? source.action ?? null : source;
    if (typeof source === 'object' && source.glob) {
      try {
        for (const name of fs.readdirSync(c.cwd)) if (matchPattern(name, source.glob)) out.push(name);
      } catch {}
      continue;
    }
    switch (key) {
      case 'a': if (c.root.aliases) out.push(...c.root.aliases.keys()); break;
      case 'b': out.push(...Object.keys(builtins)); break;
      case 'k': out.push(...KEYWORD_LIST); break;
      case 'F': out.push(...c.funcs.names()); break;
      case 'v': out.push(...c.vars.names()); break;
      case 'arr': out.push(...c.vars.names().filter(n => c.vars.find(n)?.k)); break;
      case 'e': out.push(...c.vars.names().filter(n => c.vars.find(n)?.x)); break;
      case 'c': out.push(...commandNames(c)); break;
      case 'u': {
        try {
          out.push(os.userInfo().username);
        } catch {}
        break;
      }
      case 'o': out.push('allexport', 'errexit', 'noclobber', 'noglob', 'nounset', 'pipefail', 'posix', 'verbose', 'xtrace'); break;
      case 's': out.push(...SHOPT_NAMES); break;
      case 'f':
      case 'd': {
        const slash = prefix.lastIndexOf('/');
        const dir = slash >= 0 ? prefix.slice(0, slash + 1) : '';
        try {
          for (const name of fs.readdirSync(resolvePath(c, dir || '.'))) {
            const full = dir + name;
            if (key === 'd') {
              try {
                if (!fs.statSync(resolvePath(c, full)).isDirectory()) continue;
              } catch {
                continue;
              }
            }
            out.push(full);
          }
        } catch {}
        break;
      }
    }
  }
  if (words !== null) {
    const ifs = isset(c, 'IFS', undefined, false) ? get(c, 'IFS') : ' \t\n';
    out.push(...words.split(new RegExp(`[${ifs.replace(/[\]\\^-]/g, '\\$&')}]+`)).filter(Boolean));
  }
  out = out.filter(name => name.startsWith(prefix));
  if (filter !== null) {
    const negate = filter.startsWith('!');
    const pattern = negate ? filter.slice(1) : filter;
    out = out.filter(name => matchPattern(name, pattern) === negate);
  }
  const unique = [...new Set(out)].sort();
  for (const name of unique) io.out(`${pre}${name}${suffix}\n`);
  return unique.length ? 0 : 1;
});

define('complete compopt', () => 0);
define('bind', () => 0);

define('enable', (c, argv, io) => {
  const args = argv.slice(1).map(argString);
  const disable = args.includes('-n');
  const names = args.filter(a => !a.startsWith('-'));
  const disabled = (c.root.disabled ??= new Set());
  if (!names.length) {
    for (const name of Object.keys(builtins).sort()) {
      const off = disabled.has(name);
      if (disable ? off : !off || args.includes('-a')) io.out(`enable ${off ? '-n ' : ''}${name}\n`);
    }
    return 0;
  }
  let status = 0;
  for (const name of names) {
    if (!builtins[name]) {
      io.error('enable', `${name}: not a shell builtin`);
      status = 1;
      continue;
    }
    if (disable) disabled.add(name);
    else disabled.delete(name);
  }
  return status;
});

define('help', (c, argv, io) => {
  const topics = argv.slice(1).map(argString).filter(a => !a.startsWith('-'));
  if (!topics.length) {
    io.out('ant:shell, a bash-compatible shell for Ant.\nThese shell commands are defined internally:\n\n');
    io.out(Object.keys(builtins).sort().join(' ') + '\n');
    return 0;
  }
  let status = 0;
  for (const topic of topics) {
    if (builtins[topic] || KEYWORD_LIST.includes(topic)) io.out(`${topic}: shell ${builtins[topic] ? 'builtin' : 'keyword'}\n`);
    else {
      io.error('help', `no help topics match \`${topic}'.`);
      status = 1;
    }
  }
  return status;
});

define('history', () => 0);

define('fc', (c, argv, io) => {
  io.error('fc', 'history specification out of range');
  return 1;
});

define('logout', (c, argv, io) => {
  io.error('logout', "not login shell: use `exit'");
  return 1;
});

for (const name of [':', '.', 'break', 'continue', 'eval', 'exec', 'exit', 'export', 'readonly', 'return', 'set', 'times', 'trap', 'unset']) {
  if (builtins[name]) builtins[name].special = true;
}
