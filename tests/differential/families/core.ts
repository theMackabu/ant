// The original families: property semantics, RegExp, Promise ordering,
// stream property shape, and path resolution.

import fc from 'fast-check';
import { finish, literal } from '../prelude.ts';
import { defineFamily } from './family.ts';

const propertyKeys = ['plain', '', '0', '-0', '01', '__proto__', 'constructor', 'a\0b', 'é', '😀'];

export const property = defineFamily({
  name: 'property',
  arbitrary: fc.record({ keys: fc.uniqueArray(fc.constantFrom(...propertyKeys), { minLength: 1, maxLength: 7 }) }),
  build({ keys }) {
    const assignments = keys.map((key, i) => `o[${literal(key)}] = ${i + 1};`).join('\n');
    const observations = keys
      .map(
        key =>
          `__record(${literal(`get:${key}`)}, () => o[${literal(key)}]);\n` +
          `__record(${literal(`has:${key}`)}, () => Object.hasOwn(o, ${literal(key)}));\n` +
          `__record(${literal(`descriptor:${key}`)}, () => Object.getOwnPropertyDescriptor(o, ${literal(key)}));`
      )
      .join('\n');
    return finish(`const o = {};\n${assignments}\n${observations}\n__record('keys', () => Object.keys(o));\n__record('json', () => JSON.stringify(o));`);
  }
});

interface RegExpScenario {
  pattern: string;
  flags: string;
  input: string;
  replacement: string;
  lastIndex?: string;
}

const regexpScenarios: RegExpScenario[] = [
  { pattern: '', flags: '', input: '', replacement: '_' },
  { pattern: '(?<m>b)', flags: 'g', input: 'abc', replacement: '[$<m>]' },
  { pattern: '(a)?b', flags: 'd', input: 'b', replacement: '$1' },
  { pattern: '(?:)', flags: 'g', input: '😀', replacement: '-' },
  { pattern: 'a', flags: 'g', input: 'baac', replacement: 'x', lastIndex: '1' }
];

export const regexp = defineFamily({
  name: 'regexp',
  arbitrary: fc.record({ scenarios: fc.uniqueArray(fc.constantFrom(...regexpScenarios), { minLength: 1, maxLength: 5 }) }),
  build({ scenarios }) {
    return finish(
      scenarios
        .map((scenario, i) => {
          const lastIndex = scenario.lastIndex === undefined ? '' : `r${i}.lastIndex = ${literal(scenario.lastIndex)};`;
          return `const r${i} = new RegExp(${literal(scenario.pattern)}, ${literal(scenario.flags)});
${lastIndex}
__record(${literal(`source:${i}`)}, () => r${i}.source);
__record(${literal(`exec:${i}`)}, () => r${i}.exec(${literal(scenario.input)}));
__record(${literal(`lastIndex:${i}`)}, () => r${i}.lastIndex);
__record(${literal(`replace:${i}`)}, () => ${literal(scenario.input)}.replace(r${i}, ${literal(scenario.replacement)}));`;
        })
        .join('\n')
    );
  }
});

const promiseScenarios = ['basic-order', 'nested-emitter-catch', 'nested-emitter-late-catch', 'queue-microtask'] as const;

export const promise = defineFamily({
  name: 'promise',
  arbitrary: fc.record({ scenarios: fc.uniqueArray(fc.constantFrom(...promiseScenarios), { minLength: 1, maxLength: 4 }) }),
  build({ scenarios }) {
    const blocks = scenarios.map((scenario, i) => {
      const label = literal(`${i}:${scenario}`);
      if (scenario === 'basic-order')
        return `{ const order = []; Promise.resolve().then(() => order.push('then')); order.push('sync'); await Promise.resolve(); __out.push([${label}, order]); }`;
      if (scenario === 'queue-microtask')
        return `{ const order = []; queueMicrotask(() => order.push('microtask')); Promise.resolve().then(() => order.push('promise')); await Promise.resolve(); __out.push([${label}, order]); }`;
      const late = scenario === 'nested-emitter-late-catch' ? 'await Promise.resolve(); ' : '';
      return `{ const { EventEmitter } = require('node:events'); const emitter = new EventEmitter(); let promise; emitter.on('go', () => { promise = Promise.reject(new Error('probe')); }); emitter.emit('go'); ${late}try { await promise; } catch (error) { __out.push([${label}, error.name]); } }`;
    });
    return finish(blocks.join('\n'), true);
  }
});

const streamProperties = [
  'readable', 'readableEnded', 'readableFlowing', 'readableLength', 'readableHighWaterMark', 'readableObjectMode',
  'writable', 'writableEnded', 'writableFinished', 'writableLength', 'writableHighWaterMark', 'writableObjectMode',
  'destroyed', 'closed', 'errored'
];

// Atomic: each case checks one property or one target's own keys, so a broad
// mismatch does not contaminate unrelated cases.
export const streamShape = defineFamily({
  name: 'stream-shape',
  arbitrary: fc.record({
    target: fc.constantFrom('readable', 'writable'),
    property: fc.constantFrom('keys', ...streamProperties)
  }),
  build({ target, property }) {
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
});

interface PathScenario {
  style: 'posix' | 'win32';
  method: 'matchesGlob' | 'resolve' | 'relative' | 'normalize';
  args: string[];
}

// Fixed regressions, always included.
const pathRegressions: PathScenario[] = [
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

const components = ['a', 'b', 'abc', '.', '..', '', 'file.txt'];
const posixPath = fc
  .tuple(fc.constantFrom('', '/'), fc.array(fc.constantFrom(...components), { minLength: 1, maxLength: 12 }))
  .map(([root, parts]) => root + parts.join('/'));
const win32Path = fc
  .tuple(
    fc.constantFrom('', 'C:\\', 'D:\\', 'C:', '\\', '\\\\server\\share\\'),
    fc.array(fc.tuple(fc.constantFrom(...components), fc.constantFrom('/', '\\')), { minLength: 1, maxLength: 12 })
  )
  .map(([root, parts]) => root + parts.map(([part, sep], i) => (i ? sep : '') + part).join(''));

const pathPair = fc.oneof(
  fc.tuple(fc.constant('posix' as const), fc.constantFrom('resolve' as const, 'relative' as const), posixPath, posixPath),
  fc.tuple(fc.constant('win32' as const), fc.constantFrom('resolve' as const, 'relative' as const), win32Path, win32Path)
).map(([style, method, a, b]): PathScenario => ({ style, method, args: [a, b] }));

export const pathFamily = defineFamily({
  name: 'path',
  arbitrary: fc.record({ generated: fc.array(pathPair, { maxLength: 200, size: 'max' }) }),
  build({ generated }) {
    const observations = [...pathRegressions, ...generated]
      .map(({ style, method, args }) => {
        const label = `${style}.${method}(${args.map(literal).join(', ')})`;
        return `__record(${literal(label)}, () => path.${style}.${method}(...${literal(args)}));`;
      })
      .join('\n');
    return finish(`const path = require('node:path');\n${observations}`);
  }
});
