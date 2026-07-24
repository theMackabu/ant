function makeContext(i) {
  return {
    request: i,
    store: i + 1,
    set: i + 2,
    path: i + 3,
    qi: i + 4,
    error: i + 5,
    redirect: i + 6,
    status: i + 7,
  };
}

let checksum = 0;
for (let i = 0; i < 20_000; i++) {
  const context = makeContext(i);
  checksum += context.request + context.redirect;
}

const expected = 20_000 * 20_000 + 5 * 20_000;
if (checksum !== expected) throw new Error("hot literal checksum mismatch");

const first = makeContext(10);
const second = makeContext(20);
first.path = 99;
first.extra = 100;
delete first.store;

if (first.path !== 99 || first.extra !== 100 || "store" in first)
  throw new Error("mutated literal has incorrect properties");
if (second.path !== 23 || second.store !== 21 || "extra" in second)
  throw new Error("shared literal shape leaked between objects");

const keys = Object.keys(second);
if (keys.join(",") !== "request,store,set,path,qi,error,redirect,status")
  throw new Error("hot literal key order mismatch: " + keys.join(","));

console.log("JIT object literal shape tests passed");
