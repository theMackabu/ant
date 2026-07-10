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
  copied.headers.get("content-type") === "text/plain;charset=UTF-8",
  "provided text content type gains charset",
);

const duplicateHeaders = new Headers();
duplicateHeaders.append("content-type", "text/plain");
duplicateHeaders.append("content-type", "text/html");
const duplicates = new Response("hello", { headers: duplicateHeaders });
assert(
  duplicates.headers.get("content-type") === "text/plain;charset=UTF-8",
  "duplicate text content types preserve normalization behavior",
);

class CustomResponse extends Response {}
const custom = new CustomResponse("hello", { headers: { "x-custom": "yes" } });
assert(custom instanceof CustomResponse, "Response subclass prototype");
assert(custom.headers.get("x-custom") === "yes", "record Headers init");

console.log("OK: test_response_constructor_fast_path");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
