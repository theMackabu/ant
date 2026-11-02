// @ant: compact_on_build
import fs from 'node:fs';

const CLASSES = {
  alpha: '\\p{L}',
  digit: '0-9',
  alnum: '\\p{L}\\p{N}',
  upper: '\\p{Lu}',
  lower: '\\p{Ll}',
  space: ' \\t\\n\\r\\f\\v',
  blank: ' \\t',
  punct: '!-\\/:-@\\[-`{-~',
  print: '\\P{C}',
  graph: '!-~\\p{L}\\p{N}\\p{P}\\p{S}',
  cntrl: '\\x00-\\x1f\\x7f',
  xdigit: '0-9A-Fa-f',
  word: '\\p{L}\\p{N}_'
};

const REGEX_SYNTAX = new Set(['^', '$', '\\', '.', '*', '+', '?', '(', ')', '[', ']', '{', '}', '|', '/']);

function escapeRegexChar(ch) {
  return REGEX_SYNTAX.has(ch) ? '\\' + ch : ch;
}

function escapeClassChar(ch) {
  return ch === '\\' || ch === ']' || ch === '[' || ch === '^' || ch === '-' ? '\\' + ch : ch;
}

export function escapeGlob(text) {
  let out = '';
  for (const ch of text) {
    if (ch === '*' || ch === '?' || ch === '[' || ch === ']' || ch === '\\' || ch === '(' || ch === ')' || ch === '|' || ch === '!' || ch === '@' || ch === '+') out += '\\';
    out += ch;
  }
  return out;
}

export function unescapeGlob(pattern) {
  let out = '';
  for (let i = 0; i < pattern.length; i++) {
    if (pattern[i] === '\\' && i + 1 < pattern.length) i++;
    out += pattern[i];
  }
  return out;
}

export function hasGlob(pattern, extglob = true) {
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '\\') {
      i++;
      continue;
    }
    if (c === '*' || c === '?') return true;
    if (c === '[' && pattern.indexOf(']', i + 2) !== -1) return true;
    if (extglob && (c === '+' || c === '@' || c === '!') && pattern[i + 1] === '(') return true;
  }
  return false;
}

function parseBracket(pattern, start) {
  let i = start + 1;
  let negate = false;
  if (pattern[i] === '!' || pattern[i] === '^') {
    negate = true;
    i++;
  }
  let body = '';
  let first = true;
  for (; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === ']' && !first) return { end: i, source: body ? `[${negate ? '^' : ''}${body}]` : negate ? '[^]' : '[]' };
    first = false;
    if (c === '[' && pattern[i + 1] === ':') {
      const close = pattern.indexOf(':]', i + 2);
      if (close !== -1) {
        const name = pattern.slice(i + 2, close);
        if (CLASSES[name] !== undefined) {
          body += CLASSES[name];
          i = close + 1;
          continue;
        }
      }
    }
    if (c === '[' && (pattern[i + 1] === '=' || pattern[i + 1] === '.')) {
      const close = pattern.indexOf(pattern[i + 1] + ']', i + 2);
      if (close !== -1) {
        for (const ch of pattern.slice(i + 2, close)) body += escapeClassChar(ch);
        i = close + 1;
        continue;
      }
    }
    let ch = c;
    if (c === '\\' && i + 1 < pattern.length) ch = pattern[++i];
    if (pattern[i + 1] === '-' && pattern[i + 2] !== undefined && pattern[i + 2] !== ']') {
      let to = pattern[i + 2];
      let skip = 2;
      if (to === '\\' && i + 3 < pattern.length) {
        to = pattern[i + 3];
        skip = 3;
      }
      if (to.codePointAt(0) >= ch.codePointAt(0)) body += `${escapeClassChar(ch)}-${escapeClassChar(to)}`;
      i += skip;
      continue;
    }
    body += escapeClassChar(ch);
  }
  return null;
}

function splitAlternatives(text) {
  const out = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '\\') {
      i++;
      continue;
    }
    if (c === '(') depth++;
    else if (c === ')') depth--;
    else if (c === '|' && depth === 0) {
      out.push(text.slice(start, i));
      start = i + 1;
    }
  }
  out.push(text.slice(start));
  return out;
}

function findClose(pattern, open) {
  let depth = 0;
  for (let i = open; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '\\') {
      i++;
      continue;
    }
    if (c === '(') depth++;
    else if (c === ')' && --depth === 0) return i;
  }
  return -1;
}

export function patternSource(pattern, extglob = true) {
  let out = '';
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '\\') {
      if (i + 1 < pattern.length) out += escapeRegexChar(pattern[++i]);
      else out += '\\\\';
      continue;
    }
    if (extglob && (c === '?' || c === '*' || c === '+' || c === '@' || c === '!') && pattern[i + 1] === '(') {
      const close = findClose(pattern, i + 1);
      if (close !== -1) {
        const alts = splitAlternatives(pattern.slice(i + 2, close)).map(alt => patternSource(alt, extglob)).join('|');
        const rest = patternSource(pattern.slice(close + 1), extglob);
        switch (c) {
          case '?': out += `(?:${alts})?`; break;
          case '*': out += `(?:${alts})*`; break;
          case '+': out += `(?:${alts})+`; break;
          case '@': out += `(?:${alts})`; break;
          case '!': out += `(?:(?!(?:${alts})${rest ? `(?:${rest})` : ''}$)[^]*?)`; break;
        }
        return out + rest;
      }
    }
    if (c === '*') {
      while (pattern[i + 1] === '*') i++;
      out += '[^]*';
      continue;
    }
    if (c === '?') {
      out += '[^]';
      continue;
    }
    if (c === '[') {
      const bracket = parseBracket(pattern, i);
      if (bracket) {
        out += bracket.source;
        i = bracket.end;
        continue;
      }
    }
    out += escapeRegexChar(c);
  }
  return out;
}

const cache = new Map();

export function compilePattern(pattern, nocase = false, extglob = true) {
  const key = (nocase ? 'i' : 'c') + (extglob ? 'x' : 'n') + pattern;
  let re = cache.get(key);
  if (re) return re;
  try {
    re = new RegExp(`^(?:${patternSource(pattern, extglob)})$`, nocase ? 'iu' : 'u');
  } catch {
    re = new RegExp(`^${escapeForRegex(unescapeGlob(pattern))}$`, nocase ? 'iu' : 'u');
  }
  if (cache.size > 2048) cache.clear();
  cache.set(key, re);
  return re;
}

function escapeForRegex(text) {
  let out = '';
  for (const ch of text) out += escapeRegexChar(ch);
  return out;
}

export function matchPattern(text, pattern, nocase = false, extglob = true) {
  if (!hasGlob(pattern, extglob)) {
    const literal = unescapeGlob(pattern);
    return nocase ? literal.toLowerCase() === text.toLowerCase() : literal === text;
  }
  return compilePattern(pattern, nocase, extglob).test(text);
}

function splitComponents(pattern) {
  const parts = [];
  let current = '';
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '\\' && i + 1 < pattern.length) {
      current += c + pattern[++i];
      continue;
    }
    if (c === '/') {
      parts.push(current);
      current = '';
      continue;
    }
    current += c;
  }
  parts.push(current);
  return parts;
}

function joinPath(base, name) {
  if (base === '') return name;
  if (base.endsWith('/')) return base + name;
  return base + '/' + name;
}

function isDirectory(path) {
  try {
    return fs.statSync(path).isDirectory();
  } catch {
    return false;
  }
}

function exists(path) {
  try {
    fs.lstatSync(path);
    return true;
  } catch {
    return false;
  }
}

function listDir(path) {
  try {
    return fs.readdirSync(path);
  } catch {
    return [];
  }
}

function walkAll(absBase, relBase, out, dotglob) {
  for (const name of listDir(absBase)) {
    if (name.startsWith('.') && !dotglob) continue;
    const abs = joinPath(absBase, name);
    const rel = joinPath(relBase, name);
    if (isDirectory(abs) && !fs.lstatSync(abs).isSymbolicLink()) {
      out.push({ abs, rel });
      walkAll(abs, rel, out, dotglob);
    }
  }
}

export function globExpand(cwd, pattern, options) {
  const { dotglob = false, nocase = false, extglob = true, globstar = false } = options;
  const components = splitComponents(pattern);
  let candidates;
  if (components[0] === '' && components.length > 1) {
    candidates = [{ abs: '/', rel: '/' }];
    components.shift();
  } else {
    candidates = [{ abs: cwd, rel: '' }];
  }
  for (let index = 0; index < components.length; index++) {
    const component = components[index];
    const last = index === components.length - 1;
    const next = [];
    if (component === '') {
      if (last) {
        for (const c of candidates) if (isDirectory(c.abs)) next.push({ abs: c.abs, rel: c.rel.endsWith('/') ? c.rel : c.rel + '/' });
      } else {
        for (const c of candidates) next.push(c);
      }
      candidates = next;
      continue;
    }
    if (!hasGlob(component, extglob)) {
      const literal = unescapeGlob(component);
      for (const c of candidates) {
        const abs = c.abs === '/' ? '/' + literal : joinPath(c.abs, literal);
        if (last ? exists(abs) : isDirectory(abs)) next.push({ abs, rel: c.rel === '/' ? '/' + literal : joinPath(c.rel, literal) });
      }
      candidates = next;
      continue;
    }
    if (globstar && component === '**') {
      for (const c of candidates) {
        const found = [c];
        walkAll(c.abs, c.rel, found, dotglob);
        if (last) {
          for (const f of found) {
            if (f !== c) next.push(f);
            for (const name of listDir(f.abs)) {
              if (name.startsWith('.') && !dotglob) continue;
              const abs = joinPath(f.abs, name);
              if (!isDirectory(abs)) next.push({ abs, rel: joinPath(f.rel, name) });
            }
          }
        } else next.push(...found);
      }
      candidates = next;
      continue;
    }
    const re = compilePattern(component, nocase, extglob);
    const allowDot = component.startsWith('.') || component.startsWith('\\.');
    for (const c of candidates) {
      const names = listDir(c.abs).sort();
      for (const name of names) {
        if (name.startsWith('.') && !allowDot && !dotglob) continue;
        if (!re.test(name)) continue;
        const abs = joinPath(c.abs, name);
        if (!last && !isDirectory(abs)) continue;
        next.push({ abs, rel: c.rel === '/' ? '/' + name : joinPath(c.rel, name) });
      }
    }
    candidates = next;
    if (!candidates.length) break;
  }
  const seen = new Set();
  const results = [];
  for (const c of candidates) {
    if (seen.has(c.rel)) continue;
    seen.add(c.rel);
    results.push(c.rel);
  }
  results.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  return results;
}

export function ereToRegExp(source) {
  let out = '';
  for (let i = 0; i < source.length; i++) {
    const c = source[i];
    if (c === '\\' && i + 1 < source.length) {
      const n = source[i + 1];
      if (n === '<' || n === '>') out += '\\b';
      else out += c + n;
      i++;
      continue;
    }
    if (c === '[') {
      let j = i + 1;
      let body = '[';
      if (source[j] === '^') {
        body += '^';
        j++;
      }
      if (source[j] === ']') {
        body += '\\]';
        j++;
      }
      for (; j < source.length && source[j] !== ']'; j++) {
        if (source[j] === '[' && source[j + 1] === ':') {
          const close = source.indexOf(':]', j + 2);
          const name = close !== -1 ? source.slice(j + 2, close) : '';
          if (CLASSES[name] !== undefined) {
            body += CLASSES[name];
            j = close + 1;
            continue;
          }
        }
        if (source[j] === '\\') body += '\\\\';
        else if (source[j] === '[') body += '\\[';
        else body += source[j];
      }
      if (j >= source.length) {
        out += '\\[';
        continue;
      }
      out += body + ']';
      i = j;
      continue;
    }
    out += c;
  }
  return new RegExp(out, 'u');
}
