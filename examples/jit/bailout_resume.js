const coercible = {
  valueOf() {
    return 7;
  },
};

function nestedForOf() {
  let total = 0;
  for (const outer of [10, 20, 30]) {
    let inner = 0;
    for (let i = 0; i < 700; i++) {
      inner += outer === 10 && i === 600 ? coercible : 1;
    }
    total += outer + inner;
  }
  return total;
}

function nestedIndexed() {
  const outers = [10, 20, 30];
  let total = 0;
  for (let k = 0; k < outers.length; k++) {
    const outer = outers[k];
    let inner = 0;
    for (let i = 0; i < 700; i++) {
      inner += outer === 10 && i === 600 ? coercible : 1;
    }
    total += outer + inner;
  }
  return total;
}

function perOuter() {
  const parts = [];
  for (const outer of [10, 20, 30]) {
    let inner = 0;
    for (let i = 0; i < 700; i++) {
      inner += outer === 10 && i === 600 ? coercible : 1;
    }
    parts.push(`${outer}+${inner}`);
  }
  return parts.join(',');
}

const expected = 10 + 706 + 20 + 700 + 30 + 700;
const expectedParts = '10+706,20+700,30+700';

let r1 = nestedForOf();
console.log(`[test1] nested for-of, bailout at i=600:   result=${r1}  ok=${r1 === expected}`);

let r2 = nestedIndexed();
console.log(`[test2] nested indexed, bailout at i=600:  result=${r2}  ok=${r2 === expected}`);

let r3 = perOuter();
console.log(`[test3] per-outer inner totals:            ${r3}  ok=${r3 === expectedParts}`);

console.log('all bailout resume tests passed:', r1 === expected && r2 === expected && r3 === expectedParts);
