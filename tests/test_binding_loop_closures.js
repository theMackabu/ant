const failures = [];

function assertArrayEq(name, got, expected) {
  const ok = got.length === expected.length &&
    got.every((v, i) => v === expected[i]);
  if (!ok) {
    failures.push(`${name}: expected [${expected.join(', ')}], got [${got.join(', ')}]`);
  }
}

function collectForInLet() {
  const funcs = [];
  for (let key in { a: 1, b: 1, c: 1 }) {
    funcs.push(() => key);
  }
  return funcs.map(fn => fn());
}

function collectForInConst() {
  const funcs = [];
  for (const key in { a: 1, b: 1, c: 1 }) {
    funcs.push(() => key);
  }
  return funcs.map(fn => fn());
}

function collectForOfLet() {
  const funcs = [];
  for (let value of ['x', 'y', 'z']) {
    funcs.push(() => value);
  }
  return funcs.map(fn => fn());
}

function collectForOfConst() {
  const funcs = [];
  for (const value of ['x', 'y', 'z']) {
    funcs.push(() => value);
  }
  return funcs.map(fn => fn());
}

function collectForOfDestructureLet() {
  const funcs = [];
  for (let [k, v] of [['a', 1], ['b', 2], ['c', 3]]) {
    funcs.push(() => `${k}:${v}`);
  }
  return funcs.map(fn => fn());
}

assertArrayEq('for-in let closure', collectForInLet(), ['a', 'b', 'c']);
assertArrayEq('for-in const closure', collectForInConst(), ['a', 'b', 'c']);
assertArrayEq('for-of let closure', collectForOfLet(), ['x', 'y', 'z']);
assertArrayEq('for-of const closure', collectForOfConst(), ['x', 'y', 'z']);
assertArrayEq(
  'for-of let destructuring closure',
  collectForOfDestructureLet(),
  ['a:1', 'b:2', 'c:3']
);

if (failures.length > 0) {
  console.log('Binding loop closure failures:');
  for (let i = 0; i < failures.length; i++) {
    console.log(`  ${i + 1}. ${failures[i]}`);
  }
  throw new Error(`Found ${failures.length} binding-loop closure regression(s)`);
}

console.log('PASS: binding loop closures');
