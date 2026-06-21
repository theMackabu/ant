import crypto from "node:crypto";

const enc = new TextEncoder();

function hex(bytes) {
  return Array.from(new Uint8Array(bytes), (b) => b.toString(16).padStart(2, "0")).join("");
}

const hmac = crypto.createHmac("sha256", "key").update("data").digest("hex");
if (hmac !== "5031fe3d989c6d1537a013fa6e739da23463fdaec3b70137d828e36ace221bd0") {
  throw new Error(`createHmac mismatch: ${hmac}`);
}

const key = await crypto.subtle.importKey(
  "raw",
  enc.encode("key"),
  { name: "HMAC", hash: "SHA-256" },
  false,
  ["sign", "verify"]
);
const sig = await crypto.subtle.sign("HMAC", key, enc.encode("data"));
if (hex(sig) !== hmac) throw new Error("subtle.sign HMAC mismatch");
if (!(await crypto.subtle.verify("HMAC", key, sig, enc.encode("data")))) {
  throw new Error("subtle.verify should accept matching HMAC");
}
if (await crypto.subtle.verify("HMAC", key, sig, enc.encode("other"))) {
  throw new Error("subtle.verify should reject mismatched HMAC");
}

const aesKey = await crypto.subtle.importKey(
  "raw",
  enc.encode("0123456789abcdef0123456789abcdef"),
  "AES-GCM",
  false,
  ["encrypt", "decrypt"]
);
const iv = enc.encode("123456789012");
const plaintext = enc.encode("hello aes-gcm");
const encrypted = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, aesKey, plaintext);
const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, aesKey, encrypted);
if (new TextDecoder().decode(decrypted) !== "hello aes-gcm") {
  throw new Error("AES-GCM roundtrip failed");
}

const pbkdf2 = crypto.pbkdf2Sync("password", "salt", 1, 32, "sha256").toString("hex");
if (pbkdf2 !== "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b") {
  throw new Error(`pbkdf2Sync mismatch: ${pbkdf2}`);
}

let callbackPbkdf2;
const pbkdf2Return = crypto.pbkdf2("password", "salt", 1, 32, "sha256", (err, key) => {
  if (err) throw err;
  callbackPbkdf2 = key.toString("hex");
});
if (pbkdf2Return !== undefined) throw new Error("pbkdf2 callback API should return undefined");
if (callbackPbkdf2 !== pbkdf2) throw new Error("pbkdf2 callback mismatch");

const scrypt = crypto.scryptSync("password", "salt", 16);
if (scrypt.length !== 16) throw new Error("scryptSync length mismatch");

let callbackScrypt;
const scryptReturn = crypto.scrypt("password", "salt", 16, (err, key) => {
  if (err) throw err;
  callbackScrypt = key.length;
});
if (scryptReturn !== undefined) throw new Error("scrypt callback API should return undefined");
if (callbackScrypt !== 16) throw new Error("scrypt callback mismatch");

const cipherKey = Buffer.from("0123456789abcdef0123456789abcdef");
const cipherIv = Buffer.from("123456789012");
const cipher = crypto.createCipheriv("aes-256-gcm", cipherKey, cipherIv);
const ciphertext = Buffer.concat([cipher.update("secret"), cipher.final()]);
const tag = cipher.getAuthTag();
const decipher = crypto.createDecipheriv("aes-256-gcm", cipherKey, cipherIv);
decipher.setAuthTag(tag);
const clear = Buffer.concat([decipher.update(ciphertext), decipher.final()]).toString();
if (clear !== "secret") throw new Error("createCipheriv/createDecipheriv roundtrip failed");

console.log("crypto extended ok");
