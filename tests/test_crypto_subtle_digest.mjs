import assert from "node:assert";

const data = new TextEncoder().encode("ant");
const digest = await crypto.subtle.digest("SHA-256", data);

assert.ok(digest instanceof ArrayBuffer);

const hex = Array.from(new Uint8Array(digest))
  .map((byte) => byte.toString(16).padStart(2, "0"))
  .join("");

assert.equal(hex, "67a333356cdc566e6e346b5718447308ec0e25f47e623161fb03962b327a651f");

const viaModule = await (await import("node:crypto")).webcrypto.subtle.digest(
  { name: "SHA-256" },
  data,
);

assert.ok(viaModule instanceof ArrayBuffer);
assert.equal(
  Array.from(new Uint8Array(viaModule))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join(""),
  hex,
);
