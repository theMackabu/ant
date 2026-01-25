// ============ Array.prototype.some ============
console.log('=== some ===');
console.log([1, 2, 3].some(x => x > 2) === true);
console.log([1, 2, 3].some(x => x > 5) === false);
console.log([].some(x => true) === false);

// ============ Array.prototype.every ============
console.log('=== every ===');
console.log([1, 2, 3].every(x => x > 0) === true);
console.log([1, 2, 3].every(x => x > 2) === false);
console.log([].every(x => false) === true);

// ============ Array.prototype.forEach ============
console.log('=== forEach ===');
let sum = 0;
[1, 2, 3].forEach(x => (sum += x));
console.log(sum === 6);

// ============ Array.prototype.map ============
console.log('=== map ===');
let mapped = [1, 2, 3].map(x => x * 2);
console.log(mapped[0] === 2, mapped[1] === 4, mapped[2] === 6);

// ============ Array.prototype.filter ============
console.log('=== filter ===');
let filtered = [1, 2, 3, 4].filter(x => x % 2 === 0);
console.log(filtered.length === 2, filtered[0] === 2, filtered[1] === 4);

// ============ Array.prototype.find ============
console.log('=== find ===');
console.log([1, 2, 3].find(x => x > 1) === 2);
console.log([1, 2, 3].find(x => x > 5) === undefined);

// ============ Array.prototype.findIndex ============
console.log('=== findIndex ===');
console.log([1, 2, 3].findIndex(x => x > 1) === 1);
console.log([1, 2, 3].findIndex(x => x > 5) === -1);

// ============ Array.prototype.findLast ============
console.log('=== findLast ===');
console.log([1, 2, 3, 2].findLast(x => x === 2) === 2);
console.log([1, 2, 3].findLast(x => x > 5) === undefined);

// ============ Array.prototype.findLastIndex ============
console.log('=== findLastIndex ===');
console.log([1, 2, 3, 2].findLastIndex(x => x === 2) === 3);
console.log([1, 2, 3].findLastIndex(x => x > 5) === -1);

// ============ Array.prototype.reduce ============
console.log('=== reduce ===');
console.log([1, 2, 3].reduce((a, b) => a + b) === 6);
console.log([1, 2, 3].reduce((a, b) => a + b, 10) === 16);

// ============ Array.prototype.includes ============
console.log('=== includes ===');
console.log([1, 2, 3].includes(2) === true);
console.log([1, 2, 3].includes(5) === false);
console.log(['a', 'b'].includes('a') === true);

// ============ Array.prototype.sort ============
console.log('=== sort ===');
let sorted1 = [3, 1, 2].sort();
console.log(sorted1[0] === 1, sorted1[1] === 2, sorted1[2] === 3);

let sorted2 = [3, 1, 2].sort((a, b) => b - a);
console.log(sorted2[0] === 3, sorted2[1] === 2, sorted2[2] === 1);

let sorted3 = ['z', null, undefined, 'a'].sort();
console.log(sorted3[0] === 'a', sorted3[1] === null, sorted3[2] === 'z', sorted3[3] === undefined);

// ============ Array.prototype.reverse ============
console.log('=== reverse ===');
let rev = [1, 2, 3];
rev.reverse();
console.log(rev[0] === 3, rev[1] === 2, rev[2] === 1);

// ============ Array.prototype.toSorted ============
console.log('=== toSorted ===');
let orig1 = [3, 1, 2];
let toSorted1 = orig1.toSorted();
console.log(orig1[0] === 3); // original unchanged
console.log(toSorted1[0] === 1, toSorted1[1] === 2, toSorted1[2] === 3);

// ============ Array.prototype.toReversed ============
console.log('=== toReversed ===');
let orig2 = [1, 2, 3];
let toReversed1 = orig2.toReversed();
console.log(orig2[0] === 1); // original unchanged
console.log(toReversed1[0] === 3, toReversed1[1] === 2, toReversed1[2] === 1);

// ============ Array.prototype.toSpliced ============
console.log('=== toSpliced ===');
let orig3 = [1, 2, 3, 4];
let toSpliced1 = orig3.toSpliced(1, 2, 'a', 'b');
console.log(orig3.length === 4); // original unchanged
console.log(toSpliced1.length === 4);
console.log(toSpliced1[0] === 1, toSpliced1[1] === 'a', toSpliced1[2] === 'b', toSpliced1[3] === 4);

// ============ Array.prototype.flat ============
console.log('=== flat ===');
let flat1 = [1, [2, 3], [4, [5]]].flat();
console.log(flat1.length === 5);
console.log(flat1[0] === 1, flat1[1] === 2, flat1[2] === 3, flat1[3] === 4);

let flat2 = [1, [2, [3, [4]]]].flat(2);
console.log(flat2.length === 4);

// ============ Array.prototype.indexOf ============
console.log('=== indexOf ===');
console.log([1, 2, 3, 2].indexOf(2) === 1);
console.log([1, 2, 3].indexOf(5) === -1);

// ============ Array.prototype.lastIndexOf ============
console.log('=== lastIndexOf ===');
console.log([1, 2, 3, 2].lastIndexOf(2) === 3);
console.log([1, 2, 3].lastIndexOf(5) === -1);

// ============ Sparse array handling ============
console.log('=== sparse arrays ===');
let sparse = [1, , 3];
console.log(sparse.some(x => x === undefined) === false); // holes skipped
console.log(sparse.filter(x => true).length === 2); // holes skipped

// ============ Index correctness ============
console.log('=== index correctness ===');
let indices = [];
[10, 20, 30].forEach((v, i) => indices.push(i));
console.log(indices[0] === 0, indices[1] === 1, indices[2] === 2);

// ============ Array reference in callback ============
console.log('=== array reference ===');
console.log([1, 2, 3].every((v, i, arr) => arr.length === 3) === true);

console.log('=== ALL TESTS COMPLETE ===');
