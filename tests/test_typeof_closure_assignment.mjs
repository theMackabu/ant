function assert(cond, msg) {
  if (!cond) {
    console.error(`FAIL: ${msg}`);
    process.exit(1);
  }
}


let viaExpr;
(function () { viaExpr = "hi"; })();
assert(typeof viaExpr === "string", `typeof after closure write: ${typeof viaExpr}`);

let viaHoisted;
setHoisted();
assert(typeof viaHoisted === "number", `typeof after hoisted fn write: ${typeof viaHoisted}`);
function setHoisted() { viaHoisted = 1; }

let viaAsync;
await Promise.resolve().then(() => { viaAsync = true; });
assert(typeof viaAsync === "boolean", `typeof after async write: ${typeof viaAsync}`);

let branchy;
setBranchy();
let took = "wrong";
if (typeof branchy === "string") took = "right";
assert(took === "right", `typeof branch elimination: took ${took}`);
function setBranchy() { branchy = "s"; }

let initialized = 0;
bump();
assert(typeof initialized === "number" && initialized === 1, "typed let after closure write");
function bump() { initialized += 1; }

const stable = 5;
assert(typeof stable === "number", "typeof const still works");

console.log("test_typeof_closure_assignment: OK");
