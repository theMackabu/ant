const now = () => (typeof performance !== 'undefined' && typeof performance.now === 'function' ? performance.now() : Date.now());

if (typeof BigInt !== 'function') {
  console.log('BigInt is not available in this runtime.');
} else {
  console.log('BigInt Bench (new feature coverage)');

  let sinkBig = 0n;
  let sinkNum = 0;
  const HASH_BIAS = 11400714819323198485n;

  const mixBig = x => {
    sinkBig ^= x + HASH_BIAS;
  };

  const mixNum = x => {
    sinkNum = (sinkNum * 1664525 + (x >>> 0) + 1013904223) >>> 0;
  };

  function bench(name, iterations, fn) {
    const start = now();
    for (let i = 0; i < iterations; i++) fn(i);
    const ms = now() - start;
    const opsPerSec = ms > 0 ? Math.round((iterations * 1000) / ms) : 0;
    console.log(`${name}: ${ms.toFixed(2)}ms (${opsPerSec} ops/s)`);
  }

  const prefHex = BigInt('0xff');
  const prefBin = BigInt('0b1010101010101010');
  const prefOct = BigInt('0o777777');
  const trimmed = BigInt('   123456789   ');
  const literalSeed = 0xffn + 0b1010n + 0o77n + trimmed;

  const a0 = (1n << 260n) + (1n << 129n) + 12345678901234567890123n + literalSeed;
  const b0 = (1n << 192n) + (1n << 67n) + 987654321123456789n + prefHex + prefBin;
  const c0 = (1n << 73n) + 12345n + prefOct;

  bench('parse prefixed strings x120000', 120000, i => {
    const v = BigInt('0xff') + BigInt('0b1011') + BigInt('0o77') + BigInt('  42  ') + BigInt('');
    mixBig(v + BigInt(i & 31));
  });

  bench('prefixed literal arith x180000', 180000, i => {
    const v = (0xffn << 12n) + 0b101010n + 0o777n + BigInt(i);
    mixBig(v ^ literalSeed);
  });

  bench('add/sub x300000', 300000, i => {
    const ai = a0 + BigInt(i);
    const bi = b0 - BigInt(i);
    mixBig(ai + bi - c0);
  });

  bench('mul x70000', 70000, i => {
    const x = (a0 + BigInt(i)) * (c0 + BigInt((i & 31) + 1));
    mixBig(x >> 11n);
  });

  bench('div/mod x70000', 70000, i => {
    const x = (a0 << 17n) + BigInt(i);
    const q = x / c0;
    const r = x % c0;
    mixBig(q ^ r);
  });

  bench('shift x250000', 250000, i => {
    const s1 = BigInt(i & 63);
    const s2 = BigInt((i + 7) & 63);
    const x = (a0 << s1) >> s2;
    mixBig(x);
  });

  bench('toString(10) x4000', 4000, i => {
    const s = (a0 + BigInt(i)).toString(10);
    mixNum(s.length + s.charCodeAt(0));
  });

  bench('toString(16) x6000', 6000, i => {
    const s = (b0 + BigInt(i)).toString(16);
    mixNum(s.length + s.charCodeAt(s.length - 1));
  });

  bench('toString(2) x1200', 1200, i => {
    const s = (c0 << BigInt((i & 255) + 128)).toString(2);
    mixNum(s.length);
  });

  bench('asIntN/asUintN x250000', 250000, i => {
    const x = a0 + BigInt(i);
    const y = BigInt.asIntN(127, x);
    const z = BigInt.asUintN(127, -x);
    mixBig(y ^ z);
  });

  console.log('sinkBig:', sinkBig.toString(16));
  console.log('sinkNum:', sinkNum);
}
