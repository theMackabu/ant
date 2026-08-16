function assertEq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(`${label}: got ${actual}, expected ${expected}`);
  }
}

function basicAccumulation() {
  let s = '';
  for (let i = 0; i < 8; i++) s = `${s}x`;
  return s;
}

function numericHead() {
  let s = 5;
  const n = 2;
  s = `${s}${n}`;
  return s;
}

function multiPart() {
  let s = 'a';
  const x = 1, y = 'b';
  s = `${s}-${x}=${y}!`;
  return s;
}

function observesOldValue() {
  let s = 'base';
  const spy = { toString() { return '[' + s + ']'; } };
  s = `${s}${spy}`;
  return s;
}

function writeDuringEvaluation() {
  let s = 'old';
  function clobber() { s = 'clobbered'; return '+'; }
  s = `${s}${clobber()}`;
  return s;
}

function writeDuringCoercion() {
  let s = 'old';
  const part = { toString() { s = 'clobbered'; return '+'; } };
  s = `${s}${part}`;
  return s;
}

function objectHeadCoercionOrder() {
  const order = [];
  let s = { toString() { order.push('head'); return 'H'; } };
  const part = { toString() { order.push('part'); return 'R'; } };
  s = `${s}${part}`;
  return order.join(',') + ':' + s;
}

function symbolHeadThrows() {
  let s = Symbol('head');
  try {
    s = `${s}`;
  } catch (e) {
    return e instanceof TypeError;
  }
  return false;
}

function snapshotAliasing() {
  let s = '';
  for (let i = 0; i < 8; i++) s = `${s}x`;
  const snapshot = s;
  s = `${s}y`;
  return snapshot + ':' + s;
}

function plusEqTemplate() {
  let s = 'a';
  const x = 'b';
  s += `${x}c`;
  return s;
}

function headRepeated() {
  let s = 'a';
  s = `${s}-${s}`;
  return s;
}

function symbolThrows() {
  let s = 'a';
  try {
    s = `${s}${Symbol('x')}`;
  } catch (e) {
    return e instanceof TypeError;
  }
  return false;
}

for (let i = 0; i < 300; i++) {
  assertEq(basicAccumulation(), 'xxxxxxxx', `basic ${i}`);
  assertEq(numericHead(), '52', `numeric head ${i}`);
  assertEq(multiPart(), 'a-1=b!', `multi part ${i}`);
  assertEq(observesOldValue(), 'base[base]', `observes old ${i}`);
  assertEq(writeDuringEvaluation(), 'old+', `write during ${i}`);
  assertEq(writeDuringCoercion(), 'old+', `write during coercion ${i}`);
  assertEq(objectHeadCoercionOrder(), 'head,part:HR', `head coercion order ${i}`);
  assertEq(symbolHeadThrows(), true, `symbol head throws ${i}`);
  assertEq(snapshotAliasing(), 'xxxxxxxx:xxxxxxxxy', `snapshot ${i}`);
  assertEq(plusEqTemplate(), 'abc', `plus-eq template ${i}`);
  assertEq(headRepeated(), 'a-a', `head repeated ${i}`);
  assertEq(symbolThrows(), true, `symbol throws ${i}`);
}

function staleHeadAcrossBackEdge() {
  const order = [];
  let s = 'a';
  const part = { toString() { order.push('part'); return 'P'; } };
  for (let i = 0; i < 2; i++) {
    s = `${s}${part}`;
    if (i === 0) s = {
      toString() { order.push('head'); return 'H'; },
      valueOf() { order.push('valueOf'); return 'V'; }
    };
  }
  return order.join(',') + ':' + s;
}
assertEq(staleHeadAcrossBackEdge(), 'part,head,part:HP', 'stale head across back-edge');

function staleSymbolAcrossBackEdge() {
  let partRan = false;
  const part = { toString() { partRan = true; return 'P'; } };
  let s = 'a';
  for (let i = 0; i < 2; i++) {
    try {
      s = `${s}${part}`;
    } catch (e) {
      return (e instanceof TypeError) + ':' + partRan;
    }
    partRan = false;
    if (i === 0) s = Symbol('stale');
  }
  return 'no-throw';
}
assertEq(staleSymbolAcrossBackEdge(), 'true:false', 'stale symbol across back-edge');

function throwingHeadOrdinaryTemplate() {
  let partRan = false;
  const head = { toString() { throw new Error('boom'); } };
  const part = { toString() { partRan = true; return 'P'; } };
  try {
    return `${head}${part}`;
  } catch (e) {
    return e.message + ':' + partRan;
  }
}
assertEq(throwingHeadOrdinaryTemplate(), 'boom:false', 'throwing head ordinary template');

function plusEqSymbolThrows() {
  let s = '';
  try {
    s += Symbol('x');
  } catch (e) {
    return e instanceof TypeError;
  }
  return false;
}
assertEq(plusEqSymbolThrows(), true, 'plus-eq symbol throws');

function plusEqSymbolToPrimitiveThrows() {
  let s = '';
  try {
    s += { [Symbol.toPrimitive]() { return Symbol('x'); } };
  } catch (e) {
    return e instanceof TypeError;
  }
  return false;
}
assertEq(plusEqSymbolToPrimitiveThrows(), true, 'plus-eq Symbol.toPrimitive throws');

function largeAccumulation() {
  let s = '';
  for (let i = 0; i < 100000; i++) s = `${s}ab`;
  return s.length;
}
assertEq(largeAccumulation(), 200000, 'large accumulation');

console.log('OK: test_template_self_append');
