const assert = require('node:assert');

const N = 8000;

class Shape { area(x) { return x + 1; } }
class Square extends Shape { area(x) { return super.area(x) + 10; } }
function viaThis(o, i) { return o.area(i); }
{
  const plain = new Shape(), derived = new Square();
  let sum = 0, expected = 0;
  for (let i = 0; i < N; i++) {
    if (i === N / 2) Shape.prototype.area = function (x) { return x + 100; };
    sum += viaThis(plain, i) + viaThis(derived, i);
    expected += i < N / 2 ? 2 * i + 12 : 2 * i + 210;
  }
  assert.strictEqual(sum, expected);
  for (let i = 0; i < N; i++) assert.strictEqual(viaThis(derived, i), i + 110);
}

function viaOwn(o, i) { return o.run(i); }
{
  const holder = { run(x) { return x + 1; }, pad: 0 };
  let sum = 0, expected = 0;
  for (let i = 0; i < N; i++) {
    if (i === N / 2) holder.run = function (x) { return x + 2; };
    sum += viaOwn(holder, i);
    expected += i + (i < N / 2 ? 1 : 2);
  }
  assert.strictEqual(sum, expected);
  for (let i = 0; i < N; i++) assert.strictEqual(viaOwn(holder, i), i + 2);
}

function viaFlip(o, i) { return o.step(i); }
{
  const steps = [];
  for (let k = 0; k < 12; k++) steps.push(new Function('x', `return x + ${k};`));
  const holder = { step: steps[0], pad: 0 };
  let sum = 0, expected = 0;
  for (let k = 0; k < 12; k++) {
    holder.step = steps[k];
    for (let i = 0; i < N; i++) {
      sum += viaFlip(holder, i);
      expected += i + k;
    }
  }
  assert.strictEqual(sum, expected);
}

class Root { tick(x) { return x; } }
class Mid extends Root { tick(x) { return super.tick(x) + 1; } }
class Tip extends Mid { tick(x) { return super.tick(x) + 1; } }
function viaSuperFlip(o, i) { return o.tick(i); }
{
  const tip = new Tip();
  let sum = 0, expected = 0;
  for (let k = 0; k < 10; k++) {
    Root.prototype.tick = k & 1 ? function (x) { return x * 2; } : function (x) { return x + k; };
    for (let i = 0; i < N; i++) {
      sum += viaSuperFlip(tip, i);
      expected += (k & 1 ? i * 2 : i + k) + 2;
    }
  }
  assert.strictEqual(sum, expected);
}

function Legacy(x) { this.x = x; }
function Modern(x) { this.x = x + 1; this.modern = true; }
function make(Ctor, i) { return new Ctor(i); }
{
  const box = { Ctor: Legacy };
  let sum = 0, expected = 0;
  for (let i = 0; i < N; i++) {
    if (i === N / 2) box.Ctor = Modern;
    const made = make(box.Ctor, i);
    sum += made.x + (made.modern ? 1000 : 0);
    expected += i < N / 2 ? i : i + 1001;
  }
  assert.strictEqual(sum, expected);
}

function callBoth(f, i) { return f(i); }
{
  const first = (x) => x + 1, second = (x) => x + 2, third = Math.abs;
  let sum = 0, expected = 0;
  for (let i = 0; i < 3 * N; i++) {
    const f = i < N ? first : i < 2 * N ? second : third;
    sum += callBoth(f, -i);
    expected += i < N ? 1 - i : i < 2 * N ? 2 - i : i;
  }
  assert.strictEqual(sum, expected);
}

console.log('ok');
