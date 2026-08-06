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
