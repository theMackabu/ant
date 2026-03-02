function testBand(a, b) {
  return a & b;
}
for (let i = 0; i < 110; i++) testBand(i, 0xff);
console.log('[BAND]', testBand(0xff, 0x0f), 'ok:', testBand(0xff, 0x0f) === 15);

function testBor(a, b) {
  return a | b;
}
for (let i = 0; i < 110; i++) testBor(i, 0);
console.log('[BOR]', testBor(0x0f, 0xf0), 'ok:', testBor(0x0f, 0xf0) === 255);

function testBxor(a, b) {
  return a ^ b;
}
for (let i = 0; i < 110; i++) testBxor(i, i);
console.log('[BXOR]', testBxor(0xff, 0x0f), 'ok:', testBxor(0xff, 0x0f) === 240);

function testBnot(a) {
  return ~a;
}
for (let i = 0; i < 110; i++) testBnot(i);
console.log('[BNOT]', testBnot(0), 'ok:', testBnot(0) === -1);
console.log('[BNOT]', testBnot(-1), 'ok:', testBnot(-1) === 0);

function testShl(a, b) {
  return a << b;
}
for (let i = 0; i < 110; i++) testShl(1, i % 31);
console.log('[SHL]', testShl(1, 4), 'ok:', testShl(1, 4) === 16);

function testShr(a, b) {
  return a >> b;
}
for (let i = 0; i < 110; i++) testShr(256, i % 8);
console.log('[SHR]', testShr(-16, 2), 'ok:', testShr(-16, 2) === -4);

function testUshr(a, b) {
  return a >>> b;
}
for (let i = 0; i < 110; i++) testUshr(256, i % 8);
console.log('[USHR]', testUshr(-1, 28), 'ok:', testUshr(-1, 28) === 15);

function testBandBailout(a, b) {
  return a & b;
}
for (let i = 0; i < 110; i++) testBandBailout(i, 0xff);
let bbr = testBandBailout('3', 0xff);
console.log('[BAND bailout]', bbr, 'ok:', bbr === 3);

function testNot(a) {
  return !a;
}
for (let i = 0; i < 110; i++) testNot(i);
console.log('[NOT] !0:', testNot(0), 'ok:', testNot(0) === true);
console.log('[NOT] !1:', testNot(1), 'ok:', testNot(1) === false);
console.log('[NOT] !null:', testNot(null), 'ok:', testNot(null) === true);
console.log("[NOT] !'':", testNot(''), 'ok:', testNot('') === true);
console.log("[NOT] !'hi':", testNot('hi'), 'ok:', testNot('hi') === false);

function testTypeof(a) {
  return typeof a;
}
for (let i = 0; i < 110; i++) testTypeof(i);
console.log('[TYPEOF] number:', testTypeof(42), 'ok:', testTypeof(42) === 'number');
console.log('[TYPEOF] string:', testTypeof('hi'), 'ok:', testTypeof('hi') === 'string');
console.log('[TYPEOF] boolean:', testTypeof(true), 'ok:', testTypeof(true) === 'boolean');
console.log('[TYPEOF] undefined:', testTypeof(undefined), 'ok:', testTypeof(undefined) === 'undefined');

function testGt(a, b) {
  return a > b;
}
for (let i = 0; i < 110; i++) testGt(i, 30);
console.log('[GT] 5>3:', testGt(5, 3), 'ok:', testGt(5, 3) === true);
console.log('[GT] 3>5:', testGt(3, 5), 'ok:', testGt(3, 5) === false);
console.log('[GT] 3>3:', testGt(3, 3), 'ok:', testGt(3, 3) === false);

function testGe(a, b) {
  return a >= b;
}
for (let i = 0; i < 110; i++) testGe(i, 30);
console.log('[GE] 5>=3:', testGe(5, 3), 'ok:', testGe(5, 3) === true);
console.log('[GE] 3>=5:', testGe(3, 5), 'ok:', testGe(3, 5) === false);
console.log('[GE] 3>=3:', testGe(3, 3), 'ok:', testGe(3, 3) === true);

function testNe(a, b) {
  return a != b;
}
for (let i = 0; i < 110; i++) testNe(i, 30);
console.log('[NE] 1!=2:', testNe(1, 2), 'ok:', testNe(1, 2) === true);
console.log('[NE] 1!=1:', testNe(1, 1), 'ok:', testNe(1, 1) === false);
console.log('[NE] null!=undefined:', testNe(null, undefined), 'ok:', testNe(null, undefined) === false);

function testSne(a, b) {
  return a !== b;
}
for (let i = 0; i < 110; i++) testSne(i, 30);
console.log('[SNE] 1!==2:', testSne(1, 2), 'ok:', testSne(1, 2) === true);
console.log('[SNE] 1!==1:', testSne(1, 1), 'ok:', testSne(1, 1) === false);
console.log('[SNE] null!==undefined:', testSne(null, undefined), 'ok:', testSne(null, undefined) === true);

function testDecLocal(n) {
  let count = n;
  let sum = 0;
  while (count > 0) {
    sum = sum + count;
    count--;
  }
  return sum;
}
for (let i = 0; i < 110; i++) testDecLocal(10);
let dl = testDecLocal(100);
console.log('[DEC_LOCAL]', dl, 'ok:', dl === 5050);

function Point(x, y) {
  this.x = x;
  this.y = y;
}

function testNew(a, b) {
  let p = new Point(a, b);
  return p.x + p.y;
}
for (let i = 0; i < 110; i++) testNew(i, i + 1);
console.log('[NEW]', testNew(10, 32), 'ok:', testNew(10, 32) === 42);

function testNewProto() {
  let p = new Point(3, 4);
  return p instanceof Point;
}
for (let i = 0; i < 110; i++) testNewProto();
console.log('[NEW instanceof]', testNewProto(), 'ok:', testNewProto() === true);

function countdown(n) {
  let result = 0;
  for (let i = n; i >= 1; i--) {
    if (i > 50) result = result + 2;
    else result = result + 1;
  }
  return result;
}
let cd = countdown(100);
console.log('[GT/GE loop]', cd, 'ok:', cd === 150);

function bitMix(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) {
    acc = (acc | (i & 0xff)) ^ ((i << 1) >>> 24);
  }
  return acc;
}
let bm = bitMix(1000);
console.log('[bitwise loop]', bm, 'ok:', typeof bm === 'number');

function Animal(name) {
  this.name = name;
}
function Dog(name) {
  Animal.call(this, name);
}
Dog.prototype = Object.create(Animal.prototype);
Dog.prototype.constructor = Dog;

function testInstanceof(obj) {
  return obj instanceof Animal;
}
for (let i = 0; i < 110; i++) testInstanceof(new Dog('d'));
console.log('[INSTANCEOF] Dog instanceof Animal:', testInstanceof(new Dog('Rex')), 'ok:', testInstanceof(new Dog('Rex')) === true);
console.log('[INSTANCEOF] {} instanceof Animal:', testInstanceof({}), 'ok:', testInstanceof({}) === false);
console.log('[INSTANCEOF] 42 instanceof Animal:', testInstanceof(42), 'ok:', testInstanceof(42) === false);

console.log('all ops tests done');
