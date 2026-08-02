// escaped mapped-arguments objects must survive their coroutine's activation
// being destroyed while suspended (detach-on-destroy, not use-after-free)
let genLeak, asyncLeak;

function abandonGenerator() {
  function* g() { genLeak = arguments; yield 1; }
  g("gen-held", 42).next();
}

function abandonAsync() {
  async function f() { asyncLeak = arguments; await new Promise(() => {}); }
  f("async-held", 7);
}

abandonGenerator();
abandonAsync();

let junk = [];
for (let i = 0; i < 200000; i++) { junk.push({ a: i, b: [i, i + 1] }); if (junk.length > 1024) junk = []; }

let pass = 0, fail = 0;
function check(name, got, want) {
  if (got === want) pass++;
  else { fail++; console.log(`FAIL ${name}: got ${got}, want ${want}`); }
}

check("gen[0]", genLeak[0], "gen-held");
check("gen[1]", genLeak[1], 42);
check("gen.length", genLeak.length, 2);
genLeak[0] = "rewritten";
check("gen write", genLeak[0], "rewritten");

check("async[0]", asyncLeak[0], "async-held");
check("async[1]", asyncLeak[1], 7);


// escaped closures over a dead suspended coroutine's locals must read the
// frozen values through their (closed) upvalues, not the freed snapshot
let closureLeak;
function abandonWithClosure() {
  function* g() { let x = 1234; closureLeak = () => x; yield 1; x = 9999; }
  g().next();
}
abandonWithClosure();

junk = [];
for (let i = 0; i < 200000; i++) { junk.push({ a: i, b: [i, i + 1] }); if (junk.length > 1024) junk = []; }

check("closure upvalue", closureLeak(), 1234);

console.log(`passed: ${pass}, failed: ${fail}`);
if (fail) throw new Error("escaped state regression");
