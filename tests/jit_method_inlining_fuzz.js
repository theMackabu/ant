function assertEq(actual, expected, label) {
  if (actual !== expected)
    throw new Error(label + ": expected " + expected + ", got " + actual);
}

function sharedNext(delta) {
  const old = this.item;
  this.item = old + delta;
  return old;
}

function runCase(k) {
  function Box(start) {
    if ((k & 1) === 0) {
      this.item = start;
      this.pad = k;
    } else {
      this.pad = k;
      this.item = start;
    }
    if ((k & 2) !== 0) this.more = k + 1;
  }

  Box.prototype.next = sharedNext;

  function callNext(obj, delta) {
    return obj.next(delta);
  }

  const obj = new Box(k + 1);
  const delta = (k % 4) + 1;
  for (let i = 0; i < 260; i++) callNext(obj, delta);

  const before = obj.item;

  switch (k % 5) {
    case 0: {
      obj.extra = k * 3;
      assertEq(callNext(obj, delta), before, "expando result " + k);
      assertEq(obj.item, before + delta, "expando store " + k);
      assertEq(obj.extra, k * 3, "expando preserved " + k);
      break;
    }
    case 1: {
      Box.prototype.next = function(d) {
        return this.item + d + 1000;
      };
      assertEq(callNext(obj, delta), before + delta + 1000, "prototype replacement " + k);
      assertEq(obj.item, before, "prototype replacement no old store " + k);
      break;
    }
    case 2: {
      obj.next = function(d) {
        return this.item * 10 + d;
      };
      assertEq(callNext(obj, delta), before * 10 + delta, "own shadow " + k);
      assertEq(obj.item, before, "own shadow no old store " + k);
      break;
    }
    case 3: {
      const alt = { other: k, item: 50 + k };
      Object.setPrototypeOf(alt, Box.prototype);
      assertEq(callNext(alt, delta), 50 + k, "alternate shape result " + k);
      assertEq(alt.item, 50 + k + delta, "alternate shape store " + k);
      assertEq(alt.other, k, "alternate shape preserved " + k);
      break;
    }
    default: {
      let backing = before + 5;
      Object.defineProperty(obj, "item", {
        get() { return backing; },
        set(v) { backing = v * 2; },
        configurable: true
      });
      assertEq(callNext(obj, delta), before + 5, "accessor result " + k);
      assertEq(backing, (before + 5 + delta) * 2, "accessor setter " + k);
      break;
    }
  }
}

for (let round = 0; round < 3; round++) {
  for (let k = 0; k < 40; k++) runCase(round * 40 + k);
}

console.log("jit method inlining fuzz: ok");
