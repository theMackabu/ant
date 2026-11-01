// normalize() normalizes the well-formed text around lone surrogates instead
// of returning the string unchanged. Found by tests/differential (string
// family). Expected values match V8.
function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + JSON.stringify(actual) + ' !== ' + JSON.stringify(expected));
}

same('ǅ\ud83d'.normalize('NFKC'), 'Dž\ud83d', 'trailing high surrogate');
same('\ud83dǅ'.normalize('NFKD'), '\ud83dDž', 'leading high surrogate');
same('a\ud800ǅ\udfffé'.normalize('NFC'), 'a\ud800ǅ\udfffé', 'surrogates between runs');
same('e\ud800́'.normalize('NFC'), 'e\ud800́', 'no composition across a surrogate');
same('\ud800'.normalize(), '\ud800', 'only a surrogate');
same('😀ǅ'.normalize('NFKC'), '\u{1F600}Dž', 'well-formed pair');

console.log('normalize with surrogates ok');
