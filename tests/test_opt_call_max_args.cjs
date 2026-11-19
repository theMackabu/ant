const assert = require('node:assert');
const N = 8000;
const sum = (...a) => { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; };
function f15(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) { return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o; }
function f16(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) { return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p; }
function f17(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q) { return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q; }
const o = {
  m15(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) { return this.z+a+b+c+d+e+f+g+h+i+j+k+l+m+n+o; },
  m16(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) { return this.z+a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p; },
  m17(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q) { return this.z+a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q; },
  z: 1000,
};
class C15 { constructor(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) { this.s = a+b+c+d+e+f+g+h+i+j+k+l+m+n+o; this.last = o; } }
class C16 { constructor(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) { this.s = a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p; this.last = p; } }
class C17 { constructor(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q) { this.s = a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q; this.last = q; } }
class D15 extends C15 { constructor(x) { super(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15); } }
class D16 extends C16 { constructor(x) { super(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16); } }
class D17 extends C17 { constructor(x) { super(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17); } }
class E15 extends C15 {}
class E16 extends C16 {}
class E17 extends C17 {}
class V { constructor(...a) { this.n = a.length; this.s = sum(...a); } }
class W16 extends V { constructor(x) { super(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16); } }
class W17 extends V { constructor(x) { super(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17); } }
function plain(x) {
  return f15(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15) + f16(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16) + f17(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
}
function method(x) {
  return o.m15(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15) + o.m16(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16) + o.m17(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
}
function construct(x) {
  const a = new C15(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15), b = new C16(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16), c = new C17(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
  return a.s + b.s + c.s + a.last + b.last + c.last;
}
function derived(x) {
  const a = new D15(x), b = new D16(x), c = new D17(x);
  return a.s + b.s + c.s + a.last + b.last + c.last;
}
function implicit(x) {
  const a = new E15(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15), b = new E16(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16), c = new E17(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
  return a.s + b.s + c.s + a.last + b.last + c.last;
}
function rest(x) {
  const a = new W16(x), b = new W17(x);
  return a.s + b.s + a.n + b.n + sum(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16) + sum(x,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
}
const base15 = 119, base16 = 135, base17 = 152;
let total = 0;
for (let i = 0; i < N; i++) {
  assert.strictEqual(plain(i), 3 * i + base15 + base16 + base17, `plain ${i}`);
  assert.strictEqual(method(i), 3000 + 3 * i + base15 + base16 + base17, `method ${i}`);
  assert.strictEqual(construct(i), 3 * i + base15 + base16 + base17 + 15 + 16 + 17, `construct ${i}`);
  assert.strictEqual(derived(i), 3 * i + base15 + base16 + base17 + 15 + 16 + 17, `derived ${i}`);
  assert.strictEqual(implicit(i), 3 * i + base15 + base16 + base17 + 15 + 16 + 17, `implicit ${i}`);
  assert.strictEqual(rest(i), 4 * i + 2 * base16 + 2 * base17 + 33, `rest ${i}`);
  total += i;
}
console.log('ok', total);
