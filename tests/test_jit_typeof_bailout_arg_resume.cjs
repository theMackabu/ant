let failed = 0;

function f(val, options) {
  options = options || {};
  const type = typeof val;
  if (type === 'string' && val.length > 0) {
    return 1;
  }
  if (type === 'number' && isFinite(val)) {
    return options.long ? 2 : 3;
  }
  return 0;
}

try {
  for (let i = 0; i < 300; i++) {
    const got = f(42);
    if (got !== 3) throw new Error('unexpected result at iteration ' + i + ': ' + got);
  }
} catch (e) {
  failed++;
  console.log('fail:', e && e.message ? e.message : e);
}

if (failed > 0) throw new Error('test_jit_typeof_bailout_arg_resume failed');
