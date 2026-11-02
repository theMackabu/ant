// @ant: compact_on_build
const RESERVED = new Set([
  'if', 'then', 'else', 'elif', 'fi', 'do', 'done', 'case', 'esac', 'while', 'until',
  'for', 'in', 'function', 'select', 'time', 'coproc', '{', '}', '!', '[[', ']]'
]);

const DECLARATION_BUILTINS = new Set(['declare', 'typeset', 'local', 'export', 'readonly']);

const OPERATORS = [
  ';;&', '&>>', '<<<', '<<-',
  ';;', ';&', '&&', '||', '|&', '&>', '<<', '<>', '<&', '<(', '>>', '>&', '>|', '>(',
  '&', '|', ';', '(', ')', '<', '>', '\n'
];

const REDIRECT_OPS = new Set(['<', '>', '>>', '>|', '<>', '<&', '>&', '&>', '&>>', '<<', '<<-', '<<<']);
const COND_UNARY = new Set([
  '-a', '-b', '-c', '-d', '-e', '-f', '-g', '-h', '-k', '-p', '-r', '-s', '-t', '-u',
  '-w', '-x', '-G', '-L', '-N', '-O', '-S', '-z', '-n', '-o', '-v', '-R'
]);
const COND_BINARY = new Set([
  '==', '=', '!=', '=~', '<', '>', '-eq', '-ne', '-lt', '-le', '-gt', '-ge', '-nt', '-ot', '-ef'
]);

const NAME_START = /[A-Za-z_]/;
const NAME_RE = /[A-Za-z_][A-Za-z0-9_]*/y;
const FUNC_PARENS_RE = /[ \t]*\([ \t]*\)/y;
const IO_NUMBER_RE = /([0-9]+)(?=[<>])/y;
const FD_VAR_RE = /\{([A-Za-z_][A-Za-z0-9_]*(?:\[[^\]\n]*\])?)\}(?=[<>])/y;

export function isName(text) {
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(text);
}

function isMeta(c) {
  return c === '' || c === ' ' || c === '\t' || c === '\r' || c === '\n' || c === '|' ||
    c === '&' || c === ';' || c === '(' || c === ')' || c === '<' || c === '>';
}

function isBlank(c) {
  return c === ' ' || c === '\t' || c === '\r';
}

function isSpecialParam(c) {
  return c === '@' || c === '*' || c === '#' || c === '?' || c === '-' || c === '$' || c === '!' || c === '0';
}

function lit(v, q) {
  return { t: 'lit', v, q };
}

function rawByte(b) {
  b &= 255;
  return String.fromCharCode(b < 0x80 ? b : 0xdc00 + b);
}

function decodeAnsiC(text) {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c !== '\\' || i + 1 >= text.length) {
      out += c;
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
      case '?': out += '?'; break;
      case 'c': {
        const k = text[i + 1];
        if (k !== undefined) {
          i++;
          out += String.fromCharCode(k.toUpperCase().charCodeAt(0) ^ 64);
        } else out += '\\c';
        break;
      }
      case 'x': {
        const m = /^[0-9a-fA-F]{1,2}/.exec(text.slice(i + 1));
        if (!m) {
          out += '\\x';
          break;
        }
        out += rawByte(parseInt(m[0], 16));
        i += m[0].length;
        break;
      }
      case 'u': case 'U': {
        const m = (n === 'u' ? /^[0-9a-fA-F]{1,4}/ : /^[0-9a-fA-F]{1,8}/).exec(text.slice(i + 1));
        if (!m) {
          out += '\\' + n;
          break;
        }
        const cp = parseInt(m[0], 16);
        out += cp <= 0x10ffff ? String.fromCodePoint(cp) : '';
        i += m[0].length;
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

export class Parser {
  constructor(src, markers, context) {
    this.src = src;
    this.pos = 0;
    this.markers = markers;
    this.context = context;
    this.markerAt = new Map();
    let k = 0;
    for (let i = src.indexOf('\0'); i !== -1; i = src.indexOf('\0', i + 1)) this.markerAt.set(i, markers[k++]);
    this.heredocs = [];
    this.lineStarts = null;
    this.extglob = !!context.extglob;
    this.extglobLine = null;
    this.condDepth = 0;
  }

  fail(message, at = this.pos) {
    const ctx = this.context;
    if (ctx.template) {
      const markerPositions = [...this.markerAt.keys()].filter(p => p < at);
      const segment = markerPositions.length;
      const offset = at - (segment ? markerPositions[segment - 1] + 1 : 0);
      throw new SyntaxError(`ant:shell: ${message} at template segment ${segment} offset ${offset}`);
    }
    throw new SyntaxError(`ant:shell: ${ctx.name ? ctx.name + ': ' : ''}line ${this.lineAt(at) + ctx.lineBase}: ${message}`);
  }

  lineAt(at) {
    if (!this.lineStarts) {
      this.lineStarts = [0];
      for (let i = this.src.indexOf('\n'); i !== -1; i = this.src.indexOf('\n', i + 1)) this.lineStarts.push(i + 1);
    }
    let lo = 0;
    let hi = this.lineStarts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (this.lineStarts[mid] <= at) lo = mid;
      else hi = mid - 1;
    }
    return lo + 1;
  }

  ch(at = this.pos) {
    return at < this.src.length ? this.src[at] : '';
  }

  sub(text, markers, context = this.context) {
    const parser = new Parser(text, markers, context);
    return parser;
  }

  markersIn(start, end) {
    const list = [];
    for (let i = this.src.indexOf('\0', start); i !== -1 && i < end; i = this.src.indexOf('\0', i + 1)) list.push(this.markerAt.get(i));
    return list;
  }

  skipSpace() {
    for (;;) {
      const c = this.ch();
      if (isBlank(c)) this.pos++;
      else if (c === '\\' && this.ch(this.pos + 1) === '\n') this.pos += 2;
      else if (c === '#') {
        while (this.pos < this.src.length && this.src[this.pos] !== '\n') this.pos++;
      } else break;
    }
  }

  peekOp() {
    this.skipSpace();
    const src = this.src;
    const c = this.ch();
    if (!isMeta(c) || c === '' || c === ' ' || c === '\t' || c === '\r') return null;
    for (const op of OPERATORS) if (src.startsWith(op, this.pos)) return op;
    return null;
  }

  takeOp(op) {
    if (this.peekOp() !== op) return false;
    this.pos += op.length;
    if (op === '\n') this.readHeredocs();
    return true;
  }

  expectOp(op, what) {
    if (!this.takeOp(op)) this.fail(`expected '${op === '\n' ? 'newline' : op}'${what ? ' ' + what : ''}`);
  }

  skipNewlines() {
    while (this.takeOp('\n'));
  }

  peekReserved() {
    this.skipSpace();
    const src = this.src;
    let end = this.pos;
    let word = '';
    for (;;) {
      if (src.startsWith('\\\n', end)) {
        end += 2;
        continue;
      }
      if (end >= src.length || isMeta(src[end]) || word.length > 8) break;
      word += src[end++];
    }
    this.reservedEnd = end;
    return RESERVED.has(word) ? word : null;
  }

  takeReserved(word) {
    if (this.peekReserved() !== word) return false;
    this.pos = this.reservedEnd;
    return true;
  }

  expectReserved(word, what) {
    if (!this.takeReserved(word)) this.fail(`expected '${word}'${what ? ' ' + what : ''}`);
  }

  atEnd() {
    this.skipSpace();
    return this.pos >= this.src.length;
  }

  parseProgram() {
    const list = this.parseList(() => this.atEnd());
    if (!this.atEnd()) this.fail(`unexpected token '${this.describeNext()}'`);
    if (this.heredocs.length) this.readHeredocs();
    return list;
  }

  describeNext() {
    const op = this.peekOp();
    if (op) return op === '\n' ? 'newline' : op;
    const reserved = this.peekReserved();
    if (reserved) return reserved;
    return this.src.slice(this.pos, this.pos + 10);
  }

  parseList(isEnd) {
    const items = [];
    this.skipNewlines();
    while (!isEnd()) {
      const op = this.peekOp();
      if (op === ';' || op === '&' || op === ';;' || op === ';&' || op === ';;&' || op === ')' || op === '&&' || op === '||' || op === '|' || op === '|&') {
        if (op === ';' && this.src.startsWith(';;', this.pos)) this.fail("unexpected separator ';;'");
        this.fail(op === ';' || op === '&' ? 'expected a command' : `unexpected token '${op}'`);
      }
      const reserved = this.peekReserved();
      if (reserved && reserved !== '{' && reserved !== '!' && reserved !== '[[' && !this.isCommandReserved(reserved)) {
        this.fail(`unexpected token '${reserved}'`);
      }
      const line = this.lineAt(this.pos);
      const itemStart = this.pos;
      const node = this.parseAndOr();
      const itemText = this.src.slice(itemStart, this.pos).trim().replace(/\0/g, '${...}');
      let async = false;
      const sep = this.peekOp();
      if (sep === '&') {
        this.pos++;
        async = true;
      } else if (sep === ';') {
        this.pos++;
        if (this.peekOp() === ';') this.fail("unexpected separator ';;'");
      } else if (sep === '\n') {
        this.takeOp('\n');
      } else if (!isEnd()) {
        this.fail(`unexpected token '${this.describeNext()}'`);
      }
      items.push({ node, async, line, text: itemText });
      this.noteShopt(node, line);
      this.skipNewlines();
    }
    return { t: 'list', items };
  }

  noteShopt(node, line) {
    if (node.t !== 'simple' || node.words.length < 3) return;
    const words = node.words.map(word => (word.length === 1 && word[0].t === 'lit' ? word[0].v : null));
    if (words[0] !== 'shopt' || !words.includes('extglob')) return;
    if (words.includes('-s')) this.extglobLine = this.extglobLine === null ? line : Math.min(this.extglobLine, line);
    else if (words.includes('-u') && !this.extglob) this.extglobLine = null;
  }

  isCommandReserved(word) {
    return word === 'if' || word === 'while' || word === 'until' || word === 'for' || word === 'case' ||
      word === 'function' || word === 'time' || word === 'select' || word === 'coproc';
  }

  parseAndOr() {
    const first = this.parsePipeline();
    const rest = [];
    for (;;) {
      const op = this.peekOp();
      if (op !== '&&' && op !== '||') break;
      this.pos += 2;
      this.skipNewlines();
      if (this.atEnd() || [';', '&', '&&', '||', ')', '|', ';;'].includes(this.peekOp())) {
        this.fail('conditional operator requires a following command');
      }
      rest.push({ op, pipe: this.parsePipeline() });
    }
    return rest.length ? { t: 'andor', first, rest } : first;
  }

  parsePipeline() {
    const line = this.lineAt(this.pos);
    let time = false;
    let bang = false;
    if (this.takeReserved('time')) {
      time = true;
      this.skipSpace();
      if (this.src.startsWith('-p', this.pos) && isMeta(this.ch(this.pos + 2))) this.pos += 2;
    }
    while (this.takeReserved('!')) bang = !bang;
    const cmds = [this.parseCommand()];
    const errs = [false];
    for (;;) {
      const op = this.peekOp();
      if (op !== '|' && op !== '|&') break;
      this.pos += op.length;
      errs[cmds.length - 1] = op === '|&';
      this.skipNewlines();
      const next = this.peekOp();
      if (this.atEnd() || (next && next !== '(' && next !== '<' && next !== '>' && next !== '<<' && next !== '<<<' && next !== '>>' && next !== '<(' && next !== '>(' && next !== '&>' && next !== '&>>' && next !== '<&' && next !== '>&' && next !== '>|' && next !== '<>' && next !== '<<-')) {
        this.fail("pipeline requires a command after '|'");
      }
      cmds.push(this.parseCommand());
      errs.push(false);
    }
    if (cmds.length === 1 && !bang && !time) return cmds[0];
    return { t: 'pipeline', cmds, errs, bang, time, line };
  }

  parseCommand() {
    const start = this.pos;
    const reserved = this.peekReserved();
    let node;
    if (reserved) {
      switch (reserved) {
        case 'if': node = this.parseIf(); break;
        case 'while': case 'until': node = this.parseWhile(reserved); break;
        case 'for': node = this.parseFor(); break;
        case 'select': node = this.parseFor('select'); break;
        case 'case': node = this.parseCase(); break;
        case '{': node = this.parseGroup(); break;
        case '[[': node = this.parseCond(); break;
        case 'function': node = this.parseFunctionKeyword(); return node;
        case 'coproc': return this.parseCoproc();
        default: this.fail(`unexpected token '${reserved}'`);
      }
    } else if (this.peekOp() === '(') {
      if (this.ch(this.pos + 1) === '(' && this.looksLikeArith(this.pos + 2)) node = this.parseArithCommand();
      else node = this.parseSubshell();
    } else {
      return this.parseSimple();
    }
    node.redirs = this.parseRedirections();
    node.line = this.lineAt(start);
    return node;
  }

  parseRedirections() {
    const redirs = [];
    for (;;) {
      const redir = this.tryRedirection();
      if (!redir) break;
      redirs.push(redir);
    }
    return redirs;
  }

  tryRedirection() {
    this.skipSpace();
    let fd = null;
    let fdVar = null;
    IO_NUMBER_RE.lastIndex = this.pos;
    const ioMatch = IO_NUMBER_RE.exec(this.src);
    const save = this.pos;
    if (ioMatch) {
      fd = Number(ioMatch[1]);
      this.pos += ioMatch[1].length;
    } else {
      FD_VAR_RE.lastIndex = this.pos;
      const varMatch = FD_VAR_RE.exec(this.src);
      if (varMatch) {
        fdVar = varMatch[1];
        this.pos += varMatch[0].length;
      }
    }
    const op = this.peekOp();
    if (!op || !REDIRECT_OPS.has(op)) {
      this.pos = save;
      return null;
    }
    if (fd !== null && fd > 255) this.fail(`file descriptor ${fd} is out of range`);
    if ((fd !== null || fdVar !== null) && (op === '&>' || op === '&>>')) {
      this.pos = save;
      return null;
    }
    this.pos += op.length;
    this.skipSpace();
    if (op === '<<' || op === '<<-') {
      const start = this.pos;
      const word = this.readWord();
      if (!word) this.fail('here-document requires a delimiter');
      const raw = this.src.slice(start, this.pos);
      if (raw.includes('\0')) this.fail('here-document delimiter cannot be an interpolation');
      let delim = '';
      let quoted = false;
      for (const part of word) {
        if (part.q) quoted = true;
        if (part.t !== 'lit') this.fail('here-document delimiter must be literal');
        delim += part.v;
      }
      if (/["'\\]/.test(raw)) quoted = true;
      const redir = { op, fd, fdVar, target: null, heredoc: { delim, quoted, strip: op === '<<-', body: null, raw: '' } };
      this.heredocs.push(redir);
      return redir;
    }
    const word = this.readWord();
    if (!word) this.fail('redirection requires a target word');
    return { op, fd, fdVar, target: word };
  }

  readHeredocs() {
    const pending = this.heredocs;
    this.heredocs = [];
    for (const redir of pending) {
      const doc = redir.heredoc;
      const pieces = [];
      const markers = [];
      for (;;) {
        if (this.pos >= this.src.length) break;
        let nl = this.src.indexOf('\n', this.pos);
        if (nl === -1) nl = this.src.length;
        let lineStart = this.pos;
        if (doc.strip) while (this.src[lineStart] === '\t') lineStart++;
        const line = this.src.slice(lineStart, nl);
        this.pos = nl < this.src.length ? nl + 1 : nl;
        if (line === doc.delim) break;
        pieces.push(line, '\n');
        markers.push(...this.markersIn(lineStart, nl));
      }
      const text = pieces.join('');
      doc.raw = text.replace(/\0/g, '${...}');
      if (doc.quoted) {
        doc.body = this.literalParts(text, markers, true);
      } else {
        const sub = this.sub(text, markers);
        doc.body = sub.readDoubleQuoted(true);
      }
    }
  }

  literalParts(text, markers, quoted) {
    const parts = [];
    let k = 0;
    let start = 0;
    for (let i = text.indexOf('\0'); i !== -1; i = text.indexOf('\0', i + 1)) {
      if (i > start) parts.push(lit(text.slice(start, i), quoted));
      parts.push({ t: 'interp', i: markers[k++], q: quoted });
      start = i + 1;
    }
    if (start < text.length || parts.length === 0) parts.push(lit(text.slice(start), quoted));
    return parts;
  }

  looksLikeArith(at) {
    const src = this.src;
    let depth = 0;
    for (let i = at; i < src.length; i++) {
      const c = src[i];
      if (c === '\\') {
        i++;
        continue;
      }
      if (c === "'" || c === '"') {
        const end = src.indexOf(c, i + 1);
        if (end === -1) return false;
        i = end;
        continue;
      }
      if (c === '(') depth++;
      else if (c === ')') {
        if (depth === 0) return src[i + 1] === ')';
        depth--;
      }
    }
    return false;
  }

  readArithParts() {
    const parts = [];
    let text = '';
    let depth = 0;
    const flush = () => {
      if (text) parts.push(lit(text, false));
      text = '';
    };
    for (;;) {
      const c = this.ch();
      if (c === '') this.fail('unterminated arithmetic expression');
      if (c === ')' && depth === 0) {
        if (this.ch(this.pos + 1) !== ')') this.fail("expected '))'");
        this.pos += 2;
        break;
      }
      if (c === '(') depth++;
      else if (c === ')') depth--;
      if (c === '$') {
        flush();
        const part = this.readDollar(false);
        if (part) parts.push(part);
        else {
          text += '$';
          this.pos++;
        }
        continue;
      }
      if (c === '`') {
        flush();
        parts.push(this.readBacktick(false));
        continue;
      }
      if (c === '\0') {
        flush();
        parts.push({ t: 'interp', i: this.markerAt.get(this.pos), q: false });
        this.pos++;
        continue;
      }
      if (c === '"') {
        flush();
        this.pos++;
        parts.push(...this.readDoubleQuoted(false));
        continue;
      }
      if (c === '\\' && this.ch(this.pos + 1) === '\n') {
        this.pos += 2;
        continue;
      }
      text += c;
      this.pos++;
    }
    flush();
    return parts;
  }

  parseArithCommand() {
    this.pos += 2;
    const start = this.pos;
    const expr = this.readArithParts();
    return { t: 'arithcmd', expr, raw: this.src.slice(start, this.pos - 2).trim().replace(/\0/g, '${...}') };
  }

  parseSubshell() {
    this.pos++;
    const body = this.parseList(() => this.peekOp() === ')' || this.atEnd());
    this.expectOp(')', 'to close subshell');
    return { t: 'subshell', body };
  }

  parseGroup() {
    this.expectReserved('{');
    const body = this.parseList(() => this.peekReserved() === '}' || this.atEnd());
    this.expectReserved('}', 'to close group');
    return { t: 'group', body };
  }

  parseIf() {
    this.expectReserved('if');
    const clauses = [];
    let elseBody = null;
    const cond = this.parseList(() => this.peekReserved() === 'then' || this.atEnd());
    this.expectReserved('then');
    const body = this.parseList(() => ['elif', 'else', 'fi'].includes(this.peekReserved()) || this.atEnd());
    clauses.push({ cond, body });
    for (;;) {
      if (this.takeReserved('elif')) {
        const c = this.parseList(() => this.peekReserved() === 'then' || this.atEnd());
        this.expectReserved('then');
        const b = this.parseList(() => ['elif', 'else', 'fi'].includes(this.peekReserved()) || this.atEnd());
        clauses.push({ cond: c, body: b });
        continue;
      }
      if (this.takeReserved('else')) {
        elseBody = this.parseList(() => this.peekReserved() === 'fi' || this.atEnd());
      }
      this.expectReserved('fi', "to close 'if'");
      break;
    }
    return { t: 'if', clauses, else: elseBody };
  }

  parseDoGroup(what) {
    this.skipNewlines();
    if (this.takeReserved('{')) {
      const body = this.parseList(() => this.peekReserved() === '}' || this.atEnd());
      this.expectReserved('}', `to close '${what}'`);
      return body;
    }
    this.expectReserved('do', `in '${what}'`);
    const body = this.parseList(() => this.peekReserved() === 'done' || this.atEnd());
    this.expectReserved('done', `to close '${what}'`);
    return body;
  }

  parseWhile(kind) {
    this.expectReserved(kind);
    const cond = this.parseList(() => this.peekReserved() === 'do' || this.atEnd());
    const body = this.parseDoGroup(kind);
    return { t: 'while', until: kind === 'until', cond, body };
  }

  parseCoproc() {
    const start = this.pos;
    this.expectReserved('coproc');
    this.skipSpace();
    let name = 'COPROC';
    NAME_RE.lastIndex = this.pos;
    const m = NAME_RE.exec(this.src);
    if (m) {
      const save = this.pos;
      this.pos += m[0].length;
      const reserved = this.peekReserved();
      const op = this.peekOp();
      if ((reserved && reserved !== '!' && reserved !== 'in') || op === '(') name = m[0];
      else this.pos = save;
    }
    const body = this.parseCommand();
    return { t: 'coproc', name, body, text: this.src.slice(start, this.pos).trim().replace(/\0/g, '${...}'), redirs: [], line: this.lineAt(start) };
  }

  parseFor(keyword = 'for') {
    this.expectReserved(keyword);
    this.skipSpace();
    if (keyword === 'for' && this.src.startsWith('((', this.pos)) {
      this.pos += 2;
      const forStart = this.pos;
      const exprs = this.readArithParts();
      const forRaw = this.src.slice(forStart, this.pos - 2).replace(/\0/g, '${...}');
      const sections = [[], [], []];
      let section = 0;
      for (const part of exprs) {
        if (part.t !== 'lit') {
          sections[section].push(part);
          continue;
        }
        const pieces = part.v.split(';');
        pieces.forEach((piece, index) => {
          if (index > 0) section++;
          if (section > 2) this.fail("too many ';' in arithmetic for");
          if (piece) sections[section].push(lit(piece, false));
        });
      }
      if (section !== 2) this.fail("expected two ';' in arithmetic for");
      this.skipSpace();
      this.takeOp(';');
      const body = this.parseDoGroup('for');
      return { t: 'cfor', init: sections[0], cond: sections[1], step: sections[2], body, raw: forRaw };
    }
    NAME_RE.lastIndex = this.pos;
    const m = NAME_RE.exec(this.src);
    if (!m || !isMeta(this.ch(this.pos + m[0].length))) this.fail("expected a variable name after 'for'");
    const name = m[0];
    this.pos += name.length;
    let words = null;
    this.skipNewlines();
    if (this.takeReserved('in')) {
      words = [];
      for (;;) {
        const op = this.peekOp();
        if (op === ';' || op === '\n') {
          this.takeOp(op);
          break;
        }
        if (op) this.fail(`unexpected token '${op}' in 'for' word list`);
        if (this.atEnd()) this.fail("expected 'do'");
        words.push(this.readWord());
      }
    } else {
      this.takeOp(';');
    }
    const body = this.parseDoGroup(keyword);
    return { t: keyword, name, words, body };
  }

  parseCase() {
    this.expectReserved('case');
    this.skipSpace();
    const word = this.readWord();
    if (!word) this.fail("expected a word after 'case'");
    this.skipNewlines();
    this.expectReserved('in', "in 'case'");
    const items = [];
    this.skipNewlines();
    while (!this.takeReserved('esac')) {
      if (this.atEnd()) this.fail("expected 'esac'");
      this.takeOp('(');
      const patterns = [];
      for (;;) {
        this.skipSpace();
        const pattern = this.readWord();
        if (!pattern) this.fail('expected a case pattern');
        patterns.push(pattern);
        if (this.takeOp('|')) continue;
        this.expectOp(')', 'after case pattern');
        break;
      }
      const body = this.parseList(() => {
        const op = this.peekOp();
        return op === ';;' || op === ';&' || op === ';;&' || this.peekReserved() === 'esac' || this.atEnd();
      });
      let term = ';;';
      const op = this.peekOp();
      if (op === ';;' || op === ';&' || op === ';;&') {
        this.pos += op.length;
        term = op;
      }
      items.push({ patterns, body, term });
      this.skipNewlines();
    }
    return { t: 'case', word, items };
  }

  parseFunctionKeyword() {
    const start = this.pos;
    this.expectReserved('function');
    this.skipSpace();
    const nameStart = this.pos;
    while (!isMeta(this.ch())) this.pos++;
    const name = this.src.slice(nameStart, this.pos);
    if (!name || name.includes('\0')) this.fail('expected a function name');
    FUNC_PARENS_RE.lastIndex = this.pos;
    if (FUNC_PARENS_RE.exec(this.src)) this.pos = FUNC_PARENS_RE.lastIndex;
    return this.finishFunction(name, start);
  }

  finishFunction(name, start) {
    this.skipNewlines();
    const body = this.parseCommand();
    if (body.t === 'simple') this.fail('function body must be a compound command');
    return { t: 'func', name, body, text: this.src.slice(start, this.pos).replace(/\0/g, '${...}'), redirs: [], line: this.lineAt(start) };
  }

  extglobAt(at) {
    if (this.condDepth > 0 || this.extglob) return true;
    return this.extglobLine !== null && this.lineAt(at) > this.extglobLine;
  }

  parseCond() {
    const start = this.pos;
    this.expectReserved('[[');
    const inner = this.pos;
    this.condDepth++;
    let expr;
    try {
      expr = this.condOr();
    } finally {
      this.condDepth--;
    }
    this.skipCondSpace();
    if (!this.src.startsWith(']]', this.pos) || !isMeta(this.ch(this.pos + 2))) this.fail("expected ']]'");
    const raw = this.src.slice(inner, this.pos).trim().replace(/\0/g, '${...}');
    this.pos += 2;
    return { t: 'cond', expr, raw, line: this.lineAt(start) };
  }

  skipCondSpace() {
    for (;;) {
      this.skipSpace();
      if (this.ch() === '\n') this.pos++;
      else break;
    }
  }

  condAtEnd() {
    this.skipCondSpace();
    return this.src.startsWith(']]', this.pos) && isMeta(this.ch(this.pos + 2));
  }

  condOr() {
    let left = this.condAnd();
    for (;;) {
      this.skipCondSpace();
      if (!this.src.startsWith('||', this.pos)) return left;
      this.pos += 2;
      left = { t: 'or', l: left, r: this.condAnd() };
    }
  }

  condAnd() {
    let left = this.condNot();
    for (;;) {
      this.skipCondSpace();
      if (!this.src.startsWith('&&', this.pos)) return left;
      this.pos += 2;
      left = { t: 'and', l: left, r: this.condNot() };
    }
  }

  condNot() {
    this.skipCondSpace();
    if (this.ch() === '!' && isMeta(this.ch(this.pos + 1))) {
      this.pos++;
      return { t: 'not', e: this.condNot() };
    }
    return this.condPrimary();
  }

  condWordText(word) {
    if (word.length !== 1 || word[0].t !== 'lit' || word[0].q) return null;
    return word[0].v;
  }

  condPrimary() {
    this.skipCondSpace();
    if (this.condAtEnd()) this.fail('unexpected end of conditional expression');
    if (this.ch() === '(') {
      this.pos++;
      const e = this.condOr();
      this.skipCondSpace();
      if (this.ch() !== ')') this.fail("expected ')' in conditional expression");
      this.pos++;
      return e;
    }
    const first = this.readWord();
    if (!first) this.fail(`unexpected token '${this.describeNext()}' in conditional expression`);
    const firstText = this.condWordText(first);
    this.skipCondSpace();
    if (firstText && COND_UNARY.has(firstText) && !this.condAtEnd() && !this.src.startsWith('&&', this.pos) && !this.src.startsWith('||', this.pos) && this.ch() !== ')') {
      const save = this.pos;
      const operand = this.readWord();
      this.skipCondSpace();
      const nextOp = this.peekCondBinary();
      if (operand && !nextOp) return { t: 'unary', op: firstText, w: operand };
      this.pos = save;
    }
    const op = this.peekCondBinary();
    if (!op) return { t: 'unary', op: '-n', w: first };
    this.pos += op.length;
    this.skipCondSpace();
    if (op === '=~') return { t: 'binary', op, l: first, r: this.readRegexWord() };
    const right = this.readWord();
    if (!right) this.fail(`expected an operand after '${op}'`);
    return { t: 'binary', op, l: first, r: right };
  }

  peekCondBinary() {
    const c = this.ch();
    if (c === '<' || c === '>') return c;
    let end = this.pos;
    while (end < this.src.length && !isMeta(this.src[end])) end++;
    const word = this.src.slice(this.pos, end);
    return COND_BINARY.has(word) ? word : null;
  }

  readRegexWord() {
    const parts = [];
    let text = '';
    let depth = 0;
    const flush = () => {
      if (text) parts.push(lit(text, false));
      text = '';
    };
    for (;;) {
      const c = this.ch();
      if (c === '' || ((c === ' ' || c === '\t' || c === '\n') && depth === 0)) break;
      if (depth === 0 && (this.src.startsWith('&&', this.pos) || this.src.startsWith('||', this.pos))) break;
      if (c === ')' && depth === 0) break;
      if (c === '(') depth++;
      if (c === ')') depth--;
      if (c === '\\') {
        flush();
        parts.push(lit(this.ch(this.pos + 1), true));
        this.pos += 2;
        continue;
      }
      if (c === "'" || c === '"' || c === '$' || c === '`' || c === '\0') {
        flush();
        const sub = this.readWord(ch => ch === ' ' || ch === '\t' || ch === '\n' || ch === '' || ch === '(' || ch === ')' || ch === '|' || ch === '\\' || ch === '<' || ch === '>' || ch === '&' || ch === ';', true);
        if (sub) parts.push(...sub);
        continue;
      }
      text += c;
      this.pos++;
    }
    flush();
    if (!parts.length) this.fail("expected a regular expression after '=~'");
    return parts;
  }

  tryAssignment(allowArray) {
    let at = this.pos;
    let name = '';
    for (;;) {
      if (this.src.startsWith('\\\n', at)) {
        at += 2;
        continue;
      }
      const ch = this.src[at];
      if (ch !== undefined && (name ? /[A-Za-z0-9_]/ : /[A-Za-z_]/).test(ch)) {
        name += ch;
        at++;
      } else break;
    }
    if (!name) return null;
    const m = [name, name];
    let index = null;
    if (this.src[at] === '[') {
      let depth = 0;
      let end = at;
      for (; end < this.src.length; end++) {
        const c = this.src[end];
        if (c === '[') depth++;
        else if (c === ']') {
          depth--;
          if (depth === 0) break;
        } else if (c === '\n') return null;
      }
      if (end >= this.src.length) return null;
      index = [at + 1, end];
      at = end + 1;
    }
    let append = false;
    if (this.src[at] === '+' && this.src[at + 1] === '=') {
      append = true;
      at += 2;
    } else if (this.src[at] === '=') at += 1;
    else return null;

    const start = this.pos;
    let indexWord = null;
    if (index) {
      const save = this.pos;
      this.pos = index[0];
      indexWord = this.readWord(() => this.pos >= index[1], true) || [lit('', false)];
      this.pos = save;
    }
    this.pos = at;
    if (this.ch() === '(' && allowArray) {
      if (index) this.fail('cannot assign a list to an array member');
      this.pos++;
      const elements = [];
      for (;;) {
        for (;;) {
          this.skipSpace();
          if (this.ch() === '\n') this.pos++;
          else break;
        }
        if (this.ch() === ')') {
          this.pos++;
          break;
        }
        if (this.ch() === '') this.fail('unterminated array assignment');
        if (this.ch() === '[') {
          let depth = 0;
          let end = this.pos;
          for (; end < this.src.length; end++) {
            if (this.src[end] === '[') depth++;
            else if (this.src[end] === ']' && --depth === 0) break;
          }
          if (this.src[end + 1] === '=' || (this.src[end + 1] === '+' && this.src[end + 2] === '=')) {
            const keyStart = this.pos + 1;
            this.pos = keyStart;
            const key = this.readWord(() => this.pos >= end, true) || [lit('', false)];
            this.pos = end + 1;
            const keyAppend = this.src[this.pos] === '+';
            this.pos += keyAppend ? 2 : 1;
            const value = this.readWord() || [lit('', false)];
            elements.push({ key, value, append: keyAppend });
            continue;
          }
        }
        const word = this.readWord();
        if (!word) this.fail(`unexpected token '${this.describeNext()}' in array assignment`);
        elements.push({ key: null, value: word });
      }
      return { name: m[1], index: indexWord, append, value: null, array: elements, raw: this.src.slice(start, this.pos).replace(/\0/g, '${...}') };
    }
    const value = this.readWord(null, false, true) || [];
    return { name: m[1], index: indexWord, append, value, array: null, raw: this.src.slice(start, this.pos).replace(/\0/g, '${...}') };
  }

  parseSimple() {
    const line = this.lineAt(this.pos);
    const assigns = [];
    const words = [];
    const redirs = [];
    let declaration = false;
    for (;;) {
      this.skipSpace();
      const redir = this.tryRedirection();
      if (redir) {
        redirs.push(redir);
        continue;
      }
      const op = this.peekOp();
      if (op && op !== '<(' && op !== '>(') break;
      if (this.atEnd()) break;
      if (words.length === 0) {
        const assign = this.tryAssignment(true);
        if (assign) {
          assigns.push(assign);
          continue;
        }
      } else if (declaration) {
        const save = this.pos;
        const assign = this.tryAssignment(true);
        if (assign) {
          if (!isMeta(this.ch())) this.pos = save;
          else {
            words.push({ assign });
            continue;
          }
        }
      }
      const start = this.pos;
      const word = this.readWord();
      if (!word) this.fail(`unexpected character '${this.ch()}'`);
      if (words.length === 0 && word.length === 1 && word[0].t === 'lit' && !word[0].q) {
        const text = word[0].v;
        if (isName(text) || /^[^\s'"$`\\]+$/.test(text)) {
          FUNC_PARENS_RE.lastIndex = this.pos;
          if (FUNC_PARENS_RE.exec(this.src) && assigns.length === 0 && redirs.length === 0) {
            this.pos = FUNC_PARENS_RE.lastIndex;
            return this.finishFunction(text, start);
          }
        }
        if (DECLARATION_BUILTINS.has(text)) declaration = true;
      }
      words.push(word);
    }
    if (assigns.length === 0 && words.length === 0 && redirs.length === 0) {
      this.fail(this.atEnd() ? 'expected a command' : `unexpected token '${this.describeNext()}'`);
    }
    return { t: 'simple', assigns, words, redirs, line };
  }

  readWord(stop = null, inner = false, assignment = false) {
    const parts = [];
    let text = '';
    const start = this.pos;
    const flush = () => {
      if (text) parts.push(lit(text, false));
      text = '';
    };
    for (;;) {
      const c = this.ch();
      if (c === '') break;
      if (stop ? stop(c) : isMeta(c)) {
        if (!stop && (c === '<' || c === '>') && this.ch(this.pos + 1) === '(' && this.pos === start && !inner) {
          parts.push(this.readProcSub());
          continue;
        }
        break;
      }
      if (c === '\\') {
        const n = this.ch(this.pos + 1);
        if (n === '\n') {
          this.pos += 2;
          continue;
        }
        if (n === '') this.fail('trailing backslash');
        if (n === '\0') this.fail('backslash cannot escape an interpolation boundary');
        flush();
        parts.push(lit(n, true));
        this.pos += 2;
        continue;
      }
      if (c === "'") {
        flush();
        const end = this.src.indexOf("'", this.pos + 1);
        if (end === -1) this.fail('unterminated single quote');
        const body = this.src.slice(this.pos + 1, end);
        parts.push(...this.literalParts(body, this.markersIn(this.pos + 1, end), true));
        this.pos = end + 1;
        continue;
      }
      if (c === '"') {
        flush();
        this.pos++;
        const inner = this.readDoubleQuoted(false);
        parts.push(...inner);
        continue;
      }
      if (c === '$') {
        const n = this.ch(this.pos + 1);
        if (n === "'") {
          flush();
          parts.push(lit(decodeAnsiC(this.readAnsiC()), true));
          continue;
        }
        if (n === '"') {
          flush();
          this.pos += 2;
          parts.push(...this.readDoubleQuoted(false));
          continue;
        }
        const part = this.readDollar(false);
        if (part) {
          flush();
          parts.push(part);
        } else {
          text += '$';
          this.pos++;
        }
        continue;
      }
      if (c === '`') {
        flush();
        parts.push(this.readBacktick(false));
        continue;
      }
      if (c === '\0') {
        flush();
        parts.push({ t: 'interp', i: this.markerAt.get(this.pos), q: false });
        this.pos++;
        continue;
      }
      if ((c === '?' || c === '*' || c === '+' || c === '@' || c === '!') && this.ch(this.pos + 1) === '(' && this.extglobAt(this.pos)) {
        text += this.readExtglob();
        continue;
      }
      if (assignment && c === ':' && this.ch(this.pos + 1) === '~') {
        text += ':';
        this.pos++;
        flush();
        continue;
      }
      text += c;
      this.pos++;
    }
    flush();
    if (this.pos === start) return null;
    if (parts.length === 0) parts.push(lit('', true));
    parts.raw = this.src.slice(start, this.pos).replace(/\0/g, '${...}');
    return parts;
  }

  readExtglob() {
    const start = this.pos;
    this.pos += 2;
    let depth = 1;
    while (depth > 0) {
      const c = this.ch();
      if (c === '') this.fail('unterminated extended glob pattern');
      if (c === '\\') this.pos++;
      else if (c === '(') depth++;
      else if (c === ')') depth--;
      this.pos++;
    }
    return this.src.slice(start, this.pos);
  }

  readProcSub() {
    const dir = this.ch();
    this.pos += 2;
    const body = this.parseList(() => this.peekOp() === ')' || this.atEnd());
    this.expectOp(')', 'to close process substitution');
    return { t: 'procsub', dir, body, q: false };
  }

  readAnsiC() {
    this.pos += 2;
    let out = '';
    for (;;) {
      const c = this.ch();
      if (c === '') this.fail("unterminated $'...' string");
      if (c === '\\') {
        out += c + this.ch(this.pos + 1);
        this.pos += 2;
        continue;
      }
      if (c === "'") {
        this.pos++;
        return out;
      }
      if (c === '\0') this.fail("interpolation inside $'...' is not supported");
      out += c;
      this.pos++;
    }
  }

  readDoubleQuoted(heredoc) {
    const parts = [];
    let text = '';
    let produced = false;
    const flush = () => {
      if (text) {
        parts.push(lit(text, true));
        produced = true;
      }
      text = '';
    };
    for (;;) {
      const c = this.ch();
      if (c === '') {
        if (heredoc) break;
        this.fail('unterminated double quote');
      }
      if (c === '"' && !heredoc) {
        this.pos++;
        break;
      }
      if (c === '\\') {
        const n = this.ch(this.pos + 1);
        if (n === '$' || n === '`' || n === '\\' || (n === '"' && !heredoc)) {
          text += n;
          this.pos += 2;
          continue;
        }
        if (n === '\n') {
          this.pos += 2;
          continue;
        }
        text += c;
        this.pos++;
        continue;
      }
      if (c === '$') {
        const part = this.readDollar(true);
        if (part) {
          flush();
          parts.push(part);
          produced = true;
        } else {
          text += '$';
          this.pos++;
        }
        continue;
      }
      if (c === '`') {
        flush();
        parts.push(this.readBacktick(true));
        produced = true;
        continue;
      }
      if (c === '\0') {
        flush();
        parts.push({ t: 'interp', i: this.markerAt.get(this.pos), q: true });
        produced = true;
        this.pos++;
        continue;
      }
      text += c;
      this.pos++;
    }
    flush();
    if (!produced) parts.push(lit('', true));
    return parts;
  }

  readBacktick(quoted) {
    const start = this.pos + 1;
    let i = start;
    let out = '';
    const markers = [];
    for (;;) {
      if (i >= this.src.length) this.fail('unterminated backquote');
      const c = this.src[i];
      if (c === '`') break;
      if (c === '\\') {
        const n = this.src[i + 1];
        if (n === '`' || n === '\\' || n === '$' || (quoted && n === '"')) {
          out += n;
          i += 2;
          continue;
        }
        if (n === '\n') {
          i += 2;
          continue;
        }
      }
      if (c === '\0') markers.push(this.markerAt.get(i));
      out += c;
      i++;
    }
    this.pos = i + 1;
    const sub = this.sub(out, markers);
    const body = sub.parseProgram();
    return { t: 'cmd', body, q: quoted };
  }

  readDollar(quoted) {
    const n = this.ch(this.pos + 1);
    if (n === '{') return this.readBraceParam(quoted);
    if (n === '(') {
      if (this.ch(this.pos + 2) === '(' && this.looksLikeArith(this.pos + 3)) {
        this.pos += 3;
        return { t: 'arith', expr: this.readArithParts(), q: quoted };
      }
      this.pos += 2;
      const body = this.parseList(() => this.peekOp() === ')' || this.atEnd());
      this.expectOp(')', 'to close command substitution');
      return { t: 'cmd', body, q: quoted };
    }
    if (n === '[') {
      this.pos += 2;
      const expr = this.readUntilBracket();
      return { t: 'arith', expr, q: quoted };
    }
    if (n !== '' && NAME_START.test(n)) {
      NAME_RE.lastIndex = this.pos + 1;
      const m = NAME_RE.exec(this.src);
      this.pos += 1 + m[0].length;
      return { t: 'param', name: m[0], index: null, prefix: '', op: null, q: quoted };
    }
    if (n >= '0' && n <= '9') {
      this.pos += 2;
      return { t: 'param', name: n, index: null, prefix: '', op: null, q: quoted };
    }
    if (n !== '' && isSpecialParam(n)) {
      this.pos += 2;
      return { t: 'param', name: n, index: null, prefix: '', op: null, q: quoted };
    }
    return null;
  }

  readUntilBracket() {
    const parts = [];
    let text = '';
    for (;;) {
      const c = this.ch();
      if (c === '') this.fail("unterminated '$['");
      if (c === ']') {
        this.pos++;
        break;
      }
      if (c === '$') {
        const part = this.readDollar(false);
        if (part) {
          if (text) parts.push(lit(text, false));
          text = '';
          parts.push(part);
          continue;
        }
      }
      text += c;
      this.pos++;
    }
    if (text) parts.push(lit(text, false));
    return parts;
  }

  readParamArg(quoted, stopChars) {
    const stop = c => stopChars.includes(c);
    if (quoted) {
      const parts = [];
      let text = '';
      const flush = () => {
        if (text) parts.push(lit(text, true));
        text = '';
      };
      for (;;) {
        const c = this.ch();
        if (c === '') this.fail('unterminated parameter expansion');
        if (stop(c)) break;
        if (c === '\\') {
          const n = this.ch(this.pos + 1);
          if (n === '$' || n === '`' || n === '"' || n === '\\' || n === '}') {
            text += n;
            this.pos += 2;
            continue;
          }
          text += c;
          this.pos++;
          continue;
        }
        if (c === '"') {
          flush();
          this.pos++;
          parts.push(...this.readDoubleQuoted(false));
          continue;
        }
        if (c === '$') {
          const part = this.readDollar(true);
          if (part) {
            flush();
            parts.push(part);
            continue;
          }
        }
        if (c === '`') {
          flush();
          parts.push(this.readBacktick(true));
          continue;
        }
        if (c === '\0') {
          flush();
          parts.push({ t: 'interp', i: this.markerAt.get(this.pos), q: true });
          this.pos++;
          continue;
        }
        text += c;
        this.pos++;
      }
      flush();
      return parts;
    }
    const parts = [];
    for (;;) {
      const c = this.ch();
      if (c === '') this.fail('unterminated parameter expansion');
      if (stop(c)) break;
      if (c === ' ' || c === '\t' || c === '\n' || c === '|' || c === '&' || c === ';' || c === '(' || c === ')' || c === '<' || c === '>') {
        let text = '';
        while (!stop(this.ch()) && this.ch() !== '' && /[ \t\n|&;()<>]/.test(this.ch())) text += this.src[this.pos++];
        parts.push(lit(text, false));
        continue;
      }
      const word = this.readWord(ch => stop(ch) || /[ \t\n|&;()<>]/.test(ch), true);
      if (!word) break;
      parts.push(...word);
    }
    return parts;
  }

  readBraceParam(quoted) {
    const start = this.pos;
    this.pos += 2;
    const src = this.src;
    let prefix = '';
    if (this.ch() === '#' && this.ch(this.pos + 1) !== '}' && !(this.ch(this.pos + 1) === ':' || ((this.ch(this.pos + 1) === '-' || this.ch(this.pos + 1) === '=' || this.ch(this.pos + 1) === '?' || this.ch(this.pos + 1) === '+') && this.ch(this.pos + 2) !== '}'))) {
      prefix = '#';
      this.pos++;
    } else if (this.ch() === '!' && this.ch(this.pos + 1) !== '}') {
      prefix = '!';
      this.pos++;
    }
    let name;
    const c = this.ch();
    if (c !== '' && NAME_START.test(c)) {
      NAME_RE.lastIndex = this.pos;
      name = NAME_RE.exec(src)[0];
    } else if (c >= '0' && c <= '9') {
      name = /^[0-9]+/.exec(src.slice(this.pos))[0];
    } else if (c !== '' && isSpecialParam(c)) {
      name = c;
    } else this.fail('bad substitution', start);
    this.pos += name.length;

    let index = null;
    if (this.ch() === '[' && /^[A-Za-z_]/.test(name)) {
      this.pos++;
      if ((this.ch() === '@' || this.ch() === '*') && this.ch(this.pos + 1) === ']') {
        index = this.ch();
        this.pos += 2;
      } else {
        let depth = 1;
        let end = this.pos;
        for (; end < src.length; end++) {
          if (src[end] === '[') depth++;
          else if (src[end] === ']' && --depth === 0) break;
        }
        if (end >= src.length) this.fail('bad substitution', start);
        index = this.readWord(() => this.pos >= end, true) || [lit('', false)];
        this.pos = end + 1;
      }
    }

    const node = { t: 'param', name, index, prefix, op: null, q: quoted };
    if (prefix === '!' && (this.ch() === '*' || this.ch() === '@') && this.ch(this.pos + 1) === '}') {
      node.op = 'names';
      node.mode = this.ch();
      this.pos += 2;
      return node;
    }
    if (prefix === '#') {
      if (this.ch() !== '}') this.fail('bad substitution', start);
      this.pos++;
      node.op = 'length';
      return node;
    }

    const op = this.ch();
    if (op === '}') {
      this.pos++;
      return node;
    }
    if (op === ':' && '-=?+'.includes(this.ch(this.pos + 1))) {
      node.op = this.ch(this.pos + 1);
      node.colon = true;
      this.pos += 2;
      node.arg = this.readParamArg(quoted, '}');
    } else if ('-=?+'.includes(op)) {
      node.op = op;
      node.colon = false;
      this.pos++;
      node.arg = this.readParamArg(quoted, '}');
    } else if (op === ':') {
      this.pos++;
      node.op = 'substr';
      node.offset = this.readSubstrExpr(':}');
      if (this.ch() === ':') {
        this.pos++;
        node.length = this.readSubstrExpr('}');
      }
    } else if (op === '#' || op === '%') {
      const doubled = this.ch(this.pos + 1) === op;
      node.op = doubled ? op + op : op;
      this.pos += doubled ? 2 : 1;
      node.arg = this.readParamArg(false, '}');
    } else if (op === '/') {
      this.pos++;
      let mode = '/';
      if (this.ch() === '/') {
        mode = '//';
        this.pos++;
      } else if (this.ch() === '#' || this.ch() === '%') {
        mode = '/' + this.ch();
        this.pos++;
      }
      node.op = mode;
      node.arg = this.readParamArg(false, '/}');
      if (this.ch() === '/') {
        this.pos++;
        node.rep = this.readParamArg(quoted, '}');
      } else node.rep = null;
    } else if (op === '^' || op === ',') {
      const doubled = this.ch(this.pos + 1) === op;
      node.op = doubled ? op + op : op;
      this.pos += doubled ? 2 : 1;
      node.arg = this.readParamArg(false, '}');
    } else if (op === '@') {
      node.op = '@';
      node.transform = this.ch(this.pos + 1);
      if (!'QEPAaULuK'.includes(node.transform) || node.transform === '') this.fail('bad substitution', start);
      this.pos += 2;
    } else {
      this.fail('bad substitution', start);
    }
    if (this.ch() !== '}') this.fail('bad substitution', start);
    this.pos++;
    return node;
  }

  readSubstrExpr(stops) {
    const parts = [];
    let text = '';
    let depth = 0;
    for (;;) {
      const c = this.ch();
      if (c === '') this.fail('bad substitution');
      if (depth === 0 && stops.includes(c)) break;
      if (c === '?' && depth === 0) depth = -100;
      if (c === '(') depth++;
      else if (c === ')') depth--;
      if (c === '$' || c === '`' || c === '\0') {
        if (text) parts.push(lit(text, false));
        text = '';
        if (c === '\0') {
          parts.push({ t: 'interp', i: this.markerAt.get(this.pos), q: false });
          this.pos++;
        } else if (c === '`') parts.push(this.readBacktick(false));
        else {
          const part = this.readDollar(false);
          if (part) parts.push(part);
          else {
            text += '$';
            this.pos++;
          }
        }
        continue;
      }
      text += c;
      this.pos++;
    }
    if (text) parts.push(lit(text, false));
    return parts;
  }
}

function unescapeTemplate(segment) {
  if (!segment.includes('\\')) return segment;
  let out = '';
  for (let i = 0; i < segment.length; i++) {
    const c = segment[i];
    if (c !== '\\') {
      out += c;
      continue;
    }
    let run = 0;
    while (segment[i + run] === '\\') run++;
    const next = segment[i + run];
    const jsEscape = run % 2 === 1 && (next === '`' || (next === '$' && segment[i + run + 1] === '{'));
    out += '\\'.repeat(jsEscape ? run - 1 : run);
    i += run - 1;
  }
  return out;
}

export function parseTemplate(segments, raws = null) {
  for (const segment of segments) {
    if (segment.includes('\0')) throw new SyntaxError('ant:shell: template text cannot contain NUL characters');
  }
  const markers = [];
  let source = '';
  for (let i = 0; i < segments.length; i++) {
    source += unescapeTemplate(segments[i]);
    if (i + 1 >= segments.length) break;
    if (raws && raws.has(i)) {
      const raw = raws.get(i);
      if (raw.includes('\0')) throw new SyntaxError('ant:shell: raw interpolation cannot contain NUL characters');
      source += raw;
    } else {
      source += '\0';
      markers.push(i);
    }
  }
  const parser = new Parser(source, markers, { template: true });
  return parser.parseProgram();
}

export function parseSource(text, name = '', lineBase = 0, extglob = false) {
  if (text.includes('\0')) text = text.replace(/\0/g, '');
  const parser = new Parser(text, [], { template: false, name, lineBase, extglob });
  return parser.parseProgram();
}
