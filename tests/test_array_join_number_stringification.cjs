function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(
  [2201970, 40309, 6267400].join(',') === '2201970,40309,6267400',
  `expected decimal join output, got ${[2201970, 40309, 6267400].join(',')}`
);

assert(
  [999999, 1000000, 1000001].join(',') === '999999,1000000,1000001',
  `expected plain integers around one million, got ${[999999, 1000000, 1000001].join(',')}`
);

assert(
  [2201970].toString() === '2201970',
  `expected Array#toString to use decimal formatting, got ${[2201970].toString()}`
);

assert(
  [1.5, 2201970].join(',') === '1.5,2201970',
  `expected mixed numeric join output, got ${[1.5, 2201970].join(',')}`
);

console.log('array join number stringification test passed');
