function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

// RegExp execution reads internal flags. An own property can shadow the
// prototype getter, but it must not change the compiled matcher's flags.
const shadowedFlagsRe = /a/g;
Object.defineProperty(shadowedFlagsRe, 'flags', {
  value: '',
  writable: true,
  configurable: true,
});
assert(shadowedFlagsRe.exec('a a').index === 0, 'shadowed flags first match mismatch');
assert(shadowedFlagsRe.exec('a a').index === 2, 'shadowed flags must not disable global matching');
assert(shadowedFlagsRe.exec('a a') === null, 'shadowed flags final miss mismatch');

// The lastIndex fast location is guarded by the receiver shape. Adding or
// removing properties must recache rather than using a stale property slot.
const shapeChangingRe = /a/g;
assert(shapeChangingRe.exec('a a').index === 0, 'shape guard warmup mismatch');
shapeChangingRe.extra = 1;
shapeChangingRe.lastIndex = 2;
assert(shapeChangingRe.exec('a a').index === 2, 'shape guard after add mismatch');
delete shapeChangingRe.extra;
shapeChangingRe.lastIndex = 0;
assert(shapeChangingRe.exec('a a').index === 0, 'shape guard after delete mismatch');

// A descriptor change creates a distinct shape. The guarded fast store must
// fall back so the non-writable lastIndex error remains observable.
const readonlyLastIndexRe = /a/g;
readonlyLastIndexRe.exec('a');
readonlyLastIndexRe.lastIndex = 0;
Object.defineProperty(readonlyLastIndexRe, 'lastIndex', {
  value: 0,
  writable: false,
});
let readonlyLastIndexThrew = false;
try {
  readonlyLastIndexRe.exec('a');
} catch (error) {
  readonlyLastIndexThrew = error instanceof TypeError;
}
assert(readonlyLastIndexThrew, 'non-writable lastIndex must reject the match update');

// Compiled data is shared by pattern/flags, but differently shaped receivers
// must never share an unchecked lastIndex location.
const alternatingA = /x/g;
const alternatingB = /x/g;
alternatingA.extra = true;
for (let i = 0; i < 20; i++) {
  alternatingA.lastIndex = 0;
  alternatingB.lastIndex = 0;
  assert(alternatingA.exec('x').index === 0, 'alternating shaped receiver A mismatch');
  assert(alternatingB.exec('x').index === 0, 'alternating shaped receiver B mismatch');
}

console.log('regexp internal state semantics ok');
