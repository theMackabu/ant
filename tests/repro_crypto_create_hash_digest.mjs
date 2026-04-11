import { createHash } from "node:crypto";

console.log("createHash:", typeof createHash);

const hash = createHash("sha256");

console.log("hash:", typeof hash);
console.log("hash.update:", typeof (hash && hash.update));
console.log("hash.digest:", typeof (hash && hash.digest));

const out = hash.update("ant").digest("hex");
console.log("digest:", out);
