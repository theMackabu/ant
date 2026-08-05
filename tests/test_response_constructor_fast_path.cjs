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

const sourceHeaders = new Headers({ "content-type": "text/plain", "x-source": "yes" });
const copied = new Response("hello", { headers: sourceHeaders, status: 201, statusText: "Created" });
sourceHeaders.set("x-source", "changed");

assert(copied.status === 201, "initialized response status");
assert(copied.statusText === "Created", "initialized response status text");
assert(copied.headers.get("x-source") === "yes", "Headers init is copied");
assert(
  copied.headers.get("content-type") === "text/plain",
  "provided text content type is preserved",
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

let getterCalls = 0;
const accessorInit = {
  get status() { getterCalls++; return 202; },
  get statusText() { getterCalls++; return "Accepted"; },
  get headers() { getterCalls++; return { "x-accessor": "yes" }; },
};
const accessorResponse = new Response("hello", accessorInit);
assert(accessorResponse.status === 202, "Response init status accessor");
assert(accessorResponse.statusText === "Accepted", "Response init statusText accessor");
assert(accessorResponse.headers.get("x-accessor") === "yes", "Response init headers accessor");
assert(getterCalls === 3, "Response init accessors called once");

const inheritedInit = Object.create({ status: 203, statusText: "Non-Authoritative Information" });
const inheritedResponse = new Response(null, inheritedInit);
assert(inheritedResponse.status === 203, "Response init inherited status");
assert(
  inheritedResponse.statusText === "Non-Authoritative Information",
  "Response init inherited statusText",
);

Object.prototype.status = 206;
Object.prototype.statusText = "Partial Content";
const inheritedLiteralResponse = new Response(null, {
  headers: { "content-type": "text/plain" },
});
delete Object.prototype.status;
delete Object.prototype.statusText;
assert(inheritedLiteralResponse.status === 206, "literal init inherited status");
assert(
  inheritedLiteralResponse.statusText === "Partial Content",
  "literal init inherited statusText",
);

let proxyGets = 0;
const proxyInit = new Proxy({}, {
  get(_target, key) {
    proxyGets++;
    if (key === "status") return 204;
    return undefined;
  },
});
const proxyResponse = new Response(null, proxyInit);
assert(proxyResponse.status === 204, "Response init proxy status");
assert(proxyGets === 3, "Response init proxy fields observed");

console.log("OK: test_response_constructor_fast_path");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
