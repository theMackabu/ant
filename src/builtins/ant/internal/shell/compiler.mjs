// @ant: compact_on_build
import { parseArith, arithToJS } from 'ant:internal/shell/arith';
import { formatFunction } from 'ant:internal/shell/format';

const S = JSON.stringify;
const BRACE_ITEM = Symbol('brace-item');

function hasCmdSub(parts) {
  for (const part of parts) {
    if (part.t === 'cmd' || part.t === 'procsub') return true;
    if (part.t === 'param') {
      if (part.arg && hasCmdSub(part.arg)) return true;
      if (part.rep && hasCmdSub(part.rep)) return true;
      if (part.index && Array.isArray(part.index) && hasCmdSub(part.index)) return true;
    }
    if (part.t === 'arith' && hasCmdSub(part.expr)) return true;
  }
  return false;
}

function staticText(parts) {
  let text = '';
  for (const part of parts) {
    if (part.t !== 'lit') return null;
    text += part.v;
  }
  return text;
}

const GLOB_CHARS = /[*?]|\[.*\]|[+@!]\(/;

function wordIsPlain(word, assignment = false) {
  let text = '';
  for (const part of word) {
    if (part.t !== 'lit') return null;
    if (!part.q && (GLOB_CHARS.test(part.v) || (text === '' && part.v.startsWith('~')) || (assignment && /(^|:)~/.test(part.v)))) return null;
    text += part.v;
  }
  return text;
}

function emptyUnquoted(word) {
  return word.every(part => part.t === 'lit' && !part.q && part.v === '');
}

function braceAtoms(word) {
  const atoms = [];
  for (const part of word) {
    if (part.t === 'lit' && !part.q) for (const ch of part.v) atoms.push(ch);
    else atoms.push({ [BRACE_ITEM]: part });
  }
  return atoms;
}

function atomsToWord(atoms) {
  const word = [];
  let text = '';
  for (const atom of atoms) {
    if (typeof atom === 'string') text += atom;
    else {
      if (text) word.push({ t: 'lit', v: text, q: false });
      text = '';
      word.push(atom[BRACE_ITEM]);
    }
  }
  if (text) word.push({ t: 'lit', v: text, q: false });
  if (!word.length) word.push({ t: 'lit', v: '', q: false });
  return word;
}

function findBrace(atoms, from) {
  for (let i = from; i < atoms.length; i++) {
    if (atoms[i] === '\\') {
      i++;
      continue;
    }
    if (atoms[i] !== '{') continue;
    if (atoms[i + 1] === '}' && i + 1 < atoms.length) continue;
    let depth = 0;
    const commas = [];
    for (let j = i; j < atoms.length; j++) {
      const a = atoms[j];
      if (a === '{') depth++;
      else if (a === '}') {
        depth--;
        if (depth === 0) {
          if (commas.length) return { open: i, close: j, commas };
          const inner = atoms.slice(i + 1, j);
          if (inner.every(x => typeof x === 'string')) {
            const seq = inner.join('');
            if (/^(-?\d+)\.\.(-?\d+)(\.\.(-?\d+))?$/.test(seq) || /^([A-Za-z])\.\.([A-Za-z])(\.\.(-?\d+))?$/.test(seq)) {
              return { open: i, close: j, seq };
            }
          }
          break;
        }
      } else if (a === ',' && depth === 1) commas.push(j);
    }
  }
  return null;
}

function sequence(seq) {
  let m = /^(-?\d+)\.\.(-?\d+)(?:\.\.(-?\d+))?$/.exec(seq);
  if (m) {
    const a = Number(m[1]);
    const b = Number(m[2]);
    let step = Math.abs(Number(m[3] || 1)) || 1;
    const width = (/^-?0\d/.test(m[1]) || /^-?0\d/.test(m[2])) ? Math.max(m[1].replace('-', '').length, m[2].replace('-', '').length) : 0;
    const out = [];
    const dir = a <= b ? 1 : -1;
    for (let v = a; dir > 0 ? v <= b : v >= b; v += dir * step) {
      let text = String(Math.abs(v));
      if (width) text = text.padStart(width - (v < 0 ? 1 : 0), '0');
      out.push((v < 0 ? '-' : '') + text);
      if (out.length > 100000) break;
    }
    return out;
  }
  m = /^([A-Za-z])\.\.([A-Za-z])(?:\.\.(-?\d+))?$/.exec(seq);
  const a = m[1].charCodeAt(0);
  const b = m[2].charCodeAt(0);
  const step = Math.abs(Number(m[3] || 1)) || 1;
  const out = [];
  const dir = a <= b ? 1 : -1;
  for (let v = a; dir > 0 ? v <= b : v >= b; v += dir * step) out.push(String.fromCharCode(v));
  return out;
}

function expandAtoms(atoms) {
  const found = findBrace(atoms, 0);
  if (!found) return [atoms];
  const prefix = atoms.slice(0, found.open);
  const suffix = atoms.slice(found.close + 1);
  const alternatives = [];
  if (found.seq !== undefined) {
    for (const item of sequence(found.seq)) alternatives.push([...item]);
  } else {
    let start = found.open + 1;
    for (const comma of [...found.commas, found.close]) {
      alternatives.push(atoms.slice(start, comma));
      start = comma + 1;
    }
  }
  const out = [];
  for (const alt of alternatives) {
    for (const expanded of expandAtoms([...prefix, ...alt, ...suffix])) out.push(expanded);
  }
  return out;
}

export function braceExpand(word) {
  let candidate = false;
  for (const part of word) {
    if (part.t === 'lit' && !part.q && part.v.includes('{')) candidate = true;
  }
  if (!candidate) return [word];
  const expanded = expandAtoms(braceAtoms(word));
  if (expanded.length === 1) return [word];
  return expanded.map(atomsToWord);
}

class Compiler {
  constructor() {
    this.n = 0;
    this.loops = [];
    this.inFunction = false;
  }

  id(prefix) {
    return `${prefix}${++this.n}`;
  }

  closure(cv, fn, options = {}) {
    const saved = [this.loops, this.inFunction];
    this.loops = [];
    this.inFunction = !!options.func;
    const inner = this.id('c');
    const body = fn(inner);
    [this.loops, this.inFunction] = saved;
    return `async(${inner})=>{${body}}`;
  }

  pushParts(T, parts, cv, opts = {}) {
    let code = '';
    let pending = [];
    const flushPending = () => {
      if (pending.length) code += `${T}.push(${pending.join(',')});`;
      pending = [];
    };
    const emit = (flag, expr) => pending.push(String(flag), expr);
    const statement = text => {
      flushPending();
      code += text;
    };

    for (let index = 0; index < parts.length; index++) {
      const part = parts[index];
      switch (part.t) {
        case 'lit': {
          let text = part.v;
          if (!part.q && opts.tilde && (index === 0 || (opts.assignment && parts[index - 1].t === 'lit' && !parts[index - 1].q && parts[index - 1].v.endsWith(':'))) && text.startsWith('~')) {
            const m = /^~([A-Za-z0-9._+-]*)/.exec(text);
            const rest = text.slice(m[0].length);
            if (rest === '' || rest.startsWith('/') || (opts.assignment && rest.startsWith(':'))) {
              if (rest === '' && index + 1 < parts.length && !(parts[index + 1].t === 'lit' && parts[index + 1].q === false)) {
                emit(0, S(text));
                break;
              }
              emit(1, `R.tilde(${cv},${S(m[1])})`);
              text = rest;
              if (!text) break;
            }
          }
          emit(part.q ? 1 : opts.split ? 2 : 0, S(text));
          break;
        }
        case 'interp':
          emit(part.q ? 1 : 4, `V[${part.i}]`);
          break;
        case 'param':
          statement(this.param(T, part, cv, opts));
          break;
        case 'cmd': {
          const flag = part.q ? 3 : 2;
          const list = part.body;
          const only = list.items.length === 1 && !list.items[0].async ? list.items[0].node : null;
          if (only && only.t === 'simple' && only.words.length === 0 && only.assigns.length === 0 && only.redirs.length === 1 && only.redirs[0].op === '<' && only.redirs[0].fd === null) {
            const t = this.id('t');
            statement(`{const ${t}=[];${this.pushParts(t, only.redirs[0].target, cv, { tilde: true })}${T}.push(${flag},await R.slurp(${cv},R.S(${cv},${t})));}`);
          } else {
            statement(`${T}.push(${flag},await R.cs(${cv},${this.closure(cv, c => this.list(list, c, false))}));`);
          }
          break;
        }
        case 'arith': {
          const flag = part.q ? 3 : 2;
          const expr = this.arith(part.expr, cv);
          statement(expr.pre);
          emit(flag, `String(${expr.js})`);
          break;
        }
        case 'procsub':
          statement(`${T}.push(1,await R.procsub(${cv},${S(part.dir)},${this.closure(cv, c => this.list(part.body, c, false))}));`);
          break;
        default:
          throw new Error(`unknown word part ${part.t}`);
      }
    }
    flushPending();
    return code;
  }

  stringExpr(parts, cv, pre, opts = {}) {
    const text = wordIsPlain(parts, opts.assignment);
    if (text !== null && !(opts.tilde && text.startsWith('~'))) return S(text);
    if (parts.length === 1) {
      const part = parts[0];
      if (part.t === 'arith') {
        const expr = this.arith(part.expr, cv);
        if (expr.pre) pre.push(expr.pre);
        return `String(${expr.js})`;
      }
      if (part.t === 'param' && part.op === null && !part.prefix && part.index === null && part.name !== '@' && part.name !== '*') {
        return `R.get(${cv},${S(part.name)},undefined)`;
      }
    }
    const t = this.id('t');
    pre.push(`const ${t}=[];${this.pushParts(t, parts, cv, opts)}`);
    return `R.S(${cv},${t})`;
  }

  patternExpr(parts, cv, pre) {
    const t = this.id('t');
    pre.push(`const ${t}=[];${this.pushParts(t, parts, cv, {})}`);
    return `R.Pt(${cv},${t})`;
  }

  indexExpr(part, cv, pre) {
    if (part.index === null || part.index === undefined) return 'undefined';
    if (part.index === '@') return 'R.AT';
    if (part.index === '*') return 'R.ST';
    return this.stringExpr(part.index, cv, pre, {});
  }

  param(T, part, cv, opts) {
    const pre = [];
    const flag = part.q ? 3 : 2;
    let name = S(part.name);
    if (part.prefix === '!' && part.op !== 'names' && !((part.index === '@' || part.index === '*') && part.op === null)) {
      name = `R.ind(${cv},${S(part.name)})`;
    }
    const idx = this.indexExpr(part, cv, pre);
    const get = `R.get(${cv},${name},${idx})`;
    const marker = part.q && !(part.index === '@' || part.name === '@') ? `${T}.push(1,"");` : '';
    let body;
    switch (part.op ?? null) {
      case null:
        if (part.prefix === '!' && (part.index === '@' || part.index === '*')) body = `${T}.push(${flag},R.keys(${cv},${name},${idx}));`;
        else body = `${T}.push(${flag},${get});`;
        break;
      case 'names':
        body = `${T}.push(${flag},R.names(${cv},${S(part.name)},${S(part.mode)}));`;
        break;
      case 'length':
        body = `${T}.push(${flag},R.len(${cv},${name},${idx}));`;
        break;
      case '-':
      case '=':
      case '?':
      case '+': {
        const test = `R.isset(${cv},${name},${idx},${part.colon ? 1 : 0})`;
        const argOpts = { split: !part.q, tilde: true };
        if (part.op === '-') {
          body = `${marker}if(${test}){${T}.push(${flag},${get});}else{${this.pushParts(T, part.arg, cv, argOpts)}}`;
        } else if (part.op === '+') {
          body = `${marker}if(${test}){${this.pushParts(T, part.arg, cv, argOpts)}}`;
        } else if (part.op === '=') {
          const t = this.id('t');
          body = `${marker}if(${test}){${T}.push(${flag},${get});}else{const ${t}=[];${this.pushParts(t, part.arg, cv, { tilde: true })}${T}.push(${flag},R.setp(${cv},${name},${idx},R.S(${cv},${t})));}`;
        } else {
          const t = this.id('t');
          body = `if(${test}){${T}.push(${flag},${get});}else{const ${t}=[];${this.pushParts(t, part.arg, cv, {})}R.perr(${cv},${name},R.S(${cv},${t}),${part.colon ? 1 : 0});}`;
        }
        break;
      }
      case '#':
      case '##':
      case '%':
      case '%%': {
        const p = this.patternExpr(part.arg, cv, pre);
        body = `${T}.push(${flag},R.trim(${cv},${get},${S(part.op)},${p}));`;
        break;
      }
      case '/':
      case '//':
      case '/#':
      case '/%': {
        const p = this.patternExpr(part.arg, cv, pre);
        const rep = part.rep ? this.stringExpr(part.rep, cv, pre, {}) : '""';
        body = `${T}.push(${flag},R.repl(${cv},${get},${S(part.op)},${p},${rep}));`;
        break;
      }
      case '^':
      case '^^':
      case ',':
      case ',,': {
        const p = part.arg.length ? this.patternExpr(part.arg, cv, pre) : 'null';
        body = `${T}.push(${flag},R.cased(${cv},${get},${S(part.op)},${p}));`;
        break;
      }
      case 'substr': {
        const off = this.arith(part.offset, cv);
        pre.push(off.pre);
        let len = 'undefined';
        if (part.length) {
          const l = this.arith(part.length, cv);
          pre.push(l.pre);
          len = l.js;
        }
        body = `${T}.push(${flag},R.substr(${cv},${name},${idx},${off.js},${len}));`;
        break;
      }
      case '@':
        body = `${T}.push(${flag},R.xform(${cv},${name},${idx},${S(part.transform)}));`;
        break;
      default:
        throw new Error(`unknown parameter operator ${part.op}`);
    }
    return pre.length ? `{${pre.join('')}${body}}` : body;
  }

  arith(parts, cv) {
    const text = staticText(parts);
    if (text !== null) {
      try {
        const ast = parseArith(text);
        return { pre: '', js: arithToJS(ast, cv) };
      } catch (error) {
        return { pre: '', js: `R.arithFail(${cv},${S(text)},${S(error.message)},${S(error.token || '')})` };
      }
    }
    const t = this.id('t');
    return {
      pre: `const ${t}=[];${this.pushParts(t, parts, cv, {})}`,
      js: `R.arith(${cv},R.S(${cv},${t}))`
    };
  }

  argv(words, cv) {
    const expanded = [];
    for (const word of words) {
      if (word.assign) expanded.push(word);
      else for (const item of braceExpand(word)) if (!emptyUnquoted(item)) expanded.push(item);
    }
    const literals = expanded.map(w => (w.assign ? null : wordIsPlain(w)));
    if (literals.every(x => x !== null)) return { pre: '', expr: `[${literals.map(S).join(',')}]` };
    const a = this.id('a');
    let code = `const ${a}=[`;
    let i = 0;
    for (; i < expanded.length && literals[i] !== null; i++) code += (i ? ',' : '') + S(literals[i]);
    code += '];';
    for (; i < expanded.length; i++) {
      const word = expanded[i];
      if (literals[i] !== null) {
        code += `${a}.push(${S(literals[i])});`;
        continue;
      }
      if (word.assign) {
        const pre = [];
        const value = this.assignExpr(word.assign, cv, pre);
        code += `{${pre.join('')}${a}.push(R.AW(${value}));}`;
        continue;
      }
      if (word.length === 1 && word[0].t === 'interp' && !word[0].q) {
        code += `R.pushInterp(${a},V[${word[0].i}]);`;
        continue;
      }
      if (word.length === 1 && word[0].t === 'param' && word[0].q && word[0].op === null && !word[0].prefix && word[0].index === null && word[0].name !== '@' && word[0].name !== '*') {
        code += `${a}.push(R.get(${cv},${S(word[0].name)},undefined));`;
        continue;
      }
      const t = this.id('t');
      code += `{const ${t}=[];${this.pushParts(t, word, cv, { tilde: true })}R.F(${cv},${t},${a});}`;
    }
    return { pre: code, expr: a };
  }

  assignExpr(assign, cv, pre) {
    const idx = assign.index ? this.stringExpr(assign.index, cv, pre, {}) : 'undefined';
    if (assign.array) {
      const list = this.id('l');
      let code = `const ${list}=[];`;
      for (const element of assign.array) {
        const key = element.key ? this.stringExpr(element.key, cv, pre, {}) : 'null';
        if (element.key) {
          const value = this.stringExpr(element.value, cv, pre, { tilde: true, assignment: true });
          code += `${list}.push([${key},${value},${element.append ? 1 : 0}]);`;
        } else {
          for (const word of braceExpand(element.value)) {
            const t = this.id('t');
            code += `{const ${t}=[];${this.pushParts(t, word, cv, { tilde: true })}for(const v of R.F(${cv},${t},[]))${list}.push([null,v,0]);}`;
          }
        }
      }
      pre.push(code);
      return `{n:${S(assign.name)},i:${idx},v:null,a:${list},p:${assign.append}}`;
    }
    const value = this.stringExpr(assign.value, cv, pre, { tilde: true, assignment: true });
    return `{n:${S(assign.name)},i:${idx},v:${value},a:null,p:${assign.append}}`;
  }

  redirs(redirs, cv) {
    if (!redirs.length) return { pre: '', expr: 'null' };
    const pre = [];
    const items = [];
    for (const redir of redirs) {
      let target;
      if (redir.heredoc) {
        const body = redir.heredoc.body;
        target = this.stringExpr(body, cv, pre, {});
      } else if (redir.op === '<<<') {
        target = `${this.stringExpr(redir.target, cv, pre, { tilde: true })}+"\\n"`;
      } else {
        const t = this.id('t');
        pre.push(`const ${t}=[];${this.pushParts(t, redir.target, cv, { tilde: true })}`);
        target = `R.RT(${cv},${t})`;
      }
      items.push(`[${S(redir.heredoc ? '<<' : redir.op)},${redir.fd === null ? -1 : redir.fd},${target}${redir.fdVar ? `,${S(redir.fdVar)}` : ''}]`);
    }
    return { pre: pre.join(''), expr: `[${items.join(',')}]` };
  }

  withRedirs(redirs, cv, body) {
    if (!redirs || !redirs.length) return body;
    const r = this.redirs(redirs, cv);
    const tok = this.id('r');
    return `{${r.pre}const ${tok}=await R.redir(${cv},${r.expr});if(${tok}){try{${body}}finally{await R.unredir(${cv},${tok});}}}`;
  }

  list(list, cv, chk) {
    let code = '';
    for (const item of list.items) {
      if (item.async) {
        code += `await R.bg(${cv},${this.closure(cv, c => this.andor(item.node, c, false))},${S(item.text || '')});${cv}.status=0;`;
      } else {
        code += this.andor(item.node, cv, chk);
      }
    }
    return code;
  }

  guarded(code, cv) {
    return `${cv}.ne++;${code}${cv}.ne--;`;
  }

  andor(node, cv, chk) {
    if (node.t !== 'andor') return this.pipeline(node, cv, chk);
    let code = this.guarded(this.pipeline(node.first, cv, false), cv);
    node.rest.forEach((link, index) => {
      const last = index === node.rest.length - 1;
      const body = this.pipeline(link.pipe, cv, last && chk);
      code += `if(${cv}.status${link.op === '&&' ? '===' : '!=='}0){${last ? body : this.guarded(body, cv)}}`;
    });
    return code;
  }

  pipeline(node, cv, chk) {
    if (node.t !== 'pipeline') return this.command(node, cv, chk);
    let code = '';
    let timer = null;
    if (node.time) {
      timer = this.id('tm');
      code += `const ${timer}=R.now();`;
    }
    if (node.cmds.length === 1) {
      const inner = this.command(node.cmds[0], cv, false);
      code += node.bang ? this.guarded(inner, cv) : inner;
    } else {
      const stages = node.cmds.map(cmd => this.stage(cmd, cv));
      code += `${cv}.ln=${node.line};await R.pipe(${cv},[${stages.join(',')}],${S(node.errs)});`;
    }
    if (node.bang) code += `${cv}.status=${cv}.status===0?1:0;`;
    if (timer) code += `R.time(${cv},${timer});`;
    if (chk && !node.bang) code += `if(${cv}.status!==0)await R.ck(${cv});`;
    return node.time || node.cmds.length === 1 ? `{${code}}` : code;
  }

  stage(cmd, cv) {
    const saved = [this.loops, this.inFunction];
    this.loops = [];
    this.inFunction = false;
    const inner = this.id('c');
    let fn;
    if (cmd.t === 'simple' && cmd.words.length > 0) {
      const argv = this.argv(cmd.words, inner);
      const r = this.redirs(cmd.redirs, inner);
      const pre = [];
      const assigns = cmd.assigns.map(a => this.assignExpr(a, inner, pre));
      fn = `async(${inner})=>{${inner}.ln=${cmd.line};${argv.pre}${r.pre}${pre.join('')}return R.D(${argv.expr},${assigns.length ? `[${assigns.join(',')}]` : 'null'},${r.expr});}`;
    } else {
      const body = this.closure(inner, c => this.command(cmd, c, false));
      fn = `(${inner})=>R.J(${body})`;
    }
    [this.loops, this.inFunction] = saved;
    return fn;
  }

  command(node, cv, chk) {
    const ck = chk ? `if(${cv}.status!==0)await R.ck(${cv});` : '';
    switch (node.t) {
      case 'simple':
        return this.simple(node, cv, chk);
      case 'group':
        return this.withRedirs(node.redirs, cv, this.list(node.body, cv, chk));
      case 'subshell':
        return this.withRedirs(node.redirs, cv, `await R.sub(${cv},${this.closure(cv, c => this.list(node.body, c, true))});${ck}`);
      case 'if':
        return this.withRedirs(node.redirs, cv, this.ifCommand(node, cv, chk));
      case 'while':
        return this.withRedirs(node.redirs, cv, this.whileCommand(node, cv, chk));
      case 'for':
        return this.withRedirs(node.redirs, cv, this.forCommand(node, cv, chk));
      case 'cfor':
        return this.withRedirs(node.redirs, cv, this.cforCommand(node, cv, chk));
      case 'select':
        return this.withRedirs(node.redirs, cv, this.selectCommand(node, cv, chk));
      case 'coproc':
        return `await R.coproc(${cv},${S(node.name)},${this.closure(cv, c => this.command(node.body, c, false))},${S(node.text)});`;
      case 'case':
        return this.withRedirs(node.redirs, cv, this.caseCommand(node, cv, chk));
      case 'cond': {
        const v = this.id('b');
        const code = `${cv}.ln=${node.line};let ${v}=false;${this.cond(node.expr, cv, v)}${cv}.status=${v}?0:1;`;
        return this.withRedirs(node.redirs, cv, `{${code}}${ck}`);
      }
      case 'arithcmd': {
        const expr = this.arith(node.expr, cv);
        return this.withRedirs(node.redirs, cv, `{${cv}.ln=${node.line};${expr.pre}${cv}.status=(${expr.js})!==0?0:1;}${ck}`);
      }
      case 'func': {
        const redirs = node.body.redirs && node.body.redirs.length ? node.body.redirs : null;
        const fnBody = this.closure(cv, c => {
          const inner = node.body.t === 'group' ? this.list(node.body.body, c, true) : this.command({ ...node.body, redirs: [] }, c, true);
          return `${this.withRedirs(redirs, c, inner)}return ${c}.status;`;
        }, { func: true });
        return `R.defun(${cv},${S(node.name)},${fnBody},${S(formatFunction(node.name, node.body))});${cv}.status=0;`;
      }
      default:
        throw new Error(`unknown command ${node.t}`);
    }
  }

  simple(node, cv, chk) {
    const ck = chk ? `if(${cv}.status!==0)await R.ck(${cv});` : '';
    let code = `${cv}.ln=${node.line};`;
    if (node.words.length === 0) {
      const sub = node.assigns.some(a => hasCmdSub(a.value || []) || (a.array || []).some(e => hasCmdSub(e.value) || (e.key && hasCmdSub(e.key))) || (a.index && hasCmdSub(a.index)));
      code += `${cv}.ls=0;`;
      for (const assign of node.assigns) {
        const pre = [];
        const value = this.assignExpr(assign, cv, pre);
        code += `{${pre.join('')}R.assign(${cv},${value});}`;
      }
      if (node.redirs.length) {
        const r = this.redirs(node.redirs, cv);
        code += `{${r.pre}await R.run(${cv},[],null,${r.expr});}`;
        if (sub) code += `if(${cv}.status===0)${cv}.status=${cv}.ls;`;
      } else {
        code += `${cv}.status=${sub ? `${cv}.ls` : '0'};`;
      }
      return `{${code}}${ck}`;
    }

    const first = wordIsPlain(node.words[0]);
    if ((first === 'break' || first === 'continue') && node.assigns.length === 0 && node.redirs.length === 0 && node.words.length <= 2) {
      const count = node.words.length === 2 ? wordIsPlain(node.words[1]) : '1';
      if (count !== null && /^[1-9][0-9]*$/.test(count)) {
        const n = Math.min(Number(count), this.loops.length);
        if (n > 0) {
          const target = this.loops[this.loops.length - n];
          if (first === 'break') return `{${cv}.status=0;${target.status}=0;break ${target.label};}`;
          return `{${cv}.status=0;continue ${target.label};}`;
        }
      }
    }

    const argv = this.argv(node.words, cv);
    const r = this.redirs(node.redirs, cv);
    code += argv.pre + r.pre;

    if (first === 'return' && node.redirs.length === 0 && node.assigns.length === 0) {
      if (this.inFunction) return `{${code}return R.ret(${cv},${argv.expr});}`;
      return `{${code}throw R.Ret(${cv},${argv.expr});}`;
    }

    if (node.assigns.length) {
      const frame = this.id('f');
      let assigns = '';
      for (const assign of node.assigns) {
        const pre = [];
        const value = this.assignExpr(assign, cv, pre);
        assigns += `{${pre.join('')}R.tset(${cv},${frame},${value});}`;
      }
      code += `const ${frame}=R.tframe(${cv});try{${assigns}}catch(e){R.tpop(${cv},${frame});throw e;}`;
      code += `await R.run(${cv},${argv.expr},${frame},${r.expr});`;
    } else {
      code += `await R.run(${cv},${argv.expr},null,${r.expr});`;
    }
    return `{${code}}${ck}`;
  }

  loop(fn) {
    const label = this.id('L');
    const status = this.id('s');
    this.loops.push({ label, status });
    const code = fn(label, status);
    this.loops.pop();
    return code;
  }

  ifCommand(node, cv, chk) {
    let code = '';
    let close = '';
    node.clauses.forEach((clause, index) => {
      if (index > 0) {
        code += 'else{';
        close += '}';
      }
      code += this.guarded(this.list(clause.cond, cv, false), cv);
      code += `if(${cv}.status===0){${this.list(clause.body, cv, chk)}}`;
    });
    if (node.else) code += `else{${this.list(node.else, cv, chk)}}`;
    else code += `else{${cv}.status=0;}`;
    return `{${code}${close}}`;
  }

  whileCommand(node, cv, chk) {
    return this.loop((label, status) => {
      const cond = this.guarded(this.list(node.cond, cv, false), cv);
      const body = this.list(node.body, cv, chk);
      return `{let ${status}=0;${label}:for(;;){${cond}if(${cv}.status${node.until ? '===' : '!=='}0)break;${body}${status}=${cv}.status;}${cv}.status=${status};}`;
    });
  }

  forCommand(node, cv, chk) {
    let items;
    let pre = '';
    if (node.words === null) items = `R.positional(${cv})`;
    else {
      const argv = this.argv(node.words, cv);
      pre = argv.pre;
      items = argv.expr;
    }
    return this.loop((label, status) => {
      const v = this.id('v');
      const body = this.list(node.body, cv, chk);
      return `{${pre}let ${status}=0;${label}:for(const ${v} of ${items}){R.setv(${cv},${S(node.name)},${v});${body}${status}=${cv}.status;}${cv}.status=${status};}`;
    });
  }

  selectCommand(node, cv, chk) {
    let items;
    let pre = '';
    if (node.words === null) items = `R.positional(${cv})`;
    else {
      const argv = this.argv(node.words, cv);
      pre = argv.pre;
      items = argv.expr;
    }
    return this.loop((label, status) => {
      const v = this.id('v');
      const state = this.id('q');
      const body = this.list(node.body, cv, chk);
      return `{${pre}const ${state}={m:true,items:${items}};let ${status}=0;${label}:for(;;){const ${v}=await R.select(${cv},${state});if(${v}===null)break;R.setv(${cv},${S(node.name)},${v});${body}${status}=${cv}.status;}${cv}.status=${status};}`;
    });
  }

  cforCommand(node, cv, chk) {
    return this.loop((label, status) => {
      const init = node.init.length ? this.arith(node.init, cv) : null;
      const cond = node.cond.length ? this.arith(node.cond, cv) : null;
      const step = node.step.length ? this.arith(node.step, cv) : null;
      const first = this.id('f');
      const body = this.list(node.body, cv, chk);
      let code = `{let ${status}=0;`;
      if (init) code += `{${init.pre}${init.js};}`;
      code += `${label}:for(let ${first}=1;;${first}=0){`;
      if (step) code += `if(!${first}){${step.pre}${step.js};}`;
      if (cond) code += `{${cond.pre}if((${cond.js})===0)break;}`;
      code += `${body}${status}=${cv}.status;}${cv}.status=${status};}`;
      return code;
    });
  }

  caseCommand(node, cv, chk) {
    const pre = [];
    const word = this.stringExpr(node.word, cv, pre, { tilde: true });
    const v = this.id('w');
    const m = this.id('m');
    const label = this.id('C');
    let code = `{${pre.join('')}const ${v}=${word};let ${m}=false;${cv}.status=0;${label}:{`;
    for (const item of node.items) {
      let condCode = '';
      for (const pattern of item.patterns) {
        const p = [];
        const expr = this.patternExpr(pattern, cv, p);
        condCode += `if(!${m}){${p.join('')}if(R.match(${cv},${v},${expr}))${m}=true;}`;
      }
      const body = this.list(item.body, cv, chk);
      let tail = '';
      if (item.term === ';;') tail = `break ${label};`;
      else if (item.term === ';&') tail = `${m}=true;`;
      else tail = `${m}=false;`;
      code += `${condCode}if(${m}){${m}=false;${body}${tail}}`;
    }
    code += '}}';
    return code;
  }

  cond(expr, cv, v) {
    switch (expr.t) {
      case 'and':
        return `${this.cond(expr.l, cv, v)}if(${v}){${this.cond(expr.r, cv, v)}}`;
      case 'or':
        return `${this.cond(expr.l, cv, v)}if(!${v}){${this.cond(expr.r, cv, v)}}`;
      case 'not':
        return `${this.cond(expr.e, cv, v)}${v}=!${v};`;
      case 'unary': {
        const pre = [];
        const operand = this.stringExpr(expr.w, cv, pre, { tilde: true });
        return `{${pre.join('')}${v}=R.t1(${cv},${S(expr.op)},${operand});}`;
      }
      case 'binary': {
        const pre = [];
        const left = this.stringExpr(expr.l, cv, pre, { tilde: true });
        let code;
        if (expr.op === '==' || expr.op === '=' || expr.op === '!=') {
          const right = this.patternExpr(expr.r, cv, pre);
          code = `${v}=${expr.op === '!=' ? '!' : ''}R.cmatch(${cv},${left},${right});`;
        } else if (expr.op === '=~') {
          const t = this.id('t');
          pre.push(`const ${t}=[];${this.pushParts(t, expr.r, cv, {})}`);
          code = `${v}=R.rematch(${cv},${left},R.Re(${cv},${t}));`;
        } else {
          const right = this.stringExpr(expr.r, cv, pre, { tilde: true });
          code = `${v}=R.t2(${cv},${S(expr.op)},${left},${right},true);`;
        }
        return `{${pre.join('')}${code}}`;
      }
    }
    throw new Error(`unknown conditional ${expr.t}`);
  }
}

export function compileProgram(ast) {
  const compiler = new Compiler();
  let body = '';
  let group = [];
  let line = -1;
  const flush = () => {
    if (!group.length) return;
    const text = group.map(item => item.text || '').join('; ');
    body += `if(c0.opts.n)return;if(c0.opts.v)await R.verbose(c0,${S(text)});if(c0.root.aliases!==null)R.line(c0);try{${compiler.list({ t: 'list', items: group }, 'c0', true)}}catch(e){R.fatal(c0,e);}`;
    group = [];
  };
  for (const item of ast.items) {
    if (item.line !== line) flush();
    line = item.line;
    group.push(item);
  }
  flush();
  return `const A=R.A;${body}`;
}
