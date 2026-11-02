import fc from 'fast-check';
import { finish, literal } from '../prelude.mjs';
import { tierVariants } from './jit.mjs';

const WARM_RUNS = 300;

const leaves = ['n', 's', 'a', 'o', 'm', '1', '-1', '0', '-0', '1.5', 'NaN', '"x"', '""', '"1"', 'null', 'undefined', 'true', '[]', '{}', '[1, 2]', 'a[0]', 'o.x', 's[0]', '10n', 'Infinity'];
const indices = ['0', '1', '-1', '2', '5'];
const keys = ['"x"', '"y"', '"0"', '"__proto__"', '"length"', 's'];

const statementTemplates = [
  e => `a.push(${e})`, () => 'a.pop()', () => 'a.shift()', e => `a.unshift(${e})`, (e, i) => `a.splice(${i}, 1, ${e})`,
  (e, i) => `a.slice(${i})`, e => `a.indexOf(${e})`, e => `a.includes(${e})`, () => 'a.sort()', () => 'a.sort((x, y) => (x > y) - (x < y))',
  () => 'a.reverse()', e => `a.map(x => x + ${e})`, e => `a.filter(x => x > ${e})`, e => `a.reduce((x, y) => x + y, ${e})`,
  () => 'a.flat()', (e, i) => `a.at(${i})`, e => `a.with(0, ${e})`, () => 'a.toSorted()', (e, i, k) => `a.join(${k})`,
  () => '[...a, ...s]', () => 'Array.from(s)', (e, i) => `a[${i}] = ${e}`, (e, i) => `delete a[${i}]`, (e, i) => `a.length = ${i === '-1' ? '0' : i}`,
  e => `o.x = ${e}`, (e, i, k) => `o[${k}] = ${e}`, () => 'delete o.x', () => 'Object.keys(o)', () => 'Object.entries(o)',
  e => `Object.assign(o, { y: ${e} })`, e => `({ ...o, z: ${e} })`, () => 'Object.freeze(o)', () => 'o?.p?.q', e => `o.x ?? ${e}`,
  (e, i) => `s.slice(${i})`, (e, i, k) => `s.split(${k})`, (e, i, k) => `s.replace(${k}, "$&$&")`, (e, i) => `s.at(${i})`,
  (e, i) => `s.codePointAt(${i})`, e => `s = s + ${e}`, (e, i, k) => `s.padStart(${i}, ${k})`, e => `\`\${s}\${${e}}\``,
  e => `n = n + ${e}`, e => `n = ${e}`, () => 'n++', () => 'n += s.length', e => `${e} == n`, e => `${e} === a[0]`, e => `${e} < s`,
  e => `typeof ${e}`, e => `[${e}, n, s].sort()`, () => 'JSON.stringify(o)', () => 'JSON.parse(JSON.stringify(a))',
  e => `m.set(${e}, n)`, e => `m.get(${e})`, e => `m.delete(${e})`, () => 'm.size', () => '[...m.keys()]', () => 'Math.max(...a)',
  e => `String(${e})`, () => 'Number(s)', () => 'parseInt(s)', e => `(() => { throw ${e}; })()`,
  e => `(() => { try { return ${e}; } finally { n++; } })()`, e => `(() => { const [p, q = ${e}, ...r] = a; return [p, q, r]; })()`,
  e => `new (class { constructor(v) { this.v = v; } get w() { return this.v * 2; } })(${e}).w`,
  e => `[...(function* () { yield* a; yield ${e}; })()]`, (e, i, k) => `Object.defineProperty(o, ${k}, { get() { return n; }, enumerable: true, configurable: true })`,
  (e, i, k) => `new Proxy(o, { get: (target, key) => typeof key === "string" ? key + ${e} : undefined })[${k}]`, () => 'Reflect.ownKeys(o)',
  () => 'structuredClone(a)', e => `a.concat(${e})`, e => `Array.isArray(${e})`, e => `${e} in o`, e => `({ [${e}]: 1 })`,
  e => `(${e})?.toString?.()`, e => `[${e}].flatMap(x => [x, x])`, () => 'Object.groupBy(a, x => typeof x)', () => 'a.findLast(x => x)'
];

const program = fc.record({
  initial: fc.record({
    n: fc.constantFrom('0', '1', '-1', '0.5', 'NaN', '2147483647', '-0', '"7"'),
    s: fc.constantFrom('""', '"ab"', '"a,b"', '"😀x"', '"10"', '" 1 "'),
    a: fc.constantFrom('[]', '[1, 2, 3]', '["b", "a", 10, 9]', '[, 1]', '[null, undefined, NaN]', '[[1], [2, [3]]]', '[-0, 0]'),
    o: fc.constantFrom('{}', '{ x: 1 }', '{ x: "a", y: [1] }', '{ 1: "one", x: null }', 'Object.create(null)'),
    m: fc.constantFrom('new Map()', 'new Map([[1, "a"], ["1", "b"]])', 'new Map([[NaN, 1], [-0, 2]])')
  }),
  statements: fc.array(
    fc.tuple(fc.integer({ min: 0, max: statementTemplates.length - 1 }), fc.constantFrom(...leaves), fc.constantFrom(...indices), fc.constantFrom(...keys)),
    { minLength: 1, maxLength: 10 }
  )
});

const keySource = String.raw`
function __value(value, depth) {
  if (typeof value === 'number') return Object.is(value, -0) ? '$-0' : Number.isFinite(value) ? value : '$' + value;
  if (typeof value === 'bigint') return '$' + value + 'n';
  if (value === undefined) return '$u';
  if (typeof value === 'symbol') return '$symbol';
  if (typeof value === 'function') return '$function';
  if (value === null || typeof value !== 'object') return value;
  if (depth > 4) return '$deep';
  if (Array.isArray(value)) {
    const items = ['$array'];
    for (let i = 0; i < value.length; i++) items.push(i in value ? __value(value[i], depth + 1) : '$hole');
    return items;
  }
  if (value instanceof Map) return ['$map'].concat([...value].map(([k, v]) => [__value(k, depth + 1), __value(v, depth + 1)]));
  return ['$object'].concat(Object.keys(value).map(k => [k, __value(value[k], depth + 1)]));
}
function __key(value) {
  try { return typeof value + ':' + JSON.stringify(__value(value, 0)); }
  catch (error) { return typeof value + ':!' + (error && error.name); }
}`;

function programSource({ initial, statements }, index) {
  const steps = statements
    .map(([template, e, i, k]) => `  try { log.push(__key(${statementTemplates[template](e, i, k)})); } catch (error) { log.push('throws ' + (error && error.name)); }`)
    .join('\n');
  return `function program${index}() {
  const log = [];
  let n = ${initial.n}, s = ${initial.s}, a = ${initial.a}, o = ${initial.o}, m = ${initial.m};
${steps}
  log.push(__key([n, s, a, o, [...m]]));
  return log;
}
{
  const cold = program${index}();
  for (let i = 0; i < ${WARM_RUNS}; i++) program${index}();
  const hot = program${index}();
  __record(${literal(`cold:${index}`)}, () => cold);
  __record(${literal(`hot:${index}`)}, () => hot);
}`;
}

export const jsExec = {
  name: 'js-exec',
  variants: tierVariants,
  arbitrary: fc.record({ programs: fc.array(program, { minLength: 1, maxLength: 3, size: 'max' }) }),
  build({ programs }) {
    return finish(`${keySource}\n${programs.map(programSource).join('\n')}`);
  }
};
