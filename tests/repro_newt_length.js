// Reproduces a bug where Prelude_length$27 (while-loop length) returns 0
// for a 4-element linked list under ant after GC pressure.
//
// In the full newt bootstrap, lvl2ix calls length(ctx.env) and gets 0 back
// instead of 4, producing a negative de Bruijn index (-3 instead of 1),
// which cascades into "Missing case for Prelude.MkEq".
//
// The bug does not reproduce in isolation without GC pressure.

const Nil = () => ({ tag: 0 });
const Cons = (h1, h2) => ({ tag: 1, h1: h1, h2: h2 });

// While-loop length (Prelude_length$27 from newt bootstrap)
const length_go = (_, xs, acc) => {
  while (1) {
    let xs1 = xs;
    let acc1 = acc;
    if ((xs1.tag) == (1)) {
      xs = xs1.h2;
      acc = (acc1) + (1);
      continue;
    } else {
      return acc1;
    }
  }
};
const length_prime = (xs) => length_go(xs, xs, 0);

// lvl2ix from newt: l - k - 1
const lvl2ix = (l, k) => l - k - 1;

// Build a VVar-like object (matches newt's object shape)
const VVar = (h0, h1, h2) => ({ tag: 0, h0: h0, h1: h1, h2: h2 });
const emptyFC = { tag: 0, h0: "", h1: { tag: 0, h0: 0, h1: 0, h2: 0, h3: 0 } };
const Lin = () => ({ tag: 0 });

// Simulates what quoteAlt_mkenv does: builds an env list with VVar nodes
function mkenv(lvl, env, names) {
  while (1) {
    if ((names.tag) == (1)) {
      let newVar = VVar(emptyFC, lvl, Lin());
      lvl = lvl + 1;
      env = Cons(newVar, env);
      names = names.h2;
      continue;
    } else {
      return env;
    }
  }
}

// Simulates the eval/quote cycle that builds up env lists
function simulateEval(env, depth) {
  if (depth <= 0) return env;

  // Build up environment like newt's eval does
  let fresh = VVar(emptyFC, length_prime(env), Lin());
  let newEnv = Cons(fresh, env);

  // Allocate throwaway objects to create GC pressure
  for (let i = 0; i < 100; i++) {
    let junk = { tag: 5, h0: emptyFC, h1: i, h2: { tag: 0 }, h3: "x", h4: { tag: 0, h0: env, h1: { tag: 0 } } };
  }

  return simulateEval(newEnv, depth - 1);
}

let fail = false;

// Run many iterations to trigger GC
for (let round = 0; round < 500; round++) {
  // Start with a 4-element env (matching the failing case)
  let env = Cons(VVar(emptyFC, 3, Lin()),
            Cons(VVar(emptyFC, 2, Lin()),
            Cons(VVar(emptyFC, 1, Lin()),
            Cons(VVar(emptyFC, 0, Lin()),
            Nil()))));

  let len = length_prime(env);
  if (len !== 4) {
    console.log("FAIL round " + round + ": length(env) = " + len + ", expected 4");
    console.log("  lvl2ix would give: " + lvl2ix(len, 1) + " (expected 2, got " + lvl2ix(len, 1) + ")");
    fail = true;
    break;
  }

  // Simulate eval growing the environment
  let bigEnv = simulateEval(env, 20);
  let bigLen = length_prime(bigEnv);
  if (bigLen !== 24) {
    console.log("FAIL round " + round + ": length(bigEnv) = " + bigLen + ", expected 24");
    fail = true;
    break;
  }

  // mkenv path (like quoteAlt_mkenv)
  let names = Cons("a", Cons("b", Cons("c", Nil())));
  let extEnv = mkenv(length_prime(env), env, names);
  let extLen = length_prime(extEnv);
  if (extLen !== 7) {
    console.log("FAIL round " + round + ": length(extEnv) = " + extLen + ", expected 7");
    fail = true;
    break;
  }
}

if (!fail) {
  console.log("PASS");
} else {
  process.exit(1);
}
