const bouncer = (f, ini) => {
  let obj = ini;
  while (obj.tag) obj = f(obj);
  return obj.h0;
};

const Nil = { tag: 0 };
const Cons = (h, t) => ({ tag: 1, h1: h, h2: t });

function mkList(n) {
  let xs = Nil;
  for (let i = n; i > 0; i--) xs = Cons(i, xs);
  return xs;
}

const collectBad = xs => bouncer(collectBadREC, { tag: 1, h0: xs });
const collectBadREC = arg => {
  if (arg.h0.tag === 1) {
    return { tag: 0, h0: Cons(arg.h0.h1, collectBad(arg.h0.h2)) };
  }
  return { tag: 0, h0: Nil };
};

const collectGood = xs => reverse(bouncer(collectGoodREC, { tag: 1, h0: xs, h1: Nil }));
const collectGoodREC = arg => {
  if (arg.h0.tag === 1) return { tag: 1, h0: arg.h0.h2, h1: Cons(arg.h0.h1, arg.h1) };
  return { tag: 0, h0: arg.h1 };
};

function reverse(xs) {
  let out = Nil;
  while (xs.tag === 1) {
    out = Cons(xs.h1, out);
    xs = xs.h2;
  }
  return out;
}

const n = Number(process.argv[2] || 5000);
const xs = mkList(n);

try {
  collectBad(xs);
} catch {
  console.log('FAIL: collectBad should not overflow');
  process.exit(1);
}

collectGood(xs);
console.log('PASS');
