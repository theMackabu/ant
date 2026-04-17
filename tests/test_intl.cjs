function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(typeof Intl === 'object', 'expected Intl global');
assert(Intl.constructor === Object, 'expected Intl to inherit from Object');

const collator = Intl.Collator('de-DE');
assert(collator instanceof Intl.Collator, 'expected Intl.Collator() to create an instance');
assert(typeof collator.compare === 'function', 'expected collator.compare');
assert(typeof collator.resolvedOptions === 'function', 'expected collator.resolvedOptions');

const numberFormat = new Intl.NumberFormat('en-US');
assert(numberFormat instanceof Intl.NumberFormat, 'expected NumberFormat instance');
assert(numberFormat.format(1234567.5) === '1,234,567.5', `unexpected formatted number: ${numberFormat.format(1234567.5)}`);

const dateTimeFormat = Intl.DateTimeFormat('en-US', { timeZone: 'Australia/Sydney' });
assert(dateTimeFormat instanceof Intl.DateTimeFormat, 'expected DateTimeFormat() to create an instance');
const dtfOptions = dateTimeFormat.resolvedOptions();
assert(dtfOptions.timeZone === 'Australia/Sydney', `unexpected timeZone: ${dtfOptions.timeZone}`);
const dtfFormatted = dateTimeFormat.format(0);
const dtfParts = dateTimeFormat.formatToParts(0);
assert(Array.isArray(dtfParts), 'expected formatToParts() to return an array');
assert(dtfParts.length === 7, `unexpected formatToParts() length: ${dtfParts.length}`);
assert(dtfParts.map(part => part.value).join('') === dtfFormatted, 'expected formatToParts() values to match format() output');
assert(dtfParts[0].type === 'hour', `unexpected first part type: ${dtfParts[0].type}`);
assert(dtfParts[6].type === 'dayPeriod', `unexpected last part type: ${dtfParts[6].type}`);

let rejected = false;
try {
  Intl.Collator('x-en-US-12345');
} catch (error) {
  rejected = true;
}
assert(rejected, 'expected invalid language tags to throw');

const segmenter = Intl.Segmenter('en-US', { granularity: 'word' });
const segments = segmenter.segment('ok');
assert(Array.isArray(segments), 'expected Intl.Segmenter().segment() to return an array');
assert(segments.length === 2, `unexpected segment count: ${segments.length}`);

console.log('intl test passed');
