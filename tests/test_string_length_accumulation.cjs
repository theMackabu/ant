function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function exerciseLocalBuilder(piece, unitsPerPiece) {
  let value = '';

  for (let i = 0; i < 8_000; i++) {
    value += piece;
    assert(value.length === (i + 1) * unitsPerPiece, 'local builder length changed');
  }

  assert(value.length === 8_000 * unitsPerPiece, 'final local builder length changed');
}

function exercisePropertyRope(piece, unitsPerPiece) {
  const holder = { value: '' };
  let snapshot = '';

  for (let i = 0; i < 8_000; i++) {
    holder.value += piece;
    assert(holder.value.length === (i + 1) * unitsPerPiece, 'property rope length changed');
    if (i === 1_999) snapshot = holder.value;
  }

  assert(snapshot.length === 2_000 * unitsPerPiece, 'property rope snapshot length changed');
  assert(holder.value.length === 8_000 * unitsPerPiece, 'final property rope length changed');
}

function exerciseLateLocalBuilder(piece, unitsPerPiece) {
  let value = '';

  for (let i = 0; i < 3_000; i++) value += piece;

  assert(value.length === 3_000 * unitsPerPiece, 'late local builder length changed');
  const materialized = value.slice(0);
  assert(materialized.length === 3_000 * unitsPerPiece, 'materialized local builder length changed');
}

function exerciseLatePropertyRope(piece, unitsPerPiece) {
  const holder = { value: '' };

  for (let i = 0; i < 3_000; i++) holder.value += piece;

  assert(holder.value.length === 3_000 * unitsPerPiece, 'late property rope length changed');
  const materialized = holder.value.slice(0);
  assert(materialized.length === 3_000 * unitsPerPiece, 'materialized property rope length changed');
}

exerciseLocalBuilder('x'.repeat(200), 200);
exerciseLocalBuilder('\u{1f600}'.repeat(100), 200);
exercisePropertyRope('x'.repeat(200), 200);
exercisePropertyRope('\u{1f600}'.repeat(100), 200);
exerciseLateLocalBuilder('x'.repeat(200), 200);
exerciseLateLocalBuilder('\u{1f600}'.repeat(100), 200);
exerciseLatePropertyRope('x'.repeat(200), 200);
exerciseLatePropertyRope('\u{1f600}'.repeat(100), 200);

const nonAsciiFlat = '\u{1f600}e\u{301}'.repeat(1_000);
for (let i = 0; i < 10_000; i++) assert(nonAsciiFlat.length === 4_000, 'cached flat string length changed');

console.log('string accumulation length: ok');
