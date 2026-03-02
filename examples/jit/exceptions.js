function test1() {
  try {
    throw 'error';
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test1();
console.log('[test1] throw string:', test1(), 'ok:', test1() === 'error');

function test2() {
  try {
    return 42;
  } catch (e) {
    return -1;
  }
}
for (let i = 0; i < 110; i++) test2();
console.log('[test2] no throw:', test2(), 'ok:', test2() === 42);

function test3() {
  try {
    throw 99;
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test3();
console.log('[test3] throw number:', test3(), 'ok:', test3() === 99);

function thrower() {
  throw 'boom';
}
function test4() {
  try {
    return thrower();
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test4();
console.log('[test4] catch from call:', test4(), 'ok:', test4() === 'boom');

function test5() {
  let result = 0;
  try {
    throw 'err';
  } catch (e) {
    result = 10;
  }
  return result + 5;
}
for (let i = 0; i < 110; i++) test5();
console.log('[test5] code after catch:', test5(), 'ok:', test5() === 15);

function test6() {
  try {
    try {
      throw 'inner';
    } catch (e) {
      return 'caught: ' + e;
    }
  } catch (e) {
    return 'outer: ' + e;
  }
}
for (let i = 0; i < 110; i++) test6();
console.log('[test6] nested catch:', test6(), 'ok:', test6() === 'caught: inner');

function test7() {
  try {
    try {
      throw 'propagate';
    } catch (e) {
      throw 're: ' + e;
    }
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test7();
console.log('[test7] re-throw:', test7(), 'ok:', test7() === 're: propagate');

function test8(x) {
  try {
    let a = x + 1;
    let b = a * 2;
    return b;
  } catch (e) {
    return -1;
  }
}
for (let i = 0; i < 110; i++) test8(i);
console.log('[test8] complex try body:', test8(5), 'ok:', test8(5) === 12);

function test9() {
  try {
    throw 10;
  } catch (e) {
    return e + 32;
  }
}
for (let i = 0; i < 110; i++) test9();
console.log('[test9] catch computation:', test9(), 'ok:', test9() === 42);

function test10(doThrow) {
  try {
    if (doThrow) throw 'thrown';
    return 'ok';
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test10(i % 2 === 0);
console.log('[test10a] throw path:', test10(true), 'ok:', test10(true) === 'thrown');
console.log('[test10b] no-throw path:', test10(false), 'ok:', test10(false) === 'ok');

function test11() {
  try {
    throw true;
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test11();
console.log('[test11] throw bool:', test11(), 'ok:', test11() === true);

function safe() {
  return 1;
}
function unsafe() {
  throw 'fail';
}
function test12() {
  try {
    let a = safe();
    let b = safe();
    let c = unsafe();
    return a + b + c;
  } catch (e) {
    return e;
  }
}
for (let i = 0; i < 110; i++) test12();
console.log('[test12] multi-call catch:', test12(), 'ok:', test12() === 'fail');

function test13() {
  try {
    let x = undefined;
    x.foo;
  } catch (e) {
    return 'caught';
  }
  return 'missed';
}
for (let i = 0; i < 110; i++) test13();
console.log('[test13] throw_error:', test13(), 'ok:', test13() === 'caught');

function test14() {
  let obj = {
    greet() {
      return 'hello';
    }
  };
  return obj.greet();
}
for (let i = 0; i < 110; i++) test14();
console.log('[test14] get_field2:', test14(), 'ok:', test14() === 'hello');

function test15() {
  let obj = {
    say() {
      return 'hi';
    }
  };
  let key = 'say';
  return obj[key]();
}
for (let i = 0; i < 110; i++) test15();
console.log('[test15] get_elem2:', test15(), 'ok:', test15() === 'hi');

function test16() {
  let base = { x: 10 };
  let child = { __proto__: base, y: 20 };
  return child.x + child.y;
}
for (let i = 0; i < 110; i++) test16();
console.log('[test16] set_proto:', test16(), 'ok:', test16() === 30);

function test17() {
  try {
    let x = null;
    return x.foo;
  } catch (e) {
    return 'caught';
  }
}
for (let i = 0; i < 110; i++) test17();
console.log('[test17] get_field err:', test17(), 'ok:', test17() === 'caught');

function test18() {
  try {
    let x = undefined;
    return x[0];
  } catch (e) {
    return 'caught';
  }
}
for (let i = 0; i < 110; i++) test18();
console.log('[test18] get_elem err:', test18(), 'ok:', test18() === 'caught');

function test19() {
  let arr = [1, 2, 3];
  return arr['join']('-');
}
for (let i = 0; i < 110; i++) test19();
console.log('[test19] elem2 arr:', test19(), 'ok:', test19() === '1-2-3');

function test20() {
  let obj = {
    val: 0,
    add(n) {
      return {
        val: this.val + n,
        add: this.add,
        get() {
          return this.val;
        }
      };
    },
    get() {
      return this.val;
    }
  };
  return obj.add(1).add(2).get();
}
for (let i = 0; i < 110; i++) test20();
console.log('[test20] chain calls:', test20(), 'ok:', test20() === 3);
