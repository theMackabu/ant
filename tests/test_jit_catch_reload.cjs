const assert = require('node:assert');

const hotCases = {
  breakFinallyCaptured: (k) => { const L = []; let g; for (let b = 0; b < 3; b++) { try { let v = 70; g = () => v; if (k) break; } finally { L.push('f'); } L.push('a'); } return L.join(',') + g(); },
  continueLabelFinallyCaptured: (k) => { const L = []; let g; outer: for (let a = 0; a < 2; a++) { for (let b = 0; b < 3; b++) { try { let v = 70 + b; g = () => v; if (k) continue outer; } finally { L.push('f'); } L.push('a'); } } return L.join(',') + g(); },
  breakBlockFinallyCaptured: (k) => { const L = []; let g; block: { try { let v = 70; g = () => v; if (k) break block; } finally { L.push('f'); } L.push('a'); } return L.join(',') + g(); },
  breakFinallyArguments: function (k) { const L = []; let g; for (let b = 0; b < 3; b++) { try { let v = 70; g = () => v; if (k) break; } finally { L.push('f' + arguments.length); } L.push('a'); } return L.join(',') + g(); },
  breakCatchFinallyCaptured: (k) => { const L = []; let g; for (let b = 0; b < 3; b++) { try { let v = 70; g = () => v; if (k) break; } catch (e) { L.push('c'); } finally { L.push('f'); } L.push('a'); } return L.join(',') + g(); },
  breakFromCatchFinally: (k) => { const L = []; let g = () => 0; for (let b = 0; b < 3; b++) { try { if (k) throw b; } catch (e) { g = () => e; if (k) break; } finally { L.push('f'); } L.push('a'); } return L.join(',') + g(); },
  returnFinallyCaptured: (k) => { const L = []; let g; const run = () => { for (let b = 0; b < 3; b++) { try { let v = 70; g = () => v; if (k) return 'r'; } finally { L.push('f'); } L.push('a'); } return 'e'; }; return run() + L.join(',') + g(); },
  nestedFinallyBreak: (k) => { const L = []; let g, h; for (let b = 0; b < 2; b++) { try { let v = b; g = () => v; try { let w = 9; h = () => w; if (k) break; } finally { L.push('i'); } } finally { L.push('o'); } L.push('a'); } return L.join(',') + g() + h(); },
  getterNoThrowReadsWrite: (k) => { let v = 10; const g = () => v += 1; const o = { get x() { g(); return k; } }; const r = o.x; return 'V' + v + r; },
};

const hotExpected = {
  breakFinallyCaptured: ["f,a,f,a,f,a70", "f70"],
  continueLabelFinallyCaptured: ["f,a,f,a,f,a,f,a,f,a,f,a72", "f,f70"],
  breakBlockFinallyCaptured: ["f,a70", "f70"],
  breakFinallyArguments: ["f1,a,f1,a,f1,a70", "f170"],
  breakCatchFinallyCaptured: ["f,a,f,a,f,a70", "f70"],
  breakFromCatchFinally: ["f,a,f,a,f,a0", "f0"],
  returnFinallyCaptured: ["ef,a,f,a,f,a70", "rf70"],
  nestedFinallyBreak: ["i,o,a,i,o,a19", "i,o09"],
  getterNoThrowReadsWrite: ["V110", "V111"],
};

for (const [name, run] of Object.entries(hotCases)) {
  for (let i = 0; i < 600; i++) {
    for (let k = 0; k < 2; k++) assert.strictEqual(run(k), hotExpected[name][k], `${name} k=${k} iteration ${i}`);
  }
}
assert.strictEqual(Object.keys(hotExpected).length, Object.keys(hotCases).length);

console.log('ok');
