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
let closureLeak, objectClosureLeak, functionClosureLeak;
function abandonWithClosure() {
  function* g() {
    let x = 1234;
    let object = { marker: "object-live", nested: [7, 8] };
    let callable = () => "function-live";
    closureLeak = () => x;
    objectClosureLeak = () => object;
    functionClosureLeak = () => callable;
    yield 1;
    x = 9999;
  }
  g().next();
}
abandonWithClosure();

junk = [];
for (let i = 0; i < 200000; i++) { junk.push({ a: i, b: [i, i + 1] }); if (junk.length > 1024) junk = []; }

check("closure upvalue", closureLeak(), 1234);
check("object upvalue", objectClosureLeak().marker, "object-live");
check("object upvalue child", objectClosureLeak().nested[1], 8);
check("function upvalue", functionClosureLeak()(), "function-live");

// An old closure and upvalue can be hidden behind an old object while its
// activation is suspended. Writing through that open cell, and capturing it
// again after resume, must both remember an old-to-young edge for the minor GC.
const suspendedHolder = { read: null, write: null };
let suspendedGenerator;
function* resuspendWithYoungValue() {
  let value = { marker: "before" };
  suspendedHolder.read = () => value;
  suspendedHolder.write = (next) => { value = next; };
  yield 1;
  value = { marker: "after", nested: [7, 8] };
  yield 2;
}

suspendedGenerator = resuspendWithYoungValue();
suspendedGenerator.next();
junk = [];
for (let i = 0; i < 100000; i++) { junk.push({ a: i, b: [i, i + 1] }); if (junk.length > 1024) junk = []; }
check("open upvalue before resume", suspendedHolder.read().marker, "before");
for (let i = 0; i < 1000; i++) suspendedHolder.write(i);
check("open upvalue write warmup", suspendedHolder.read(), 999);
suspendedHolder.write({ marker: "written", nested: [5, 6] });
junk = [];
for (let i = 0; i < 10000; i++) { junk.push({ a: i, b: [i, i + 1] }); if (junk.length > 1024) junk = []; }
const written = suspendedHolder.read();
check("open upvalue write", written && written.marker, "written");
check("open upvalue write child", written && written.nested && written.nested[1], 6);
suspendedGenerator.next();
junk = [];
for (let i = 0; i < 10000; i++) { junk.push({ a: i, b: [i, i + 1] }); if (junk.length > 1024) junk = []; }
check("open upvalue after resume", suspendedHolder.read().marker, "after");
check("open upvalue child after resume", suspendedHolder.read().nested[1], 8);

console.log(`passed: ${pass}, failed: ${fail}`);
if (fail) throw new Error("escaped state regression");
