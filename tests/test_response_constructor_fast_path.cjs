function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function main() {
const empty = new Response();
assert(empty.status === 200, "empty response status");
assert(empty.statusText === "", "empty response status text");
assert(empty.type === "default", "empty response type");
assert(empty.body === null, "empty response body");

const text = new Response("hello");
assert(
  text.headers.get("content-type") === "text/plain;charset=UTF-8",
  "string body default content type",
);
assert(await text.text() === "hello", "string body contents");

let borrowedBody = "root";
for (let i = 0; i < 64; i++) borrowedBody += `:${i}`;
let borrowed = new Response(borrowedBody);
const borrowedClone = borrowed.clone();
borrowedBody = null;
let borrowedExpected = "root";
for (let i = 0; i < 64; i++) borrowedExpected += `:${i}`;
for (let i = 0; i < 100_000; i++) ({ value: `gc-${i}` });
assert(await borrowed.text() === borrowedExpected, "borrowed string body survives GC pressure");
borrowed = null;
for (let i = 0; i < 100_000; i++) ({ value: `gc-${i}` });
assert(await borrowedClone.text() === borrowedExpected, "borrowed string clone contents");

const sourceHeaders = new Headers({ "content-type": "text/plain", "x-source": "yes" });
const copied = new Response("hello", { headers: sourceHeaders, status: 201, statusText: "Created" });
sourceHeaders.set("x-source", "changed");

assert(copied.status === 201, "initialized response status");
assert(copied.statusText === "Created", "initialized response status text");
assert(copied.headers.get("x-source") === "yes", "Headers init is copied");
assert(
  copied.headers.get("content-type") === "text/plain;charset=UTF-8",
  "provided plain text content type gains the default charset",
);

const customTextType = new Response("hello", {
  headers: { "content-type": "text/custom" },
});
assert(
  customTextType.headers.get("content-type") === "text/custom",
  "custom text content type is preserved",
);

const duplicateHeaders = new Headers();
duplicateHeaders.append("content-type", "text/plain");
duplicateHeaders.append("content-type", "text/html");
const duplicates = new Response("hello", { headers: duplicateHeaders });
assert(
  duplicates.headers.get("content-type") === "text/plain, text/html",
  "duplicate text content types are preserved",
);

class CustomResponse extends Response {}
const custom = new CustomResponse("hello", { headers: { "x-custom": "yes" } });
assert(custom instanceof CustomResponse, "Response subclass prototype");
assert(custom.headers.get("x-custom") === "yes", "record Headers init");

let prototypeReads = 0;
function AlternateResponse() {}
const proxyNewTarget = new Proxy(AlternateResponse, {
  get(target, property, receiver) {
    if (property === "prototype") prototypeReads++;
    return Reflect.get(target, property, receiver);
  },
});
const reflected = Reflect.construct(Response, [], proxyNewTarget);
assert(prototypeReads === 1, "Response reads explicit newTarget prototype once");
assert(
  Object.getPrototypeOf(reflected) === AlternateResponse.prototype,
  "Response preserves explicit proxy newTarget prototype",
);

const BoundResponse = Response.bind(null, "bound");
const bound = new BoundResponse();
assert(bound instanceof Response, "bound Response uses target prototype");
assert(bound instanceof BoundResponse, "bound Response preserves instanceof");

const ProxyResponse = new Proxy(Response, {});
const proxied = new ProxyResponse();
assert(proxied instanceof Response, "proxied Response uses target prototype");
assert(proxied instanceof ProxyResponse, "proxied Response preserves instanceof");

console.log("OK: test_response_constructor_fast_path");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
