let assertions = 0;

function check(name, actual, expected) {
  assertions++;
  const actualJson = JSON.stringify(actual);
  const expectedJson = JSON.stringify(expected);
  if (actualJson !== expectedJson) {
    throw new Error(`${name}: got ${actualJson}, expected ${expectedJson}`);
  }
}

function hasOwn(object, key) {
  return Object.prototype.hasOwnProperty.call(object, key);
}

// Deleting enough interleaved properties forces stable compaction. Surviving
// values and insertion order must remain paired, and re-added keys append.
{
  const object = {};
  for (let i = 0; i < 80; i++) object[`k${i}`] = i;
  for (let i = 0; i < 80; i += 2) delete object[`k${i}`];

  const oddKeys = [];
  for (let i = 1; i < 80; i += 2) oddKeys.push(`k${i}`);
  check('keys after compacting deletes', Object.keys(object), oddKeys);
  check('values after compacting deletes', oddKeys.map(key => object[key]),
    oddKeys.map(key => Number(key.slice(1))));

  for (let i = 0; i < 80; i += 2) object[`k${i}`] = i + 1000;
  const evenKeys = [];
  for (let i = 0; i < 80; i += 2) evenKeys.push(`k${i}`);
  check('re-added keys append', Object.keys(object), oddKeys.concat(evenKeys));
  check('re-added values remain paired', evenKeys.map(key => object[key]),
    evenKeys.map(key => Number(key.slice(1)) + 1000));
}

// Accessor metadata and its backing values move with their property.
{
  let stored = 17;
  const object = {};
  for (let i = 0; i < 40; i++) object[`before${i}`] = i;
  Object.defineProperty(object, 'accessor', {
    configurable: true,
    enumerable: true,
    get() { return stored; },
    set(value) { stored = value; },
  });
  for (let i = 0; i < 39; i++) object[`after${i}`] = i;
  for (let i = 0; i < 40; i++) delete object[`before${i}`];

  check('accessor getter after compaction', object.accessor, 17);
  object.accessor = 23;
  check('accessor setter after compaction', [stored, object.accessor], [23, 23]);
  check('accessor remains enumerable',
    Object.keys(object).slice(0, 2), ['accessor', 'after0']);
}

// A warmed own-property read must fall back to the prototype after deletion,
// including after later deletions compact the receiver's shape.
{
  const proto = { inherited: 'prototype' };
  const object = Object.create(proto);
  object.inherited = 'own';
  for (let i = 0; i < 80; i++) object[`p${i}`] = i;

  function readInherited(value) {
    return value.inherited;
  }
  for (let i = 0; i < 200; i++) readInherited(object);

  delete object.inherited;
  for (let i = 0; i < 40; i++) delete object[`p${i}`];
  check('prototype fallback after compaction',
    [readInherited(object), hasOwn(object, 'inherited')],
    ['prototype', false]);
}

// A warmed write must follow a moved property rather than its old slot.
{
  const object = {};
  for (let i = 0; i < 40; i++) object[`prefix${i}`] = i;
  object.keep = 1;
  for (let i = 0; i < 39; i++) object[`suffix${i}`] = i;

  function writeKeep(value, next) {
    value.keep = next;
  }
  for (let i = 0; i < 200; i++) writeKeep(object, i);
  for (let i = 0; i < 40; i++) delete object[`prefix${i}`];
  writeKeep(object, 999);
  check('cached write after compaction',
    [object.keep, object.suffix0, object.suffix38],
    [999, 0, 38]);
}

// Symbols retain their own insertion order, while integer-index keys retain
// the ECMAScript numeric ordering independent of physical slots.
{
  const first = Symbol('first');
  const second = Symbol('second');
  const object = { 10: 'ten', 2: 'two' };
  object[first] = 1;
  object.middle = true;
  object[second] = 2;
  for (let i = 0; i < 70; i++) object[`hole${i}`] = i;

  delete object[first];
  delete object[2];
  for (let i = 0; i < 35; i++) delete object[`hole${i}`];
  object[first] = 3;
  object[2] = 'two again';

  check('integer and string key order after re-add',
    Object.keys(object).slice(0, 4),
    ['2', '10', 'middle', 'hole35']);
  const symbols = Object.getOwnPropertySymbols(object);
  check('symbol re-add moves to end',
    [symbols.length, symbols[0] === second, symbols[1] === first],
    [2, true, true]);
}

// Deleting from one object must not mutate a shared literal-site shape.
{
  function makePair() {
    return { first: 1, second: 2, third: 3 };
  }
  const left = makePair();
  const right = makePair();
  delete left.second;
  check('shared shape clone isolation',
    [Object.keys(left), Object.keys(right), right.second],
    [['first', 'third'], ['first', 'second', 'third'], 2]);
}

// The RegExp lastIndex fast location is shape guarded. Compaction of later
// user properties must not make it read or write a neighboring value.
{
  const regexp = /a/g;
  check('regexp warmup', regexp.exec('a a').index, 0);
  for (let i = 0; i < 64; i++) regexp[`extra${i}`] = i;
  for (let i = 0; i < 32; i++) delete regexp[`extra${i}`];
  regexp.lastIndex = 2;
  check('regexp lastIndex after compaction',
    [regexp.exec('a a').index, regexp.lastIndex, regexp.extra32],
    [2, 3, 32]);
}

// Repeated delete/reinsert cycles exercise compaction under allocation churn.
{
  const object = {};
  for (let i = 0; i < 96; i++) object[`live${i}`] = { round: -1, index: i };

  for (let round = 0; round < 40; round++) {
    for (let i = 0; i < 48; i++) delete object[`live${i}`];
    for (let i = 0; i < 48; i++) object[`live${i}`] = { round, index: i };
    const pressure = [];
    for (let i = 0; i < 128; i++) pressure.push({ round, i });
  }

  check('churn key count', Object.keys(object).length, 96);
  check('churn surviving values',
    [object.live48.index, object.live0.round, object.live47.index],
    [48, 39, 47]);
  check('churn reinsertion order',
    Object.keys(object).slice(0, 4),
    ['live48', 'live49', 'live50', 'live51']);
}

console.log(`property delete compaction semantics ok (${assertions} assertions)`);
