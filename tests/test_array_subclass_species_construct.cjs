const assert = require('node:assert');

const log = [];
class Fields extends Array {
  field = 'f';
  #secret = 1;
  static owns(value) { return #secret in value; }
}
class Rest extends Array {
  constructor(...args) { log.push(args.length === 1 ? args[0] : args.length); super(...args); this.rest = true; }
}
class Explicit extends Array {
  #brand = 2;
  constructor(length) { super(length); this.tag = 't'; }
  static owns(value) { return #brand in value; }
}
class Deep extends Fields {
  extra = 'e';
  constructor(...args) { super(...args); }
}
class Other extends Array { other = true; }
class ToOther extends Array { static get [Symbol.species]() { return Other; } }
class ToNull extends Array { static get [Symbol.species]() { return null; } }
class ToUndefined extends Array { static get [Symbol.species]() { return undefined; } }
class ToArray extends Array { static get [Symbol.species]() { return Array; } }
class ToArrow extends Array { static get [Symbol.species]() { return () => []; } }
class ToNumber extends Array { static get [Symbol.species]() { return 1; } }
class ToObject extends Array {
  static get [Symbol.species]() { return function () { return { custom: true, length: 0 }; }; }
}

const methods = {
  map: (a) => a.map((x) => x * 2),
  filter: (a) => a.filter((x) => x > 1),
  slice: (a) => a.slice(1),
  sliceAll: (a) => a.slice(),
  splice: (a) => a.splice(0, 2),
  concat: (a) => a.concat([9], 10),
  flat: (a) => a.flat(),
  flatMap: (a) => a.flatMap((x) => [x, x]),
};

const fill = (C) => {
  const a = new C(3);
  a[0] = 1; a[1] = 2; a[2] = 3;
  return a;
};

const checkInstance = (label, result, C) => {
  assert.strictEqual(Array.isArray(result), true, `${label} isArray`);
  assert.strictEqual(Object.getPrototypeOf(result), C.prototype, `${label} prototype`);
  if (C === Fields || C === Deep) {
    assert.strictEqual(result.field, 'f', `${label} field`);
    assert.strictEqual(Fields.owns(result), true, `${label} private brand`);
  }
  if (C === Deep) assert.strictEqual(result.extra, 'e', `${label} extra`);
  if (C === Rest) assert.strictEqual(result.rest, true, `${label} rest marker`);
  if (C === Other) assert.strictEqual(result.other, true, `${label} other marker`);
  if (C === Explicit) {
    assert.strictEqual(result.tag, 't', `${label} tag`);
    assert.strictEqual(Explicit.owns(result), true, `${label} private brand`);
  }
};

for (let round = 0; round < 300; round++) {
  for (const C of [Fields, Rest, Explicit, Deep]) {
    for (const [name, run] of Object.entries(methods)) {
      const label = `${C.name}.${name} round ${round}`;
      const expected = run(fill(Array));
      const result = run(fill(C));
      checkInstance(label, result, C);
      assert.deepStrictEqual(Array.from(result), expected, `${label} contents`);
      assert.strictEqual(result.length, expected.length, `${label} length`);
    }

    let made = C.of(7);
    checkInstance(`${C.name}.of(7)`, made, C);
    assert.deepStrictEqual([...made], [7]);
    made = C.of(1, 2, 3);
    checkInstance(`${C.name}.of(1,2,3)`, made, C);
    assert.deepStrictEqual([...made], [1, 2, 3]);
    made = C.of();
    checkInstance(`${C.name}.of()`, made, C);
    assert.strictEqual(made.length, 0);

    made = C.from([4, 5]);
    checkInstance(`${C.name}.from(array)`, made, C);
    assert.deepStrictEqual([...made], [4, 5]);
    made = C.from(new Set([6, 7]), (x) => x + 1);
    checkInstance(`${C.name}.from(set)`, made, C);
    assert.deepStrictEqual([...made], [7, 8]);
    made = C.from({ length: 2, 0: 'a', 1: 'b' });
    checkInstance(`${C.name}.from(array-like)`, made, C);
    assert.deepStrictEqual([...made], ['a', 'b']);
    made = C.from('xy');
    checkInstance(`${C.name}.from(string)`, made, C);
    assert.deepStrictEqual([...made], ['x', 'y']);
  }

  log.length = 0;
  Rest.of(7, 8);
  Rest.from([1, 2, 3]);
  Rest.from({ length: 4 });
  fill(Rest).map((x) => x);
  fill(Rest).filter((x) => x);
  assert.deepStrictEqual(log, [2, 0, 4, 3, 3, 3, 0]);

  for (const [name, run] of Object.entries(methods)) {
    const expected = run(fill(Array));
    checkInstance(`ToOther.${name}`, run(fill(ToOther)), Other);
    assert.deepStrictEqual([...run(fill(ToOther))], expected);
    for (const C of [ToNull, ToUndefined, ToArray]) {
      const result = run(fill(C));
      assert.strictEqual(Object.getPrototypeOf(result), Array.prototype, `${C.name}.${name}`);
      assert.deepStrictEqual(result, expected, `${C.name}.${name}`);
    }
    assert.throws(() => run(fill(ToArrow)), TypeError, `ToArrow.${name}`);
    assert.throws(() => run(fill(ToNumber)), TypeError, `ToNumber.${name}`);
    assert.strictEqual(run(fill(ToObject)).custom, true, `ToObject.${name}`);

    const noCtor = fill(Fields);
    noCtor.constructor = undefined;
    assert.strictEqual(Object.getPrototypeOf(run(noCtor)), Array.prototype, `undefined constructor ${name}`);
    const badCtor = fill(Fields);
    badCtor.constructor = 1;
    assert.throws(() => run(badCtor), TypeError, `number constructor ${name}`);
  }

  const plain = [1, 2, 3];
  for (const [name, run] of Object.entries(methods)) {
    const result = run(plain.slice());
    assert.strictEqual(Object.getPrototypeOf(result), Array.prototype, `plain ${name}`);
  }
  assert.strictEqual(Object.getPrototypeOf(Array.of.call(Other, 1)), Other.prototype);
  assert.strictEqual(Object.getPrototypeOf(Array.from.call(Other, [1])), Other.prototype);
  assert.deepStrictEqual(Array.of.call(undefined, 1, 2), [1, 2]);
  assert.deepStrictEqual(Array.from.call(() => {}, [1, 2]), [1, 2]);

  function Legacy(length) { this.args = arguments.length; this.given = length; }
  const legacy = Array.of.call(Legacy, 'a', 'b');
  assert.deepStrictEqual([legacy instanceof Legacy, legacy.args, legacy.given, legacy.length, legacy[0], legacy[1]], [true, 1, 2, 2, 'a', 'b']);
  const fromLegacy = Array.from.call(Legacy, ['a']);
  assert.deepStrictEqual([fromLegacy instanceof Legacy, fromLegacy.args, fromLegacy.length, fromLegacy[0]], [true, 0, 1, 'a']);
}

console.log('ok');
