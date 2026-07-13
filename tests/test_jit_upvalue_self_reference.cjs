function assertSame(actual, expected, message) {
  if (actual !== expected) throw new Error(message);
}

function makeMutableSelfReference() {
  let current;
  const original = function () {
    return current;
  };

  current = original;
  for (let i = 0; i < 300; i++) {
    assertSame(original(), original, "warm mutable self-reference");
  }

  const replacement = function () {};
  current = replacement;
  return { original, replacement };
}

const pair = makeMutableSelfReference();
assertSame(
  pair.original(),
  pair.replacement,
  "JIT must observe reassigned mutable self-reference"
);
for (let i = 0; i < 300; i++) {
  assertSame(
    pair.original(),
    pair.replacement,
    "recompiled function must keep observing mutable self-reference"
  );
}

globalThis.__antMutableGlobalSelf = function () {
  return __antMutableGlobalSelf;
};

const globalOriginal = globalThis.__antMutableGlobalSelf;
for (let i = 0; i < 300; i++) {
  assertSame(
    globalOriginal(),
    globalOriginal,
    "warm mutable global self-reference"
  );
}

const globalReplacement = function () {};
globalThis.__antMutableGlobalSelf = globalReplacement;
assertSame(
  globalOriginal(),
  globalReplacement,
  "JIT must observe reassigned global self-reference"
);
delete globalThis.__antMutableGlobalSelf;

function makeCapturedTailCall() {
  let current;
  const original = function (n) {
    if (n === 0) return 0;
    return current(n - 1);
  };
  current = original;
  return {
    original,
    replace(value) {
      current = value;
    },
  };
}

const capturedTail = makeCapturedTailCall();
for (let i = 0; i < 300; i++) {
  assertSame(capturedTail.original(2), 0, "warm captured self tail call");
}
assertSame(
  capturedTail.original(100_000),
  0,
  "captured self-reference must retain TCO"
);
capturedTail.replace(function () {
  return 42;
});
assertSame(
  capturedTail.original(1),
  42,
  "captured tail call must observe reassigned binding"
);

globalThis.__antMutableGlobalTail = function (n) {
  if (n === 0) return 0;
  return __antMutableGlobalTail(n - 1);
};

const globalTailOriginal = globalThis.__antMutableGlobalTail;
for (let i = 0; i < 300; i++) {
  assertSame(globalTailOriginal(2), 0, "warm global self tail call");
}
assertSame(
  globalTailOriginal(100_000),
  0,
  "global self-reference must retain TCO"
);
globalThis.__antMutableGlobalTail = function () {
  return 42;
};
assertSame(
  globalTailOriginal(1),
  42,
  "global tail call must observe reassigned binding"
);
delete globalThis.__antMutableGlobalTail;

function conditionalOther() {
  return 42;
}

globalThis.__antConditionalSelf = function (n, chooseOther) {
  if (n === 0) return 0;
  return (chooseOther ? conditionalOther : __antConditionalSelf)(
    n - 1,
    chooseOther
  );
};

const conditionalOriginal = globalThis.__antConditionalSelf;
for (let i = 0; i < 300; i++) {
  assertSame(conditionalOriginal(2, false), 0, "warm conditional self arm");
}
assertSame(
  conditionalOriginal(1, true),
  42,
  "conditional merge must clear known self function"
);
delete globalThis.__antConditionalSelf;

globalThis.__antLocalMergeSelf = function (n, chooseOther) {
  if (n === 0) return 0;
  let callee;
  if (chooseOther) callee = conditionalOther;
  else callee = __antLocalMergeSelf;
  return callee(n - 1, chooseOther);
};

const localMergeOriginal = globalThis.__antLocalMergeSelf;
for (let i = 0; i < 300; i++) {
  assertSame(localMergeOriginal(2, false), 0, "warm local self arm");
}
assertSame(
  localMergeOriginal(1, true),
  42,
  "control-flow merge must clear known function locals"
);
delete globalThis.__antLocalMergeSelf;

function makeConstSibling(target) {
  const captured = target;
  return function () {
    return captured;
  };
}

const siblingSeed = makeConstSibling(null);
const siblingReader = makeConstSibling(siblingSeed);
for (let i = 0; i < 300; i++) {
  assertSame(
    siblingReader(),
    siblingSeed,
    "shared function body must preserve sibling closure identity"
  );
}

function makeConstSelfOrSibling(previous) {
  function inner() {
    return current;
  }
  const current = previous || inner;
  return inner;
}

const constSelf = makeConstSelfOrSibling(null);
for (let i = 0; i < 300; i++) {
  assertSame(constSelf(), constSelf, "warm const self-reference");
}
const constSibling = makeConstSelfOrSibling(constSelf);
assertSame(
  constSibling(),
  constSelf,
  "shared JIT code must load each closure's const upvalue"
);

let accessorReads = 0;
const accessorTarget = function () {
  return __antAccessorSelf;
};
Object.defineProperty(globalThis, "__antAccessorSelf", {
  configurable: true,
  get() {
    accessorReads++;
    return accessorTarget;
  },
});
for (let i = 0; i < 300; i++) {
  assertSame(accessorTarget(), accessorTarget, "accessor global self read");
}
assertSame(
  accessorReads,
  300,
  "JIT compilation must not invoke a global accessor"
);
delete globalThis.__antAccessorSelf;

let undefAccessorReads = 0;
Object.defineProperty(globalThis, "__antAccessorTypeofSelf", {
  configurable: true,
  get() {
    undefAccessorReads++;
    return accessorTarget;
  },
});
const readAccessorType = function () {
  return typeof __antAccessorTypeofSelf;
};
for (let i = 0; i < 300; i++) {
  assertSame(readAccessorType(), "function", "accessor global typeof read");
}
assertSame(
  undefAccessorReads,
  300,
  "GET_GLOBAL_UNDEF compilation must not invoke a global accessor"
);
delete globalThis.__antAccessorTypeofSelf;

console.log("PASS");
