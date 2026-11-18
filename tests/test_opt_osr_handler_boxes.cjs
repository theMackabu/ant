const assert = require('node:assert');

const handlerLoopCases = {
  forLetCatchLoop(k) {
    const fs = [];
    for (let v = 1; v < 4; v++) {
      fs.push(() => v);
      try {
        if (k) thrower.g;
      } catch (e) {
        let n = 0;
        while (n++ < 2) {}
      }
    }
    return fs.map(f => f()).join();
  },
  forOfCatchLoopWrites(k) {
    const fs = [];
    for (let v of [1, 2, 3]) {
      fs.push(() => v, () => v += 10);
      try {
        if (k) throw k;
      } catch (e) {
        for (let n = 0; n < 2; n++) fs[fs.length - 1]();
      }
    }
    return fs.map(f => f()).join();
  },
  nestedCatchLoop(k) {
    const fs = [];
    for (let v = 1; v < 4; v++) {
      let w = v * 2;
      fs.push(() => v + w);
      try {
        try {
          if (k) thrower.g;
        } finally {
          w++;
        }
      } catch (e) {
        try {
          throw e;
        } catch (inner) {
          let n = 0;
          do { n++; } while (n < 2);
        }
      }
    }
    return fs.map(f => f()).join();
  },
};
const thrower = { get g() { throw new Error('g'); } };
const handlerLoopExpected = {
  forLetCatchLoop: ['1,2,3', '1,2,3'],
  forOfCatchLoopWrites: ['1,11,2,12,3,13', '21,31,22,32,23,33'],
  nestedCatchLoop: ['4,7,10', '4,7,10'],
};
for (const [name, run] of Object.entries(handlerLoopCases)) {
  for (let i = 0; i < 600; i++) {
    for (let k = 0; k < 2; k++) assert.strictEqual(run(k), handlerLoopExpected[name][k], `${name} k=${k} iteration ${i}`);
  }
}

console.log('ok');
