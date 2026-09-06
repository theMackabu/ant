export type Value = Expr | string | number | boolean | null | readonly Value[] | Attrs;
export interface Attrs { readonly [key: string]: Value }
interface Symbol { readonly hint: string }
type Name = string | Symbol;
type Node =
  | { kind: 'raw'; source: string }
  | { kind: 'literal'; value: string | number | boolean | null }
  | { kind: 'ref'; name: Name }
  | { kind: 'path'; value: string }
  | { kind: 'attrs'; entries: readonly [string | Node, Node][] }
  | { kind: 'list'; items: Node[] }
  | { kind: 'select'; base: Node; names: string[]; fallback?: Node }
  | { kind: 'call'; fn: Node; args: Node[] }
  | { kind: 'lambda'; args: string | readonly string[]; names: Name[]; defaults: Attrs; body: Node }
  | { kind: 'let'; bindings: readonly [Name, Node][]; body: Node }
  | { kind: 'if'; condition: Node; yes: Node; no: Node }
  | { kind: 'binary'; op: '//' | '++' | '=='; left: Node; right: Node }
  | { kind: 'template'; string: boolean; parts: readonly string[]; values: Node[] };

export class Expr {
  readonly node: Node;
  constructor(value: string | Expr) {
    if (typeof value === 'string') this.node = { kind: 'raw', source: value };
    else this.node = value.node;
  }
  static value(value: Value): Expr { return expression(toNode(value)); }
  get source(): string { return render(this).trimEnd(); }
  render(): string { return render(this); }
  get(...names: string[]): Expr { return select(this, ...names); }
  getOr(fallback: Value, ...names: string[]): Expr {
    if (!names.length) throw new TypeError('getOr requires an attribute name');
    return expression({ kind: 'select', base: this.node, names, fallback: toNode(fallback) });
  }
  set(name: string | Expr, value: Value): Expr {
    return this.merge(expression({ kind: 'attrs', entries: [[typeof name === 'string' ? name : name.node, toNode(value)]] }));
  }
  call(...args: Value[]): Expr { return call(this, ...args); }
  merge(value: Value): Expr { return binary('//', this, value); }
  concat(value: Value): Expr { return binary('++', this, value); }
  equals(value: Value): Expr { return binary('==', this, value); }
}

function expression(node: Node): Expr {
  const result = Object.create(Expr.prototype);
  Object.defineProperty(result, 'node', { value: node, enumerable: true });
  return result;
}
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
function toNode(value: Value, seen = new Set<object>()): Node {
  if (value instanceof Expr) return value.node;
  if (value === null || typeof value === 'boolean') return { kind: 'literal', value };
  if (typeof value === 'string') {
    quote(value);
    return { kind: 'literal', value };
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) {
      throw new TypeError('Nix numbers must be finite and integers must be safe');
    }
    return { kind: 'literal', value };
  }
  if (typeof value !== 'object') throw new TypeError(`Unsupported Nix value: ${typeof value}`);
  if (seen.has(value)) throw new TypeError('Cyclic Nix value');
  seen.add(value);
  try {
    if (Array.isArray(value)) return { kind: 'list', items: Array.from(value, item => toNode(item, seen)) };
    const proto = Object.getPrototypeOf(value);
    if (proto !== Object.prototype && proto !== null) throw new TypeError('Expected a plain attribute object');
    if (Object.getOwnPropertySymbols(value).length) throw new TypeError('Symbol attribute keys are unsupported');
    return { kind: 'attrs', entries: Object.entries(value).map(([name, item]) => [name, toNode(item, seen)]) };
  } finally {
    seen.delete(value);
  }
}

// These internal constructors preserve binding identity until the whole expression is rendered.
export function symbol(hint: string): Expr {
  binding(hint);
  if (hint.startsWith('__tsnix_')) throw new TypeError('The __tsnix_ prefix is reserved for generated bindings');
  return expression({ kind: 'ref', name: { hint } });
}
function symbolName(handle: Expr): Name {
  if (handle.node.kind !== 'ref') throw new TypeError('Expected a binding handle');
  return handle.node.name;
}
export function boundLet(bindings: readonly [Expr, Value][], body: Value): Expr {
  if (!bindings.length) return Expr.value(body);
  return expression({ kind: 'let', bindings: bindings.map(([name, value]) => [symbolName(name), toNode(value)]), body: toNode(body) });
}
export function boundFunction(args: string | readonly string[], handles: Expr[], body: Value, defaults: Attrs = {}): Expr {
  const names = typeof args === 'string' ? [args] : args;
  names.forEach(binding);
  if (new Set(names).size !== names.length) throw new TypeError('Duplicate function argument');
  for (const name of Object.keys(defaults)) {
    if (typeof args === 'string' || !args.includes(name)) throw new TypeError(`Unknown default argument: ${name}`);
    toNode(defaults[name]);
  }
  return expression({ kind: 'lambda', args, names: handles.map(symbolName), defaults: { ...defaults }, body: toNode(body) });
}

function children(node: Node): Node[] {
  switch (node.kind) {
    case 'attrs': return node.entries.flatMap(([name, value]) => typeof name === 'string' ? [value] : [name, value]);
    case 'list': return node.items;
    case 'select': return [node.base, ...(node.fallback ? [node.fallback] : [])];
    case 'call': return [node.fn, ...node.args];
    case 'lambda': return [...Object.values(node.defaults).map(value => toNode(value)), node.body];
    case 'let': return [...node.bindings.map(([, value]) => value), node.body];
    case 'if': return [node.condition, node.yes, node.no];
    case 'binary': return [node.left, node.right];
    case 'template': return node.values;
    default: return [];
  }
}
function bindingNames(root: Node): Map<Symbol, string> {
  const symbols = new Set<Symbol>();
  const reserved = new Set<string>();
  const explicit = new Set<string>();
  const visited = new Set<Node>();
  function collect(name: Name) {
    if (typeof name === 'string') { reserved.add(name); explicit.add(name); }
    else { symbols.add(name); reserved.add(name.hint); }
  }
  function visit(node: Node) {
    if (visited.has(node)) return;
    visited.add(node);
    if (node.kind === 'ref') collect(node.name);
    if (node.kind === 'lambda') {
      node.names.forEach(collect);
      if (typeof node.args !== 'string') node.args.forEach(name => reserved.add(name));
    }
    if (node.kind === 'let') node.bindings.forEach(([name]) => collect(name));
    children(node).forEach(visit);
  }
  visit(root);
  const counts = new Map<string, number>();
  for (const item of symbols) counts.set(item.hint, (counts.get(item.hint) ?? 0) + 1);
  const result = new Map<Symbol, string>();
  for (const item of symbols) {
    let name = item.hint;
    if (counts.get(name)! > 1 || explicit.has(name)) {
      let suffix = 1;
      while (reserved.has(`${name}_${suffix}`)) suffix++;
      name = `${name}_${suffix}`;
    }
    reserved.add(name);
    result.set(item, name);
  }
  return result;
}

const indent = (depth: number) => '  '.repeat(depth);
const operatorPrecedence = { '//': 40, '++': 70, '==': 25 };
function precedence(node: Node): number {
  switch (node.kind) {
    case 'raw': return 0;
    case 'lambda': case 'let': case 'if': return 0;
    case 'template': return node.string ? 110 : 0;
    case 'binary': return operatorPrecedence[node.op];
    case 'call': return 90;
    case 'select': return node.fallback ? 0 : 100;
    case 'literal': return typeof node.value === 'number' && node.value < 0 ? 80 : 110;
    default: return 110;
  }
}

export function render(value: Value): string {
  const root = toNode(value);
  const names = bindingNames(root);
  const nameOf = (name: Name) => typeof name === 'string' ? name : names.get(name)!;

  function string(parts: readonly string[], values: Node[], depth: number): string {
    const preview = parts.join('${value}');
    const multiline = preview.includes('\n') && preview.endsWith('\n')
      && /(^|\n)[^\s]/.test(preview) && !/(^|\n)[ \t]+\n/.test(preview);
    if (!multiline) {
      return '"' + parts.map((part, i) => (i ? '${' + print(values[i - 1], depth) + '}' : '') + quote(part).slice(1, -1)).join('') + '"';
    }
    const escape = (part: string) => part.replace(/''|\$\{|\r|\t/g, token => {
      if (token === "''") return "'''";
      if (token === '${') return "''${";
      return token === '\r' ? "''\\r" : "''\\t";
    });
    let content = "''\n" + (parts[0].startsWith("\n") ? "" : indent(depth + 1));
    parts.forEach((part, i) => {
      if (i) content += '${' + print(values[i - 1], depth + 1) + '}';
      content += escape(part).replace(/\n(?!\n)/g, '\n' + indent(depth + 1));
    });
    return content.slice(0, -indent(depth + 1).length) + indent(depth) + "''";
  }
  function entry(name: string, value: Node, depth: number): string {
    const text = print(value, depth);
    if ((value.kind === 'if' && text.includes('\n')) || indent(depth).length + name.length + 3 + text.split('\n')[0].length > 100) {
      return `${indent(depth)}${name} =\n${indent(depth + 1)}${print(value, depth + 1)};`;
    }
    return `${indent(depth)}${name} = ${text};`;
  }
  function print(node: Node, depth: number, minimum = 0): string {
    let text: string;
    switch (node.kind) {
      case 'raw': text = node.source; break;
      case 'ref': text = nameOf(node.name); break;
      case 'path': text = node.value; break;
      case 'literal':
        text = typeof node.value === 'string' ? string([node.value], [], depth) : String(node.value).replace(/^(\-?\d+)e/, '$1.0e');
        break;
      case 'attrs':
        text = node.entries.length ? '{\n' + node.entries.map(([name, value]) =>
          typeof name === 'string' && value.kind === 'ref' && name === nameOf(value.name)
            ? `${indent(depth + 1)}inherit ${key(name)};`
            : entry(typeof name === 'string' ? key(name) :
            (name.kind === 'template' && name.string) || (name.kind === 'literal' && typeof name.value === 'string')
              ? print(name, depth + 1) : '${' + print(name, depth + 1) + '}', value, depth + 1)
        ).join('\n') + '\n' + indent(depth) + '}' : '{}';
        break;
      case 'list': {
        const items = node.items.map(item => print(item, depth + 1, 91));
        const compact = items.length ? '[ ' + items.join(' ') + ' ]' : '[ ]';
        text = !compact.includes('\n') && compact.length + indent(depth).length <= 90 ? compact :
          '[\n' + items.map(item => indent(depth + 1) + item).join('\n') + '\n' + indent(depth) + ']';
        break;
      }
      case 'select':
        text = print(node.base, depth, 100) + '.' + node.names.map(key).join('.');
        if (node.fallback) text += ' or ' + print(node.fallback, depth, 91);
        break;
      case 'call':
        text = [print(node.fn, depth, 90), ...node.args.map(arg => print(arg, depth, 91))].join(' ');
        if (text.split('\n')[0].length + indent(depth).length > 100) {
          text = print(node.fn, depth, 90) + '\n' + node.args.map(arg => indent(depth + 1) + print(arg, depth + 1, 91)).join('\n');
        }
        break;
      case 'lambda': {
        const args = typeof node.args === 'string' ? [nameOf(node.names[0])] : node.args.map(arg =>
          arg + (Object.hasOwn(node.defaults, arg) ? ' ? ' + print(toNode(node.defaults[arg]), depth + 1) : '')
        );
        const compact = '{ ' + args.join(', ') + ' }';
        const pattern = typeof node.args === 'string' ? args[0] :
          compact.length + indent(depth).length <= 90 && !compact.includes('\n') ? compact :
          '{\n' + args.map(arg => indent(depth + 1) + arg + ',').join('\n') + '\n' + indent(depth) + '}';
        const aliases: [Name, Node][] = [];
        if (typeof node.args !== 'string') node.args.forEach((arg, i) => {
          if (nameOf(node.names[i]) !== arg) aliases.push([node.names[i], { kind: 'ref', name: arg }]);
        });
        let bodyNode: Node = node.body;
        if (aliases.length) bodyNode = { kind: 'let', bindings: aliases, body: node.body };
        const body = print(bodyNode, depth);
        text = pattern + ':' + (bodyNode.kind === 'attrs' || (!body.includes('\n') && pattern.length + body.length + indent(depth).length < 90) ? ' ' : '\n' + indent(depth)) + body;
        break;
      }
      case 'let':
        text = 'let\n' + node.bindings.map(([name, value]) => entry(nameOf(name), value, depth + 1)).join('\n\n') +
          '\nin\n' + indent(depth) + print(node.body, depth);
        break;
      case 'if': {
        const condition = print(node.condition, depth);
        const yes = print(node.yes, depth);
        const no = print(node.no, depth);
        text = `if ${condition} then ${yes} else ${no}`;
        if (text.includes('\n') || text.length + indent(depth).length > 100) {
          text = 'if ' + condition + '\n' + indent(depth) + 'then ' + yes + '\n' + indent(depth) + 'else ' + no;
        }
        break;
      }
      case 'binary': {
        const prec = operatorPrecedence[node.op];
        const associative = node.left.kind === 'binary' && node.left.op === node.op && node.op !== '==';
        const left = print(node.left, depth, prec + (associative ? 0 : 1));
        const right = print(node.right, depth, prec + (node.op === '==' ? 1 : 0));
        text = left + ' ' + node.op + ' ' + right;
        break;
      }
      case 'template':
        text = node.string ? string(node.parts, node.values, depth) : node.parts.map((part, i) =>
          (i ? print(node.values[i - 1], depth, 91) : '') + part
        ).join('');
        break;
    }
    return precedence(node) < minimum ? '(' + text + ')' : text;
  }
  return print(root, 0) + '\n';
}

function binary(op: '//' | '++' | '==', left: Value, right: Value): Expr {
  return expression({ kind: 'binary', op, left: toNode(left), right: toNode(right) });
}
export function nix(parts: TemplateStringsArray, ...values: Value[]): Expr {
  return expression({ kind: 'template', string: false, parts: [...parts], values: values.map(value => toNode(value)) });
}
export function str(parts: TemplateStringsArray, ...values: (string | Expr)[]): Expr {
  parts.forEach(quote);
  return expression({ kind: 'template', string: true, parts: [...parts], values: values.map(value => toNode(value)) });
}
export function ref(name: string): Expr { return expression({ kind: 'ref', name: binding(name) }); }
export function path(value: string): Expr {
  if (!/^(?:\.\/|\.\.\/|\/)[a-zA-Z0-9._+\-/]+$/.test(value) || value.endsWith('/') || value.includes('//')) {
    throw new TypeError(`Unsupported literal Nix path: ${value}`);
  }
  return expression({ kind: 'path', value });
}
export function select(value: Value, ...names: string[]): Expr {
  if (!names.length) throw new TypeError('select requires an attribute name');
  return expression({ kind: 'select', base: toNode(value), names });
}
export function call(fn: Value, ...args: Value[]): Expr {
  if (!args.length) throw new TypeError('call requires an argument');
  return expression({ kind: 'call', fn: toNode(fn), args: args.map(value => toNode(value)) });
}
export function lambda(args: string | readonly string[], body: Value, defaults: Attrs = {}): Expr {
  return boundFunction(args, (typeof args === 'string' ? [args] : args).map(ref), body, defaults);
}
export function letIn(bindings: Attrs, body: Value): Expr {
  return boundLet(Object.entries(bindings).map(([name, value]) => [ref(name), value]), body);
}
export function ifElse(condition: Value, yes: Value, no: Value): Expr {
  return expression({ kind: 'if', condition: toNode(condition), yes: toNode(yes), no: toNode(no) });
}
