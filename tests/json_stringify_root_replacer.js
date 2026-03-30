function show(label, value) {
  const out = JSON.stringify(value, function (key, current) {
    if (key === '') {
      if (typeof current === 'function') return { root: 'function' };
      if (current === undefined) return { root: 'undefined' };
      if (typeof current === 'symbol') return { root: 'symbol' };
    }
    return current;
  });

  console.log(`${label}:${out}`);
}

show('fn', function demo() {});
show('undef', undefined);
show('sym', Symbol.for('demo'));
