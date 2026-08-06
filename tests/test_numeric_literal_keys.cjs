// Regression: numeric literal property keys were formatted with %g, so
// keys with >=7 significant digits became exponent-form strings
// ({123456789: 1} got key "1.23457e+08"). Keys must use JS ToString.
let failures = 0;
function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) {
    console.log(`FAIL ${name}: got ${a}, want ${e}`);
    failures++;
  } else {
    console.log(`ok ${name}`);
  }
}

// shaped-site path (all-static keys, qualifies for shape boilerplating)
const shaped = { 123456789: 1, 1.5: 2, 1e21: 3, 0.0001: 4, 10: 5, 4294967296: 6 };
check('shaped keys', Object.keys(shaped),
  ['10', '123456789', '1.5', '1e+21', '0.0001', '4294967296']);
check('shaped lookup', [shaped[123456789], shaped['123456789'], shaped['1e+21']], [1, 1, 3]);

// DEFINE_FIELD fallback path (spread disqualifies the shaped site)
const fallback = { ...{}, 123456789: 7, 0.5: 8, 1234567.891: 9 };
check('fallback keys', Object.keys(fallback), ['123456789', '0.5', '1234567.891']);
check('fallback lookup', fallback['1234567.891'], 9);

// class field / single-key path
const single = { 987654321: 'x' };
check('single key', Object.keys(single), ['987654321']);

// precision extremes
const extremes = { 9007199254740993: 1, 1e300: 2 };
check('extreme keys', Object.keys(extremes), ['9007199254740992', '1e+300']);

if (failures) {
  console.log(`${failures} failures`);
  process.exit(1);
}
console.log('all numeric-literal-key tests passed');
