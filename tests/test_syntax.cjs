const { stripTypes, parseJavaScript } = require('ant:syntax');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function assertThrows(fn, pattern, message) {
  try {
    fn();
  } catch (error) {
    assert(pattern.test(String(error)), `${message}: unexpected error ${error}`);
    return;
  }
  throw new Error(`${message}: expected an exception`);
}

assert((true ? 0.5 : 0) === 0.5, 'conditional decimal consequent parses without whitespace');
assert((false ? 0.5 : 0) === 0, 'conditional decimal alternate remains intact');
assert({ value: 1 }?.value === 1, 'ordinary optional chaining still parses');

function collectTypes(node, out = new Set()) {
  if (!node || typeof node !== 'object') return out;
  if (typeof node.type === 'string') out.add(node.type);
  if (Array.isArray(node)) {
    for (const value of node) collectTypes(value, out);
  } else {
    for (const key of Object.keys(node)) collectTypes(node[key], out);
  }
  return out;
}

const stripped = stripTypes(
  `
enum Color { Red, Blue }
interface Hidden { value: number }
const color: Color = Color.Red;
`,
  { filename: 'colors.ts', sourceType: 'module' }
);

assert(!stripped.includes('interface Hidden'), 'stripTypes removes interfaces');
assert(!stripped.includes(': Color'), 'stripTypes removes annotations');
assert(stripped.includes('var Color'), 'stripTypes lowers enums');
assert(stripped.includes('const color = Color.Red'), 'stripTypes preserves runtime code');

const commonjs = stripTypes('const value: number = 1;', {
  filename: 'value.cts',
  sourceType: 'commonjs'
});
assert(!commonjs.includes('export {};'), 'commonjs stripping does not add a module marker');

assertThrows(() => stripTypes(1), /source must be a string/, 'stripTypes validates source');
assertThrows(() => stripTypes('const x: number = 1', { sourceType: 'tsx' }), /sourceType/, 'stripTypes validates sourceType');

const source = `
import defaultValue, { named as local } from "pkg";
export const answer = 42;
export default async function work(value = 1, ...rest) {
  const object = { value, get size() { return rest.length }, ...rest[0] };
  for (const item of rest) object.value += item ?? 0;
  return \`\${object.value}\`;
}
class Box extends Base {
  static count = 0;
  #value = 1;
  get value() { return this.#value; }
  static { this.count++; }
}
`;

const tree = parseJavaScript(source, {
  filename: 'fixture.mjs',
  sourceType: 'module',
  locations: true
});

assert(tree.schema === 'ant.syntax@1', 'parseJavaScript reports schema version');
assert(tree.type === 'Program', 'parseJavaScript returns a Program');
assert(tree.sourceType === 'module', 'module source type is preserved');
assert(tree.start === 0 && tree.end === source.length, 'Program spans the full source');
assert(tree.loc.start.line === 1 && tree.loc.start.column === 0, 'Program has locations');

const types = collectTypes(tree);
for (const type of [
  'ImportDeclaration',
  'ImportDefaultSpecifier',
  'ImportSpecifier',
  'ExportNamedDeclaration',
  'ExportDefaultDeclaration',
  'FunctionDeclaration',
  'ObjectExpression',
  'Property',
  'ForOfStatement',
  'LogicalExpression',
  'TemplateLiteral',
  'ClassDeclaration',
  'PropertyDefinition',
  'MethodDefinition',
  'PrivateIdentifier',
  'StaticBlock',
  'ExpressionStatement'
])
  assert(types.has(type), `syntax tree includes ${type}`);

const privateIdentifier = (function find(node) {
  if (!node || typeof node !== 'object') return null;
  if (node.type === 'PrivateIdentifier') return node;
  if (Array.isArray(node)) {
    for (const value of node) {
      const found = find(value);
      if (found) return found;
    }
  } else {
    for (const key of Object.keys(node)) {
      const found = find(node[key]);
      if (found) return found;
    }
  }
  return null;
})(tree);
assert(privateIdentifier.name === 'value', 'private identifier names omit #');

const serialized = JSON.stringify(tree);
assert(serialized.length > 0, 'syntax tree is JSON serializable');
assert(JSON.parse(serialized).schema === 'ant.syntax@1', 'syntax tree JSON round trips');

const literalTree = parseJavaScript('const values = [123n, /a+/gi];');
const literals = [];
(function visit(node) {
  if (!node || typeof node !== 'object') return;
  if (node.type === 'Literal') literals.push(node);
  if (Array.isArray(node)) for (const value of node) visit(value);
  else for (const key of Object.keys(node)) visit(node[key]);
})(literalTree);
assert(
  literals.some(node => node.bigint === '123' && node.value === null),
  'bigints are JSON safe'
);
assert(
  literals.some(node => node.regex?.pattern === 'a+' && node.regex.flags === 'gi'),
  'regexps are JSON safe'
);
JSON.stringify(literalTree);

const unicodeSource = 'const face = "💩";\nlet next = 1;';
const unicodeTree = parseJavaScript(unicodeSource, { locations: true });
const nextDeclaration = unicodeTree.body[1].declarations[0];
assert(nextDeclaration.id.start === unicodeSource.indexOf('next'), 'offsets use UTF-16 code units');
assert(nextDeclaration.id.loc.start.line === 2, 'line locations account for newlines');
assert(nextDeclaration.id.loc.start.column === 4, 'columns use UTF-16 code units');

assert(parseJavaScript('const value = 1').sourceType === 'script', 'unambiguous defaults to script');
assert(parseJavaScript('export const value = 1').sourceType === 'module', 'unambiguous detects modules');
assertThrows(
  () => parseJavaScript('export const value = 1', { sourceType: 'script' }),
  /import\/export syntax is not allowed/,
  'script mode rejects module syntax'
);
assertThrows(
  () => parseJavaScript('with (value) {}', { sourceType: 'module' }),
  /with statement not allowed in strict mode/,
  'module mode parses strictly'
);
assertThrows(
  () => parseJavaScript('import "pkg"; with (value) {}'),
  /with statement not allowed in strict mode/,
  'unambiguous module detection reparses strictly'
);
assertThrows(() => parseJavaScript('const ='), /SyntaxError/, 'parseJavaScript preserves syntax errors');

const largeSource = Array.from({ length: 1500 }, (_, i) => `const value${i} = ${i};`).join('\n');
const largeTree = parseJavaScript(largeSource);
assert(largeTree.body.length === 1500, 'large syntax trees survive allocation and GC pressure');
JSON.stringify(largeTree);

console.log('ant:syntax tests passed');
