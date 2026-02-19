class Foo {
  constructor() { this.x = 42; }
  bar() { return this.x; }
}

const foo = new Foo();

console.log("A:", foo.bar());

const f = foo;
console.log("B:", f.bar());

function test1(obj) { return obj.bar(); }
console.log("C:", test1(foo));

function test2(obj) { const o = obj; return o.bar(); }
console.log("D:", test2(foo));

function test3(z) { return z.bar(); }
console.log("E:", test3(foo));

function test4(obj) { return obj.x; }
console.log("F:", test4(foo));

function test5(obj) { return obj.bar(); }
console.log("G:", test5({x: 99, bar() { return this.x; }}));
