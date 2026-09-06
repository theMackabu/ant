export class Expr {
  readonly source: string;
  constructor(source: string) { this.source = source; }
  render(): string { return render(this); }
  get(...names: string[]): Expr { return select(this, ...names); }
  getOr(fallback: Value, ...names: string[]): Expr {
    return new Expr('(' + source(select(this, ...names)) + ' or (' + source(fallback) + '))');
  }
  call(...args: Value[]): Expr { return call(this, ...args); }
  merge(value: Value): Expr { return new Expr('((' + this.source + ') // (' + source(value) + '))'); }
  concat(value: Value): Expr { return new Expr('((' + this.source + ') ++ (' + source(value) + '))'); }
  equals(value: Value): Expr { return new Expr('((' + this.source + ') == (' + source(value) + '))'); }
}

export type Value = Expr | string | number | boolean | null | readonly Value[] | Attrs;
export interface Attrs { readonly [key: string]: Value }

const identifier = /^[a-zA-Z_][a-zA-Z0-9_'\-]*$/;
const keywords = new Set(['if', 'then', 'else', 'assert', 'with', 'let', 'in', 'rec', 'inherit', 'or', 'true', 'false', 'null']);
function binding(name: string): string {
  if (!identifier.test(name) || keywords.has(name)) throw new TypeError(`Invalid Nix binding: ${name}`);
  return name;
}
function quote(value: string): string {
  if (value.includes('\0')) throw new TypeError('Nix strings cannot contain NUL');
  return '"' + value.replace(/\\/g, '\\\\').replace(/"/g, '\\"')
    .replace(/\$\{/g, '\\${').replace(/\n/g, '\\n').replace(/\r/g, '\\r').replace(/\t/g, '\\t') + '"';
}
function key(name: string): string {
  return identifier.test(name) && !keywords.has(name) ? name : quote(name);
}
function emit(value: Value, depth: number, seen: Set<object>): string {
  if (value instanceof Expr) return value.source;
  if (value === null) return 'null';
  if (typeof value === 'string') return quote(value);
  if (typeof value === 'boolean') return String(value);
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) {
      throw new TypeError('Nix numbers must be finite and integers must be safe');
    }
    const number = String(value).replace(/^(\-?\d+)e/, '$1.0e');
    return value < 0 ? '(' + number + ')' : number;
  }
  if (typeof value !== 'object') throw new TypeError(`Unsupported Nix value: ${typeof value}`);
  if (seen.has(value)) throw new TypeError('Cyclic Nix value');
  seen.add(value);
  try {
    if (Array.isArray(value)) {
      return '[ ' + Array.from(value, item => item instanceof Expr ? '(' + emit(item, depth, seen) + ')' : emit(item, depth, seen)).join(' ') + ' ]';
    }
    const proto = Object.getPrototypeOf(value);
    if (proto !== Object.prototype && proto !== null) throw new TypeError('Expected a plain attribute object');
    if (Object.getOwnPropertySymbols(value).length) throw new TypeError('Symbol attribute keys are unsupported');
    const entries = Object.entries(value);
    if (!entries.length) return '{}';
    const indent = '  '.repeat(depth + 1);
    return '{\n' + entries.map(([name, item]) => `${indent}${key(name)} = ${emit(item, depth + 1, seen)};`).join('\n')
      + '\n' + '  '.repeat(depth) + '}';
  } finally {
    seen.delete(value);
  }
}

export function render(value: Value): string { return emit(value, 0, new Set()) + '\n'; }
function source(value: Value): string { return emit(value, 0, new Set()); }

export function nix(parts: TemplateStringsArray, ...values: Value[]): Expr {
  return new Expr(parts.reduce((out, part, i) => out + (i ? '(' + source(values[i - 1]) + ')' : '') + part, ''));
}
export function ref(name: string): Expr { return new Expr(binding(name)); }
export function select(value: Value, ...names: string[]): Expr {
  if (!names.length) throw new TypeError('select requires an attribute name');
  return new Expr('(' + source(value) + ').' + names.map(key).join('.'));
}
export function path(value: string): Expr {
  if (!/^(?:\.\/|\.\.\/|\/)[a-zA-Z0-9._+\-/]+$/.test(value) || value.endsWith('/') || value.includes('//')) {
    throw new TypeError(`Unsupported literal Nix path: ${value}`);
  }
  return new Expr(value);
}
export function call(fn: Value, ...args: Value[]): Expr {
  if (!args.length) throw new TypeError('call requires an argument');
  return new Expr('(' + [fn, ...args].map(value => '(' + source(value) + ')').join(' ') + ')');
}
export function lambda(args: string | readonly string[], body: Value): Expr {
  if (Array.isArray(args) && new Set(args).size !== args.length) throw new TypeError('Duplicate function argument');
  const pattern = typeof args === 'string' ? binding(args) : '{ ' + args.map(binding).join(', ') + ' }';
  return new Expr('(' + pattern + ': ' + source(body) + ')');
}
export function letIn(bindings: Attrs, body: Value): Expr {
  const entries = Object.entries(bindings).map(([name, value]) => `  ${binding(name)} = ${source(value)};`);
  return new Expr('(let\n' + entries.join('\n') + '\nin ' + source(body) + ')');
}
export function ifElse(condition: Value, yes: Value, no: Value): Expr {
  return new Expr(`(if (${source(condition)}) then (${source(yes)}) else (${source(no)}))`);
}
