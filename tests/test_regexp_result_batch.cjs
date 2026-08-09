function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function assertJson(actual, expected, message) {
  assert(
    JSON.stringify(actual) === JSON.stringify(expected),
    `${message}: ${JSON.stringify(actual)}`
  );
}

const result = /(a)(b)?/d.exec('xa');
assertJson(result, ['a', 'a', undefined], 'result captures');
assert(result.index === 1, 'result index');
assert(result.input === 'xa', 'result input');
assert(result.groups === undefined, 'result groups');
assert(Object.hasOwn(result, 'indices'), 'result owns indices');
assertJson(result.indices, [[1, 2], [1, 2], undefined], 'result indices');
assertJson(
  Object.keys(result),
  ['0', '1', '2', 'index', 'input', 'groups', 'indices'],
  'result property order'
);
for (const key of ['index', 'input', 'groups', 'indices']) {
  const descriptor = Object.getOwnPropertyDescriptor(result, key);
  assert(
    descriptor.writable && descriptor.enumerable && descriptor.configurable,
    `${key} descriptor`
  );
}

let capturePattern = '';
for (let i = 0; i < 40; i++) capturePattern += '(.)';
const manyCaptures = new RegExp(capturePattern, 'd').exec(
  'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN'
);
assert(manyCaptures.length === 41, 'exec result includes captures beyond 31');
assert(manyCaptures[32] === 'F', 'capture 32 value');
assert(manyCaptures[40] === 'N', 'capture 40 value');
assert(manyCaptures.indices.length === 41, 'indices include captures beyond 31');
assertJson(manyCaptures.indices[40], [39, 40], 'capture 40 indices');
assert(
  'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN'.replace(
    new RegExp(capturePattern, 'g'),
    '$40-$32'
  ) === 'N-F',
  'batched replace can read captures beyond 31'
);
let replacerArgCount = 0;
let replacerCapture40;
'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN'.replace(
  new RegExp(capturePattern, 'g'),
  function () {
    replacerArgCount = arguments.length;
    replacerCapture40 = arguments[40];
    return 'ok';
  }
);
assert(replacerArgCount === 43, 'function replacer receives every capture');
assert(replacerCapture40 === 'N', 'function replacer capture 40 value');

let namedCapturePattern = '';
for (let i = 0; i < 39; i++) namedCapturePattern += '(.)';
namedCapturePattern += '(?<last>.)';
const namedManyCaptures = new RegExp(namedCapturePattern, 'd').exec(
  'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN'
);
assert(namedManyCaptures.groups.last === 'N', 'named capture beyond 31');
assertJson(namedManyCaptures.indices.groups.last, [39, 40], 'named indices beyond 31');

let regexp = /a/g;
assertJson('baac'.match(regexp), ['a', 'a'], 'global match');
assert(regexp.lastIndex === 0, 'global match final lastIndex');

regexp = /(?:)/gu;
assertJson('🎈x'.match(regexp), ['', '', ''], 'unicode empty global match');
assert(regexp.lastIndex === 0, 'unicode empty match final lastIndex');

regexp = /a/gy;
assertJson('aa ba'.match(regexp), ['a', 'a'], 'global sticky match');
assert(regexp.lastIndex === 0, 'global sticky final lastIndex');

regexp = /(a)(b)/g;
assertJson('abxab'.match(regexp), ['ab', 'ab'], 'global capture match values');
assert(RegExp.$1 === 'a' && RegExp.$2 === 'b', 'global match RegExp statics');

regexp = /([a-z]+)(\d+)?/g;
assert(
  'ab12 cd'.replace(regexp, "<$1|$2|$&|$$|$`|$'>") ===
    '<ab|12|ab12|$|| cd> <cd||cd|$|ab12 |>',
  'global substitution replace'
);
assert(regexp.lastIndex === 0, 'global replace final lastIndex');

regexp = /(?:)/gu;
assert('🎈x'.replace(regexp, '_') === '_🎈_x_', 'unicode empty global replace');
assert(regexp.lastIndex === 0, 'unicode empty replace final lastIndex');

const readonlyMatch = /a/g;
Object.defineProperty(readonlyMatch, 'lastIndex', {
  value: 0,
  writable: false,
});
let readonlyMatchThrew = false;
try {
  'a'.match(readonlyMatch);
} catch (error) {
  readonlyMatchThrew = error instanceof TypeError;
}
assert(readonlyMatchThrew, 'readonly lastIndex match must use generic semantics');

const readonlyReplace = /a/g;
Object.defineProperty(readonlyReplace, 'lastIndex', {
  value: 0,
  writable: false,
});
let readonlyReplaceThrew = false;
try {
  'a'.replace(readonlyReplace, 'x');
} catch (error) {
  readonlyReplaceThrew = error instanceof TypeError;
}
assert(readonlyReplaceThrew, 'readonly lastIndex replace must use generic semantics');

const builtinExec = RegExp.prototype.exec;
let execGetterCalls = 0;
const accessorExec = /a/g;
Object.defineProperty(accessorExec, 'exec', {
  configurable: true,
  get() {
    execGetterCalls++;
    return builtinExec;
  },
});
assertJson('aba'.match(accessorExec), ['a', 'a'], 'accessor exec match');
assert(execGetterCalls === 3, 'accessor exec must run once per RegExpExec');

let customExecCalls = 0;
const customExec = /a/g;
customExec.exec = function () {
  customExecCalls++;
  return customExecCalls === 1 ? ['x'] : null;
};
assertJson('aaa'.match(customExec), ['x'], 'custom exec match');
assert(customExecCalls === 2, 'custom exec call count');

let replacerCalls = 0;
regexp = /(a)/g;
assert(
  'aba'.replace(regexp, (match, capture, index) => {
    replacerCalls++;
    return capture + index;
  }) === 'a0ba2',
  'function replacer fallback'
);
assert(replacerCalls === 2, 'function replacer call count');

console.log('regexp result and batch fast paths ok');
