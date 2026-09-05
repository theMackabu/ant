function mulberry32(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) | 0;
    let value = Math.imul(state ^ (state >>> 15), 1 | state);
    value ^= value + Math.imul(value ^ (value >>> 7), 61 | value);
    return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
  };
}

function pick(random, values) {
  return values[Math.floor(random() * values.length)];
}

const literal = JSON.stringify;

const prelude = String.raw`
const __out = [];
function __canon(value, seen = new Set()) {
  if (value === undefined) return { $type: 'undefined' };
  if (typeof value === 'number') {
    if (Number.isNaN(value)) return { $type: 'number', value: 'NaN' };
    if (value === Infinity) return { $type: 'number', value: 'Infinity' };
    if (value === -Infinity) return { $type: 'number', value: '-Infinity' };
    if (Object.is(value, -0)) return { $type: 'number', value: '-0' };
    return value;
  }
  if (typeof value === 'bigint') return { $type: 'bigint', value: String(value) };
  if (typeof value === 'symbol') return { $type: 'symbol', value: String(value) };
  if (typeof value === 'function') return { $type: 'function', name: value.name, length: value.length };
  if (value === null || typeof value !== 'object') return value;
  if (seen.has(value)) return { $type: 'circular' };
  seen.add(value);
  if (value instanceof Error) {
    const result = { $type: 'error', name: value.name, message: value.message };
    seen.delete(value);
    return result;
  }
  const result = Array.isArray(value) ? [] : {};
  for (const key of Object.keys(value).sort()) result[key] = __canon(value[key], seen);
  seen.delete(value);
  return result;
}
function __record(label, callback) {
  try { __out.push([label, __canon(callback())]); }
  catch (error) { __out.push([label, __canon(error)]); }
}
`;

function finish(body, asynchronous = false) {
  if (asynchronous) {
    return `${prelude}\n(async () => {\n${body}\nconsole.log(JSON.stringify(__out));\n})().catch(error => { console.log(JSON.stringify([['$fatal', __canon(error)]])); process.exitCode = 1; });\n`;
  }
  return `${prelude}\n${body}\nconsole.log(JSON.stringify(__out));\n`;
}

function propertyCase(random, index) {
  const pool = ['plain', '', '0', '-0', '01', '__proto__', 'constructor', 'a\0b', 'é', '😀'];
  const count = 3 + Math.floor(random() * 5);
  const keys = [];
  while (keys.length < count) {
    const key = pick(random, pool);
    if (!keys.includes(key)) keys.push(key);
  }
  return {
    family: 'property',
    id: `property-${index}`,
    shrinkKeys: ['keys'],
    params: { keys },
    build(params) {
      const assignments = params.keys.map((key, keyIndex) => `o[${literal(key)}] = ${keyIndex + 1};`).join('\n');
      const observations = params.keys
        .map(
          key =>
            `__record(${literal(`get:${key}`)}, () => o[${literal(key)}]);\n` +
            `__record(${literal(`has:${key}`)}, () => Object.hasOwn(o, ${literal(key)}));\n` +
            `__record(${literal(`descriptor:${key}`)}, () => Object.getOwnPropertyDescriptor(o, ${literal(key)}));`
        )
        .join('\n');
      return finish(
        `const o = {};\n${assignments}\n${observations}\n__record('keys', () => Object.keys(o));\n__record('json', () => JSON.stringify(o));`
      );
    }
  };
}

const regexpScenarios = [
  { pattern: '', flags: '', input: '', replacement: '_' },
  { pattern: '(?<m>b)', flags: 'g', input: 'abc', replacement: '[$<m>]' },
  { pattern: '(a)?b', flags: 'd', input: 'b', replacement: '$1' },
  { pattern: '(?:)', flags: 'g', input: '😀', replacement: '-' },
  { pattern: 'a', flags: 'g', input: 'baac', replacement: 'x', lastIndex: '1' }
];

function regexpCase(random, index) {
  const scenarios = [];
  const count = 2 + Math.floor(random() * 4);
  while (scenarios.length < count) {
    const scenario = pick(random, regexpScenarios);
    if (!scenarios.includes(scenario)) scenarios.push(scenario);
  }
  return {
    family: 'regexp',
    id: `regexp-${index}`,
    shrinkKeys: ['scenarios'],
    params: { scenarios },
    build(params) {
      const body = params.scenarios
        .map((scenario, scenarioIndex) => {
          const setup = `const r${scenarioIndex} = new RegExp(${literal(scenario.pattern)}, ${literal(scenario.flags)});`;
          const lastIndex = scenario.lastIndex === undefined ? '' : `r${scenarioIndex}.lastIndex = ${literal(scenario.lastIndex)};`;
          return `${setup}\n${lastIndex}
__record(${literal(`source:${scenarioIndex}`)}, () => r${scenarioIndex}.source);
__record(${literal(`exec:${scenarioIndex}`)}, () => r${scenarioIndex}.exec(${literal(scenario.input)}));
__record(${literal(`lastIndex:${scenarioIndex}`)}, () => r${scenarioIndex}.lastIndex);
__record(${literal(`replace:${scenarioIndex}`)}, () => ${literal(scenario.input)}.replace(r${scenarioIndex}, ${literal(scenario.replacement)}));`;
        })
        .join('\n');
      return finish(body);
    }
  };
}

const promiseScenarios = ['basic-order', 'nested-emitter-catch', 'nested-emitter-late-catch', 'queue-microtask'];

function promiseCase(random, index) {
  const scenarios = [];
  const count = 2 + Math.floor(random() * 3);
  while (scenarios.length < count) {
    const scenario = pick(random, promiseScenarios);
    if (!scenarios.includes(scenario)) scenarios.push(scenario);
  }
  return {
    family: 'promise',
    id: `promise-${index}`,
    shrinkKeys: ['scenarios'],
    params: { scenarios },
    build(params) {
      const blocks = params.scenarios.map((scenario, scenarioIndex) => {
        const label = literal(`${scenarioIndex}:${scenario}`);
        if (scenario === 'basic-order')
          return `{ const order = []; Promise.resolve().then(() => order.push('then')); order.push('sync'); await Promise.resolve(); __out.push([${label}, order]); }`;
        if (scenario === 'queue-microtask')
          return `{ const order = []; queueMicrotask(() => order.push('microtask')); Promise.resolve().then(() => order.push('promise')); await Promise.resolve(); __out.push([${label}, order]); }`;
        if (scenario === 'nested-emitter-catch')
          return `{ const { EventEmitter } = require('node:events'); const emitter = new EventEmitter(); let promise; emitter.on('go', () => { promise = Promise.reject(new Error('probe')); }); emitter.emit('go'); try { await promise; } catch (error) { __out.push([${label}, error.name]); } }`;
        return `{ const { EventEmitter } = require('node:events'); const emitter = new EventEmitter(); let promise; emitter.on('go', () => { promise = Promise.reject(new Error('probe')); }); emitter.emit('go'); await Promise.resolve(); try { await promise; } catch (error) { __out.push([${label}, error.name]); } }`;
      });
      return finish(blocks.join('\n'), true);
    }
  };
}

const streamProperties = [
  'readable',
  'readableEnded',
  'readableFlowing',
  'readableLength',
  'readableHighWaterMark',
  'readableObjectMode',
  'writable',
  'writableEnded',
  'writableFinished',
  'writableLength',
  'writableHighWaterMark',
  'writableObjectMode',
  'destroyed',
  'closed',
  'errored'
];

function streamCase(random, index) {
  const scenarios = [
    { target: 'readable', property: 'keys' },
    { target: 'writable', property: 'keys' },
    ...streamProperties.flatMap(property => [
      { target: 'readable', property },
      { target: 'writable', property }
    ])
  ];
  const scenario = pick(random, scenarios);
  return {
    family: 'stream-shape',
    id: `stream-shape-${index}`,
    shrinkKeys: [],
    params: { scenario },
    build(params) {
      const { target, property } = params.scenario;
      const constructor = target === 'readable' ? 'Readable' : 'Writable';
      const observation =
        property === 'keys'
          ? `__record(${literal(`${target}:keys`)}, () => Object.keys(${target}));`
          : `__record(${literal(`${target}:${property}`)}, () => ({
  value: ${target}[${literal(property)}],
  own: Object.hasOwn(${target}, ${literal(property)}),
  proto: Object.getOwnPropertyDescriptor(${constructor}.prototype, ${literal(property)})
}));`;
      return finish(`const { Readable, Writable } = require('node:stream');
const readable = new Readable({ read() {} });
const writable = new Writable({ write(chunk, encoding, callback) { callback(); } });
${observation}`);
    }
  };
}

const pathScenarios = [
  { style: 'posix', method: 'matchesGlob', args: ['src/a.js', 'src/*.js'] },
  { style: 'posix', method: 'matchesGlob', args: ['src/nested/a.js', '**/*.{js,ts}'] },
  { style: 'posix', method: 'matchesGlob', args: ['src/a.ts', 'src/+(a|b).ts'] },
  { style: 'posix', method: 'matchesGlob', args: ['src/.hidden', 'src/*'] },
  { style: 'posix', method: 'matchesGlob', args: ['a.js', '!*.js'] },
  { style: 'posix', method: 'matchesGlob', args: ['#file', '#file'] },
  { style: 'win32', method: 'matchesGlob', args: ['C:\\src\\a.js', 'C:/src/*.js'] },
  { style: 'win32', method: 'matchesGlob', args: ['C:\\src\\nested\\a.ts', 'C:/src/**/*.{js,ts}'] },
  { style: 'win32', method: 'resolve', args: ['\\\\\\..\\file.txt\\..\\b'] },
  { style: 'win32', method: 'resolve', args: ['\\\\server'] },
  { style: 'win32', method: 'resolve', args: ['\\\\server\\\\share\\a'] },
  { style: 'win32', method: 'resolve', args: ['\\\\server\\share\\a', '\\\\SERVER\\\\share\\b'] },
  { style: 'win32', method: 'resolve', args: ['\\\\?\\C:\\a\\..\\b'] },
  { style: 'win32', method: 'relative', args: ['D:\\b', '\\'] },
  { style: 'win32', method: 'relative', args: ['\\abc\\file.txt', '\\a'] },
  { style: 'win32', method: 'relative', args: ['\\abx', '\\ab'] },
  { style: 'win32', method: 'relative', args: ['\\ab', '\\abx'] },
  { style: 'win32', method: 'relative', args: ['\\', '\\\\server\\share\\a'] },
  { style: 'win32', method: 'relative', args: ['\\\\server\\share', '\\\\server\\other'] },
  { style: 'win32', method: 'relative', args: ['\\\\server\\share', '\\\\server\\share\\a'] },
  { style: 'win32', method: 'relative', args: ['\\\\server\\share\\a', '\\\\server\\share'] },
  { style: 'win32', method: 'relative', args: ['\\\\server\\share', '\\\\other\\share'] },
  { style: 'win32', method: 'relative', args: ['C:\\a', '\\\\server\\share'] },
  { style: 'posix', method: 'normalize', args: ['../../a/../b/'] },
  { style: 'posix', method: 'normalize', args: ['/a/../../b//./'] },
  { style: 'posix', method: 'normalize', args: ['./'] },
  { style: 'win32', method: 'normalize', args: ['C:foo\\..\\..\\bar\\'] },
  { style: 'posix', method: 'resolve', args: ['/', ...Array(40).fill('dir'), ...Array(39).fill('..'), 'file'] },
  { style: 'posix', method: 'relative', args: ['/a', '/ab/file'] },
  { style: 'posix', method: 'relative', args: ['/a/b/c', '/a'] },
  { style: 'posix', method: 'relative', args: ['/' + Array(40).fill('dir').join('/'), '/other'] },
  { style: 'posix', method: 'relative', args: ['.', 'm.js'] },
  { style: 'posix', method: 'relative', args: ['', 'm.js'] },
  { style: 'posix', method: 'relative', args: ['a/..', '.'] },
  { style: 'posix', method: 'relative', args: ['a', 'a/b/c.js'] },
  { style: 'posix', method: 'relative', args: ['a/b', 'a'] },
  { style: 'posix', method: 'relative', args: ['a/../b', 'c'] },
  { style: 'posix', method: 'resolve', args: ['foo', '..', 'bar'] },
  { style: 'win32', method: 'relative', args: ['C:\\a', 'C:foo'] },
  { style: 'win32', method: 'relative', args: ['C:\\a', '\\foo'] },
  { style: 'win32', method: 'relative', args: ['\\a', '\\b'] },
  { style: 'win32', method: 'relative', args: ['\\a\\x', '\\a\\y'] },
  { style: 'win32', method: 'relative', args: ['C:\\a', 'C:\\b'] },
  { style: 'win32', method: 'relative', args: ['C:\\a', 'D:\\b'] },
  { style: 'win32', method: 'relative', args: ['C:foo', 'C:bar'] },
  { style: 'win32', method: 'relative', args: ['C:\\a\\..', 'C:\\'] },
  { style: 'win32', method: 'relative', args: ['C:\\A', 'c:\\a'] },
  { style: 'win32', method: 'relative', args: ['\\\\server\\share\\a', '\\\\server\\share\\b'] },
  { style: 'win32', method: 'relative', args: ['\\\\server\\share\\a', '\\\\server\\other\\b'] },
  { style: 'win32', method: 'resolve', args: ['C:foo'] },
  { style: 'win32', method: 'resolve', args: ['\\foo'] },
  { style: 'win32', method: 'resolve', args: ['C:\\base', '\\foo'] },
  { style: 'win32', method: 'resolve', args: ['C:\\base', 'D:foo'] },
  { style: 'win32', method: 'resolve', args: ['C:base', '\\foo'] },
  { style: 'win32', method: 'resolve', args: ['C:\\base', 'D:\\root', 'tail'] },
  { style: 'win32', method: 'resolve', args: ['\\\\server\\share', 'dir', '..', 'file'] }
];

function pathCase(random, index) {
  const rotation = Math.floor(random() * pathScenarios.length);
  const scenarios = pathScenarios.slice(rotation).concat(pathScenarios.slice(0, rotation));
  // Exercise root parsing and component boundaries beyond the fixed regressions.
  const pick = values => values[Math.floor(random() * values.length)];
  const components = ['a', 'b', 'abc', '.', '..', '', 'file.txt'];
  for (const style of ['posix', 'win32']) {
    const roots = style === 'posix' ? ['', '/'] : ['', 'C:\\', 'D:\\', 'C:', '\\', '\\\\server\\share\\'];
    const makePath = () => pick(roots) + Array.from(
      { length: 1 + Math.floor(random() * 12) }, () => pick(components)
    ).join(style === 'posix' ? '/' : pick(['/', '\\']));
    for (let i = 0; i < 100; i++) {
      const args = [makePath(), makePath()];
      scenarios.push({ style, method: 'resolve', args }, { style, method: 'relative', args });
    }
  }
  return {
    family: 'path',
    id: `path-${index}`,
    shrinkKeys: ['scenarios'],
    params: { scenarios },
    build(params) {
      const observations = params.scenarios
        .map(({ style, method, args }) => {
          const label = `${style}.${method}(${args.map(literal).join(', ')})`;
          return `__record(${literal(label)}, () => path.${style}.${method}(...${literal(args)}));`;
        })
        .join('\n');
      return finish(`const path = require('node:path');\n${observations}`);
    }
  };
}

const builders = {
  property: propertyCase,
  regexp: regexpCase,
  promise: promiseCase,
  'stream-shape': streamCase,
  path: pathCase
};

export const familyNames = Object.freeze(Object.keys(builders));

export function generateCases({ families = familyNames, seed = 1, casesPerFamily = 1 }) {
  const random = mulberry32(seed);
  const cases = [];
  for (const family of families) {
    const builder = builders[family];
    if (!builder) throw new Error(`unknown family: ${family}`);
    for (let index = 0; index < casesPerFamily; index += 1) cases.push(builder(random, index));
  }
  return cases;
}
