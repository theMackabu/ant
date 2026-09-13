function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

// Exercise bounded search, repeated prefixes, embedded NUL and UTF-8 bytes.
for (const needle of ['x', 'xyz', 'aaaaab', '\0', 'é']) {
  for (const subject of ['', 'a'.repeat(8192), 'a'.repeat(8192) + needle, needle + 'q' + needle]) {
    const rx = new RegExp(needle, 'g');
    same(subject.replace(rx, '!'), subject.split(needle).join('!'), 'literal replacement');
    same(subject.search(rx), subject.indexOf(needle), 'literal search');
  }
}

// A warm cached descriptor must not survive a lastIndex attribute change.
for (const pattern of ['x', '[x]']) {
  const rx = new RegExp(pattern, 'g');
  same('x x'.replace(rx, '!'), '! !', 'warm replacement');
  Object.defineProperty(rx, 'lastIndex', {writable: false});
  let threw = false;
  try { 'x x'.replace(rx, '!'); } catch (error) { threw = error instanceof TypeError; }
  same(threw, true, 'readonly lastIndex');
}

const rx = /xyz/g;
rx.exec = rx.exec;
same('xyz xyz'.replace(rx, '!'), '! !', 'own builtin exec');
let calls = 0;
rx.exec = function () { calls++; return null; };
same('xyz'.replace(rx, '!'), 'xyz', 'custom exec result');
same(calls, 1, 'custom exec called');
same('é😀xyz'.search(/xyz/), 3, 'search uses UTF-16 offsets');
// Rejected probes must not perform observable exec reads.
for (const pattern of ['x', '[x]']) {
  for (const operation of ['replace', 'search']) {
    const re = new RegExp(pattern);
    let reads = 0;
    Object.defineProperty(re, 'exec', {get() {
      reads++;
      return function () { return null; };
    }});
    if (operation === 'replace') same('x'.replace(re, '!'), 'x', 'getter replace');
    else same('x'.search(re), -1, 'getter search');
    same(reads, 1, 'exec getter read exactly once');
  }
}
console.log('literal search guards ok');
