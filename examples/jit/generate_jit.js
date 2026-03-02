let pass = 0,
  fail = 0;
function check(name, got, expected) {
  if (got === expected) {
    pass++;
    console.log('PASS', name, 'got:', got, 'expected:', expected);
  } else {
    fail++;
    console.log('FAIL', name, 'got:', got, 'expected:', expected);
  }
}

function warm(fn) {
  for (let i = 0; i < 110; i++) fn();
}
function warm1(fn) {
  for (let i = 0; i < 110; i++) fn(i);
}
function warm2(fn) {
  for (let i = 0; i < 110; i++) fn(i, i + 1);
}

function testConstI8() {
  return 7;
}
warm(testConstI8);
check('CONST_I8', testConstI8(), 7);

function testConst() {
  return 123456789;
}
warm(testConst);
check('CONST', testConst(), 123456789);

function testConst8() {
  return 3.14;
}
warm(testConst8);
check('CONST8', testConst8(), 3.14);

function testUndef() {
  return undefined;
}
warm(testUndef);
check('UNDEF', testUndef(), undefined);

function testNull() {
  return null;
}
warm(testNull);
check('NULL', testNull(), null);

function testTrue() {
  return true;
}
function testFalse() {
  return false;
}
warm(testTrue);
warm(testFalse);
check('TRUE', testTrue(), true);
check('FALSE', testFalse(), false);

function testThis() {
  return this;
}
warm(testThis);
check('THIS', testThis() === undefined || typeof testThis() === 'object', true);

function testPop() {
  let x = 1;
  2;
  return x;
}
warm(testPop);
check('POP', testPop(), 1);

function testDup() {
  let x;
  return (x = 42);
}
warm(testDup);
check('DUP', testDup(), 42);

function testDup2() {
  let a = 1,
    b = 2;
  return a + b + a + b;
}
warm(testDup2);
check('DUP2', testDup2(), 6);

function testSwap(a, b) {
  return [b, a];
}
warm2(testSwap);
let sw = testSwap(1, 2);
check('SWAP', sw[0] === 2 && sw[1] === 1, true);

function testRot3l() {
  let o = {};
  o.x = 5;
  return o.x;
}
warm(testRot3l);
check('ROT3L', testRot3l(), 5);

function testInsert2() {
  let o = { x: 10 };
  o.x += 5;
  return o.x;
}
warm(testInsert2);
check('INSERT2', testInsert2(), 15);

function testInsert3() {
  let o = { x: 10 };
  let k = 'x';
  o[k] += 3;
  return o[k];
}
warm(testInsert3);
check('INSERT3', testInsert3(), 13);

function testGetArg(a, b, c) {
  return a + b + c;
}
warm(function () {
  testGetArg(1, 2, 3);
});
check('GET_ARG', testGetArg(10, 20, 30), 60);

function testSetArg(a) {
  a = 99;
  return a;
}
warm1(testSetArg);
check('SET_ARG', testSetArg(0), 99);

function testLocals() {
  let x = 10;
  let y = x + 5;
  x = y * 2;
  return x;
}
warm(testLocals);
check('GET/PUT/SET_LOCAL', testLocals(), 30);

function testLocal8() {
  let a = 1;
  return a;
}
warm(testLocal8);
check('LOCAL8', testLocal8(), 1);

function testSetLocalUndef() {
  let x = 42;
  return x;
}
warm(testSetLocalUndef);
check('SET_LOCAL_UNDEF', testSetLocalUndef(), 42);

function testIncLocal() {
  let x = 0;
  x++;
  x++;
  x++;
  return x;
}
warm(testIncLocal);
check('INC_LOCAL', testIncLocal(), 3);

function testDecLocal() {
  let x = 10;
  x--;
  x--;
  return x;
}
warm(testDecLocal);
check('DEC_LOCAL', testDecLocal(), 8);

function testAddLocal() {
  let x = 0;
  x += 10;
  x += 32;
  return x;
}
warm(testAddLocal);
check('ADD_LOCAL', testAddLocal(), 42);

function testGetUpval() {
  let v = 77;
  function inner() {
    return v;
  }
  return inner();
}
warm(testGetUpval);
check('GET_UPVAL', testGetUpval(), 77);

function testPutUpval() {
  let v = 0;
  function inc() {
    v = v + 1;
  }
  inc();
  inc();
  return v;
}
warm(testPutUpval);
check('PUT_UPVAL', testPutUpval(), 2);

function testSetUpval() {
  let v = 0;
  function set() {
    return (v = 42);
  }
  return set();
}
warm(testSetUpval);
check('SET_UPVAL', testSetUpval(), 42);

function testClosure() {
  function inner(x) {
    return x + 1;
  }
  return inner(41);
}
warm(testClosure);
check('CLOSURE', testClosure(), 42);

function testCloseUpval() {
  let fns = [];
  for (let i = 0; i < 3; i++) {
    fns.push(function () {
      return i;
    });
  }
  return fns[0]() + fns[1]() + fns[2]();
}
check('CLOSE_UPVAL', testCloseUpval(), 3);

function testSetName() {
  let fn = function myName() {
    return 1;
  };
  return fn.name;
}
warm(testSetName);
check('SET_NAME', testSetName(), 'myName');

function testGetGlobal() {
  return Math;
}
warm(testGetGlobal);
check('GET_GLOBAL', typeof testGetGlobal(), 'object');

function testGetGlobalUndef() {
  return typeof nonExistentVar123;
}
warm(testGetGlobalUndef);
check('GET_GLOBAL_UNDEF', testGetGlobalUndef(), 'undefined');

var gval = 0;
function testPutGlobal() {
  gval = 42;
  return gval;
}
warm(testPutGlobal);
check('PUT_GLOBAL', testPutGlobal(), 42);

function testAdd(a, b) {
  return a + b;
}
warm2(testAdd);
check('ADD', testAdd(20, 22), 42);
check('ADD str', testAdd('a', 'b'), 'ab');

function testSub(a, b) {
  return a - b;
}
warm2(testSub);
check('SUB', testSub(50, 8), 42);

function testMul(a, b) {
  return a * b;
}
warm2(testMul);
check('MUL', testMul(6, 7), 42);

function testDiv(a, b) {
  return a / b;
}
warm2(testDiv);
check('DIV', testDiv(84, 2), 42);

function testMod(a, b) {
  return a % b;
}
warm2(testMod);
check('MOD', testMod(47, 5), 2);

function testNeg(a) {
  return -a;
}
warm1(testNeg);
check('NEG', testNeg(42), -42);
check('NEG zero', testNeg(0), -0);

function testLt(a, b) {
  return a < b;
}
warm2(testLt);
check('LT true', testLt(1, 2), true);
check('LT false', testLt(2, 1), false);
check('LT eq', testLt(1, 1), false);

function testLe(a, b) {
  return a <= b;
}
warm2(testLe);
check('LE true', testLe(1, 1), true);
check('LE false', testLe(2, 1), false);

function testGt(a, b) {
  return a > b;
}
warm2(testGt);
check('GT true', testGt(2, 1), true);
check('GT false', testGt(1, 2), false);

function testGe(a, b) {
  return a >= b;
}
warm2(testGe);
check('GE true', testGe(1, 1), true);
check('GE false', testGe(0, 1), false);

function testEq(a, b) {
  return a == b;
}
warm2(testEq);
check('EQ', testEq(1, 1), true);
check('EQ coerce', testEq(1, '1'), true);
check('EQ null/undef', testEq(null, undefined), true);

function testNe(a, b) {
  return a != b;
}
warm2(testNe);
check('NE', testNe(1, 2), true);
check('NE false', testNe(1, 1), false);

function testSeq(a, b) {
  return a === b;
}
warm2(testSeq);
check('SEQ true', testSeq(42, 42), true);
check('SEQ false', testSeq(1, '1'), false);

function testSne(a, b) {
  return a !== b;
}
warm2(testSne);
check('SNE true', testSne(1, '1'), true);
check('SNE false', testSne(1, 1), false);

function testIsUndef(v) {
  return v === undefined;
}
warm1(testIsUndef);
check('IS_UNDEF true', testIsUndef(undefined), true);
check('IS_UNDEF false', testIsUndef(0), false);

function testIsNull(v) {
  return v === null;
}
warm1(testIsNull);
check('IS_NULL true', testIsNull(null), true);
check('IS_NULL false', testIsNull(0), false);

function Ctor() {}
function testInstanceof(v) {
  return v instanceof Ctor;
}
warm(function () {
  testInstanceof(new Ctor());
});
check('INSTANCEOF true', testInstanceof(new Ctor()), true);
check('INSTANCEOF false', testInstanceof({}), false);

function testIn(k, o) {
  return k in o;
}
warm(function () {
  testIn('x', { x: 1 });
});
check('IN true', testIn('x', { x: 1 }), true);
check('IN false', testIn('y', { x: 1 }), false);

function testBand(a, b) {
  return a & b;
}
warm2(testBand);
check('BAND', testBand(0xff, 0x0f), 15);

function testBor(a, b) {
  return a | b;
}
warm2(testBor);
check('BOR', testBor(0x0f, 0xf0), 255);

function testBxor(a, b) {
  return a ^ b;
}
warm2(testBxor);
check('BXOR', testBxor(0xff, 0x0f), 240);

function testBnot(a) {
  return ~a;
}
warm1(testBnot);
check('BNOT 0', testBnot(0), -1);
check('BNOT -1', testBnot(-1), 0);

function testShl(a, b) {
  return a << b;
}
warm2(testShl);
check('SHL', testShl(1, 4), 16);

function testShr(a, b) {
  return a >> b;
}
warm2(testShr);
check('SHR', testShr(-16, 2), -4);

function testUshr(a, b) {
  return a >>> b;
}
warm2(testUshr);
check('USHR', testUshr(-1, 28), 15);

function testNot(a) {
  return !a;
}
warm1(testNot);
check('NOT 0', testNot(0), true);
check('NOT 1', testNot(1), false);
check('NOT null', testNot(null), true);
check('NOT str', testNot('hi'), false);

function testTypeof(a) {
  return typeof a;
}
warm1(testTypeof);
check('TYPEOF num', testTypeof(1), 'number');
check('TYPEOF str', testTypeof('x'), 'string');
check('TYPEOF bool', testTypeof(true), 'boolean');
check('TYPEOF undef', testTypeof(undefined), 'undefined');
check('TYPEOF obj', testTypeof({}), 'object');
check(
  'TYPEOF fn',
  testTypeof(function () {}),
  'function'
);

function testVoid(a) {
  return void a;
}
warm1(testVoid);
check('VOID', testVoid(42), undefined);

function testDelete() {
  let o = { x: 1, y: 2 };
  delete o.x;
  return o.x;
}
warm(testDelete);
check('DELETE', testDelete(), undefined);

function testJmp(x) {
  if (x) return 1;
  else return 2;
}
warm1(testJmp);
check('JMP true', testJmp(true), 1);
check('JMP false', testJmp(false), 2);

function testJmpFalse(x) {
  if (x) {
    return 'yes';
  }
  return 'no';
}
warm1(testJmpFalse);
check('JMP_FALSE', testJmpFalse(false), 'no');
check('JMP_TRUE', testJmpFalse(true), 'yes');

function testJmp8(x) {
  return x ? 1 : 0;
}
warm1(testJmp8);
check('JMP8', testJmp8(true), 1);

function testLoop(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += i;
  return s;
}
warm1(testLoop);
check('LOOP', testLoop(10), 45);

function helper(x) {
  return x * 2;
}
function testCall(x) {
  return helper(x);
}
warm1(testCall);
check('CALL', testCall(21), 42);

function testCallMethod() {
  let o = {
    f(x) {
      return x + 1;
    }
  };
  return o.f(41);
}
warm(testCallMethod);
check('CALL_METHOD', testCallMethod(), 42);

function tailHelper(n, acc) {
  if (n <= 0) return acc;
  return tailHelper(n - 1, acc + n);
}
function testTailCall(n) {
  return tailHelper(n, 0);
}
warm(function () {
  testTailCall(10);
});
check('TAIL_CALL', testTailCall(100), 5050);

function testTailCallMethod() {
  let o = {
    count(n, acc) {
      if (n <= 0) return acc;
      return this.count(n - 1, acc + n);
    }
  };
  return o.count(10, 0);
}
warm(testTailCallMethod);
check('TAIL_CALL_METHOD', testTailCallMethod(), 55);

function Point(x, y) {
  this.x = x;
  this.y = y;
}
function testNew() {
  let p = new Point(3, 4);
  return p.x + p.y;
}
warm(testNew);
check('NEW', testNew(), 7);

function testReturn() {
  return 42;
}
warm(testReturn);
check('RETURN', testReturn(), 42);

function testReturnUndef() {
  let x = 1;
}
warm(testReturnUndef);
check('RETURN_UNDEF', testReturnUndef(), undefined);

function testField() {
  let o = {};
  o.x = 42;
  return o.x;
}
warm(testField);
check('GET/PUT_FIELD', testField(), 42);

function testGetField2() {
  let o = {
    m() {
      return 42;
    }
  };
  return o.m();
}
warm(testGetField2);
check('GET_FIELD2', testGetField2(), 42);

function testElem() {
  let a = [0];
  a[0] = 42;
  return a[0];
}
warm(testElem);
check('GET/PUT_ELEM', testElem(), 42);

function testGetElem2() {
  let o = {
    f() {
      return 42;
    }
  };
  return o['f']();
}
warm(testGetElem2);
check('GET_ELEM2', testGetElem2(), 42);

function testDefineField() {
  let o = { x: 42 };
  return o.x;
}
warm(testDefineField);
check('DEFINE_FIELD', testDefineField(), 42);

function testGetLength() {
  return [1, 2, 3].length;
}
warm(testGetLength);
check('GET_LENGTH', testGetLength(), 3);

function testObject() {
  let o = {};
  return typeof o;
}
warm(testObject);
check('OBJECT', testObject(), 'object');

function testArray() {
  let a = [1, 2, 3];
  return a[0] + a[1] + a[2];
}
warm(testArray);
check('ARRAY', testArray(), 6);

function testSetProto() {
  let p = { x: 10 };
  let c = { __proto__: p, y: 20 };
  return c.x + c.y;
}
warm(testSetProto);
check('SET_PROTO', testSetProto(), 30);

function testToPropkey() {
  let o = {};
  let k = 42;
  o[k] = 'v';
  return o['42'];
}
warm(testToPropkey);
check('TO_PROPKEY', testToPropkey(), 'v');

function testTryCatch() {
  try {
    throw 'err';
  } catch (e) {
    return e;
  }
}
warm(testTryCatch);
check('TRY/CATCH', testTryCatch(), 'err');

function testTryNoThrow() {
  try {
    return 42;
  } catch (e) {
    return -1;
  }
}
warm(testTryNoThrow);
check('TRY no throw', testTryNoThrow(), 42);

function testThrowError() {
  try {
    let x = null;
    x.foo;
    return 'miss';
  } catch (e) {
    return 'caught';
  }
}
warm(testThrowError);
check('THROW_ERROR', testThrowError(), 'caught');

function testNestedTry() {
  try {
    try {
      throw 'inner';
    } catch (e) {
      return 'got: ' + e;
    }
  } catch (e) {
    return 'outer';
  }
}
warm(testNestedTry);
check('NESTED TRY', testNestedTry(), 'got: inner');

function testRethrow() {
  try {
    try {
      throw 'a';
    } catch (e) {
      throw 'b' + e;
    }
  } catch (e) {
    return e;
  }
}
warm(testRethrow);
check('RETHROW', testRethrow(), 'ba');

function tfbNumAdd(a, b) {
  return a + b;
}
for (let i = 0; i < 200; i++) tfbNumAdd(i, i);
check('TFB num_only ADD', tfbNumAdd(20, 22), 42);

function tfbBailout(a, b) {
  return a + b;
}
for (let i = 0; i < 200; i++) tfbBailout(i, i);
check('TFB bailout', tfbBailout('x', 'y'), 'xy');
check('TFB post-bailout', tfbBailout(1, 2), 3);

function tfbStrAdd(a, b) {
  return a + b;
}
for (let i = 0; i < 200; i++) tfbStrAdd('a', 'b');
check('TFB never_num ADD', tfbStrAdd('hello', ' world'), 'hello world');

function tfbStrLt(a, b) {
  return a < b;
}
for (let i = 0; i < 200; i++) tfbStrLt('a', 'b');
check('TFB never_num LT', tfbStrLt('a', 'b'), true);
check('TFB never_num LT 2', tfbStrLt('b', 'a'), false);

function tfbMixed(a, b) {
  return a + b;
}
for (let i = 0; i < 100; i++) tfbMixed(i, i);
for (let i = 0; i < 100; i++) tfbMixed('a', 'b');
check('TFB mixed num', tfbMixed(1, 2), 3);
check('TFB mixed str', tfbMixed('x', 'y'), 'xy');

function tfbLoop(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + i;
  return s;
}
for (let i = 0; i < 200; i++) tfbLoop(10);
check('TFB loop', tfbLoop(1000), 499500);

function tfbCmpLoop(n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    if (i > 50) c++;
  }
  return c;
}
for (let i = 0; i < 200; i++) tfbCmpLoop(100);
check('TFB cmp loop', tfbCmpLoop(100), 49);

function testSpecialObj() {
  return new.target;
}
warm(testSpecialObj);
check('SPECIAL_OBJ', testSpecialObj(), undefined);

console.log('passed:', pass, 'failed:', fail, 'total:', pass + fail);
console.log('all tests passed:', fail === 0);
