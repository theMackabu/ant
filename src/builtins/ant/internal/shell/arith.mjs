// @ant: compact_on_build
export class ArithError extends Error {
  constructor(message, token) {
    super(message);
    this.token = token;
  }
}

const TWO31 = 2147483648;
const MAX_SAFE = 9007199254740991n;

function norm(b) {
  b = BigInt.asIntN(64, b);
  return b >= -MAX_SAFE && b <= MAX_SAFE ? Number(b) : b;
}

function small(x) {
  return typeof x === 'number' && x >= -TWO31 && x < TWO31;
}

function both(a, b) {
  return typeof a === 'number' && typeof b === 'number';
}

function isZero(x) {
  return x === 0 || x === 0n;
}

export const ops = {
  add(a, b) {
    if (both(a, b)) {
      const r = a + b;
      if (Number.isSafeInteger(r)) return r;
    }
    return norm(BigInt(a) + BigInt(b));
  },
  sub(a, b) {
    if (both(a, b)) {
      const r = a - b;
      if (Number.isSafeInteger(r)) return r;
    }
    return norm(BigInt(a) - BigInt(b));
  },
  mul(a, b) {
    if (both(a, b)) {
      const r = a * b;
      if (Number.isSafeInteger(r)) return r === 0 ? 0 : r;
    }
    return norm(BigInt(a) * BigInt(b));
  },
  div(a, b) {
    if (isZero(b)) throw new ArithError('division by 0', String(b));
    if (both(a, b) && small(a) && small(b)) {
      const q = Math.trunc(a / b);
      return q === 0 ? 0 : q;
    }
    return norm(BigInt(a) / BigInt(b));
  },
  mod(a, b) {
    if (isZero(b)) throw new ArithError('division by 0', String(b));
    if (both(a, b) && small(a) && small(b)) {
      const r = a % b;
      return r === 0 ? 0 : r;
    }
    return norm(BigInt(a) % BigInt(b));
  },
  pow(a, b) {
    if (b < 0) throw new ArithError('exponent less than 0', String(b));
    if (both(a, b)) {
      const r = a ** b;
      if (Number.isSafeInteger(r)) return r;
    }
    let base = BigInt.asIntN(64, BigInt(a));
    let exp = BigInt(b);
    let result = 1n;
    while (exp > 0n) {
      if (exp & 1n) result = BigInt.asIntN(64, result * base);
      base = BigInt.asIntN(64, base * base);
      exp >>= 1n;
    }
    return norm(result);
  },
  shl(a, b) {
    return norm(BigInt(a) << (BigInt(b) & 63n));
  },
  shr(a, b) {
    return norm(BigInt.asIntN(64, BigInt(a)) >> (BigInt(b) & 63n));
  },
  and(a, b) {
    return small(a) && small(b) ? a & b : norm(BigInt(a) & BigInt(b));
  },
  or(a, b) {
    return small(a) && small(b) ? a | b : norm(BigInt(a) | BigInt(b));
  },
  xor(a, b) {
    return small(a) && small(b) ? a ^ b : norm(BigInt(a) ^ BigInt(b));
  },
  not(a) {
    return small(a) ? ~a : norm(~BigInt(a));
  },
  neg(a) {
    if (typeof a === 'number') return a === 0 ? 0 : -a;
    return norm(-a);
  }
};

const DIGITS = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ@_';

export function parseNumber(text) {
  const s = text;
  let m;
  if ((m = /^([0-9]+)#([0-9A-Za-z@_]+)$/.exec(s))) {
    const base = Number(m[1]);
    if (base < 2 || base > 64) throw new ArithError('invalid arithmetic base', s);
    let value = 0n;
    for (const ch of m[2]) {
      let d = base <= 36 ? DIGITS.indexOf(ch.toLowerCase()) : DIGITS.indexOf(ch);
      if (d < 0 || d >= base) throw new ArithError('value too great for base', s);
      value = value * BigInt(base) + BigInt(d);
    }
    return norm(value);
  }
  if (/^0[xX][0-9a-fA-F]+$/.test(s)) return norm(BigInt(s));
  if (/^0[0-7]+$/.test(s)) return norm(BigInt('0o' + s.slice(1)));
  if (/^[0-9]+$/.test(s)) {
    if (s.length > 1 && s[0] === '0') throw new ArithError('value too great for base', s);
    const n = Number(s);
    return Number.isSafeInteger(n) ? n : norm(BigInt(s));
  }
  throw new ArithError('invalid number', s);
}

const TOKEN_RE = /\s*(?:(0[xX][0-9a-fA-F]+|[0-9]+#[0-9A-Za-z@_]+|[0-9][0-9A-Za-z]*)|([A-Za-z_][A-Za-z0-9_]*)|(<<=|>>=|\*\*|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|\/=|%=|&=|\^=|\|=|[-+*/%<>=!~&^|?:,()\[\]]))/y;

function tokenize(text) {
  const tokens = [];
  TOKEN_RE.lastIndex = 0;
  let pos = 0;
  while (pos < text.length) {
    if (/^\s*$/.test(text.slice(pos))) break;
    TOKEN_RE.lastIndex = pos;
    const m = TOKEN_RE.exec(text);
    if (!m) throw new ArithError('syntax error: invalid arithmetic operator', text.slice(pos).trim());
    const value = m[1] ?? m[2] ?? m[3];
    const start = TOKEN_RE.lastIndex - value.length;
    if (m[1] !== undefined) tokens.push({ k: 'num', v: m[1], s: start });
    else if (m[2] !== undefined) tokens.push({ k: 'id', v: m[2], s: start });
    else tokens.push({ k: 'op', v: m[3], s: start });
    pos = TOKEN_RE.lastIndex;
  }
  return tokens;
}

const ASSIGN_OPS = new Set(['=', '+=', '-=', '*=', '/=', '%=', '<<=', '>>=', '&=', '^=', '|=']);
const BINARY_LEVELS = [
  ['||'], ['&&'], ['|'], ['^'], ['&'], ['==', '!='], ['<', '>', '<=', '>='], ['<<', '>>'], ['+', '-'], ['*', '/', '%']
];

export function parseArith(text) {
  const tokens = tokenize(text);
  let i = 0;
  const peek = () => tokens[i];
  const isOp = v => tokens[i] && tokens[i].k === 'op' && tokens[i].v === v;
  const fail = () => {
    const rest = tokens.slice(i).map(t => t.v).join(' ');
    throw new ArithError(rest ? 'syntax error: operand expected' : 'syntax error: operand expected', rest);
  };

  const comma = () => {
    let node = assign();
    while (isOp(',')) {
      i++;
      node = { t: ',', l: node, r: assign() };
    }
    return node;
  };

  const assign = () => {
    const start = i;
    const target = tryLvalue();
    if (target && tokens[i] && tokens[i].k === 'op' && ASSIGN_OPS.has(tokens[i].v)) {
      const op = tokens[i++].v;
      return { t: '=', op, target, e: assign() };
    }
    i = start;
    return ternary();
  };

  const tryLvalue = () => {
    const tok = peek();
    if (!tok || tok.k !== 'id') return null;
    i++;
    if (isOp('[')) {
      let depth = 0;
      let close = -1;
      for (let j = i; j < tokens.length; j++) {
        if (tokens[j].k !== 'op') continue;
        if (tokens[j].v === '[') depth++;
        else if (tokens[j].v === ']' && --depth === 0) {
          close = j;
          break;
        }
      }
      if (close === -1) fail();
      const key = text.slice(tokens[i].s + 1, tokens[close].s).trim();
      let index = null;
      try {
        index = parseArith(key);
      } catch {}
      i = close + 1;
      return { t: 'v', name: tok.v, index, key };
    }
    return { t: 'v', name: tok.v, index: null, key: null };
  };

  const ternary = () => {
    const cond = binary(0);
    if (!isOp('?')) return cond;
    i++;
    const yes = comma();
    if (!isOp(':')) fail();
    i++;
    const no = assign();
    return { t: '?', c: cond, y: yes, n: no };
  };

  const binary = level => {
    if (level >= BINARY_LEVELS.length) return power();
    let node = binary(level + 1);
    for (;;) {
      const tok = peek();
      if (!tok || tok.k !== 'op' || !BINARY_LEVELS[level].includes(tok.v)) return node;
      i++;
      node = { t: 'b', op: tok.v, l: node, r: binary(level + 1) };
    }
  };

  const power = () => {
    const base = unary();
    if (isOp('**')) {
      i++;
      return { t: 'b', op: '**', l: base, r: power() };
    }
    return base;
  };

  const unary = () => {
    const tok = peek();
    if (tok && tok.k === 'op') {
      if (tok.v === '++' || tok.v === '--') {
        i++;
        const target = tryLvalue();
        if (!target) fail();
        return { t: 'pre', op: tok.v, target };
      }
      if (tok.v === '+' || tok.v === '-' || tok.v === '!' || tok.v === '~') {
        i++;
        return { t: 'u', op: tok.v, e: unary() };
      }
    }
    return postfix();
  };

  const postfix = () => {
    const tok = peek();
    if (tok && tok.k === 'id') {
      const target = tryLvalue();
      if (isOp('++') || isOp('--')) {
        const op = tokens[i++].v;
        return { t: 'post', op, target };
      }
      return target;
    }
    return primary();
  };

  const primary = () => {
    const tok = peek();
    if (!tok) fail();
    if (tok.k === 'num') {
      i++;
      return { t: 'n', v: parseNumber(tok.v) };
    }
    if (isOp('(')) {
      i++;
      const node = comma();
      if (!isOp(')')) fail();
      i++;
      return node;
    }
    fail();
  };

  if (tokens.length === 0) return { t: 'n', v: 0 };
  const node = comma();
  if (i < tokens.length) fail();
  return node;
}

const BINARY_JS = {
  '+': 'A.add', '-': 'A.sub', '*': 'A.mul', '/': 'A.div', '%': 'A.mod', '**': 'A.pow',
  '<<': 'A.shl', '>>': 'A.shr', '&': 'A.and', '|': 'A.or', '^': 'A.xor'
};
const COMPARE_JS = { '<': '<', '>': '>', '<=': '<=', '>=': '>=', '==': '===', '!=': '!==' };
const COMPOUND = { '+=': '+', '-=': '-', '*=': '*', '/=': '/', '%=': '%', '<<=': '<<', '>>=': '>>', '&=': '&', '^=': '^', '|=': '|' };

export function arithToJS(node, cv) {
  const gen = n => {
    switch (n.t) {
      case 'n': return typeof n.v === 'bigint' ? `${n.v}n` : String(n.v);
      case 'v': {
        const name = JSON.stringify(n.name);
        if (n.key === null) return `A.v(${cv},${name})`;
        return `(A.isA(${cv},${name})?A.vs(${cv},${name},${JSON.stringify(n.key)}):A.ve(${cv},${name},${n.index ? gen(n.index) : `A.bad(${JSON.stringify(n.key)})`}))`;
      }
      case ',': return `(${gen(n.l)},${gen(n.r)})`;
      case '?': return `(${gen(n.c)}!==0?${gen(n.y)}:${gen(n.n)})`;
      case 'u':
        if (n.op === '-') return `A.neg(${gen(n.e)})`;
        if (n.op === '+') return gen(n.e);
        if (n.op === '!') return `(${gen(n.e)}===0?1:0)`;
        return `A.not(${gen(n.e)})`;
      case 'b':
        if (n.op === '&&') return `(${gen(n.l)}!==0&&${gen(n.r)}!==0?1:0)`;
        if (n.op === '||') return `(${gen(n.l)}!==0||${gen(n.r)}!==0?1:0)`;
        if (COMPARE_JS[n.op]) return `(${gen(n.l)}${COMPARE_JS[n.op]}${gen(n.r)}?1:0)`;
        return `${BINARY_JS[n.op]}(${gen(n.l)},${gen(n.r)})`;
      case '=': {
        const t = n.target;
        const name = JSON.stringify(t.name);
        const value = n.op === '=' ? gen(n.e) : `${BINARY_JS[COMPOUND[n.op]]}(${gen(t)},${gen(n.e)})`;
        if (t.key !== null) return `(A.isA(${cv},${name})?A.ss(${cv},${name},${JSON.stringify(t.key)},${value}):A.se(${cv},${name},${t.index ? gen(t.index) : `A.bad(${JSON.stringify(t.key)})`},${value}))`;
        return `A.s(${cv},${name},${value})`;
      }
      case 'pre':
      case 'post': {
        const t = n.target;
        const delta = n.op === '++' ? 1 : -1;
        if (t.key !== null) return `(A.isA(${cv},${JSON.stringify(t.name)})?A.incs(${cv},${JSON.stringify(t.name)},${JSON.stringify(t.key)},${delta},${n.t === 'post'}):A.ince(${cv},${JSON.stringify(t.name)},${t.index ? gen(t.index) : `A.bad(${JSON.stringify(t.key)})`},${delta},${n.t === 'post'}))`;
        return `A.inc(${cv},${JSON.stringify(t.name)},${delta},${n.t === 'post'})`;
      }
    }
    throw new Error('unknown arithmetic node');
  };
  return gen(node);
}

export function evalArith(A, c, n) {
  switch (n.t) {
    case 'n': return n.v;
    case 'v':
      if (n.key === null) return A.v(c, n.name);
      if (A.isA(c, n.name)) return A.vs(c, n.name, n.key);
      return A.ve(c, n.name, n.index ? evalArith(A, c, n.index) : A.bad(n.key));
    case ',': evalArith(A, c, n.l); return evalArith(A, c, n.r);
    case '?': return evalArith(A, c, n.c) !== 0 ? evalArith(A, c, n.y) : evalArith(A, c, n.n);
    case 'u': {
      const v = evalArith(A, c, n.e);
      if (n.op === '-') return A.neg(v);
      if (n.op === '+') return v;
      if (n.op === '!') return v === 0 ? 1 : 0;
      return A.not(v);
    }
    case 'b': {
      if (n.op === '&&') return evalArith(A, c, n.l) !== 0 && evalArith(A, c, n.r) !== 0 ? 1 : 0;
      if (n.op === '||') return evalArith(A, c, n.l) !== 0 || evalArith(A, c, n.r) !== 0 ? 1 : 0;
      const l = evalArith(A, c, n.l);
      const r = evalArith(A, c, n.r);
      return binaryValue(A, n.op, l, r);
    }
    case '=': {
      const t = n.target;
      const value = n.op === '=' ? evalArith(A, c, n.e) : binaryValue(A, COMPOUND[n.op], evalArith(A, c, t), evalArith(A, c, n.e));
      if (t.key === null) return A.s(c, t.name, value);
      if (A.isA(c, t.name)) return A.ss(c, t.name, t.key, value);
      return A.se(c, t.name, t.index ? evalArith(A, c, t.index) : A.bad(t.key), value);
    }
    case 'pre':
    case 'post': {
      const t = n.target;
      const delta = n.op === '++' ? 1 : -1;
      if (t.key === null) return A.inc(c, t.name, delta, n.t === 'post');
      if (A.isA(c, t.name)) return A.incs(c, t.name, t.key, delta, n.t === 'post');
      return A.ince(c, t.name, t.index ? evalArith(A, c, t.index) : A.bad(t.key), delta, n.t === 'post');
    }
  }
  throw new Error('unknown arithmetic node');
}

function binaryValue(A, op, l, r) {
  switch (op) {
    case '+': return A.add(l, r);
    case '-': return A.sub(l, r);
    case '*': return A.mul(l, r);
    case '/': return A.div(l, r);
    case '%': return A.mod(l, r);
    case '**': return A.pow(l, r);
    case '<<': return A.shl(l, r);
    case '>>': return A.shr(l, r);
    case '&': return A.and(l, r);
    case '|': return A.or(l, r);
    case '^': return A.xor(l, r);
    case '<': return l < r ? 1 : 0;
    case '>': return l > r ? 1 : 0;
    case '<=': return l <= r ? 1 : 0;
    case '>=': return l >= r ? 1 : 0;
    case '==': return l === r ? 1 : 0;
    case '!=': return l !== r ? 1 : 0;
  }
  throw new ArithError('syntax error', op);
}
