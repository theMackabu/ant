function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const bytes = new Uint8Array([72, 101, 108, 108, 111, 32, 87, 111, 114, 108, 100]);

assert(bytes.toHex() === "48656c6c6f20576f726c64", "Uint8Array.prototype.toHex");
assert(bytes.toBase64() === "SGVsbG8gV29ybGQ=", "Uint8Array.prototype.toBase64");

const fromHex = Uint8Array.fromHex("48656c6c6f20576f726c64");
assert(fromHex.length === bytes.length, "Uint8Array.fromHex length");
assert(bytes.every((value, index, array) => array === bytes && value === fromHex[index]), "Uint8Array.fromHex bytes");

const fromBase64 = Uint8Array.fromBase64("SGVsbG8gV29ybGQ=");
assert(fromBase64.length === bytes.length, "Uint8Array.fromBase64 length");
assert(bytes.every((value, index) => value === fromBase64[index]), "Uint8Array.fromBase64 bytes");

const hexTarget = new Uint8Array(16);
const hexResult = hexTarget.setFromHex("48656c6c6f20576f726c64");
assert(hexResult.read === 22, "Uint8Array.setFromHex read count");
assert(hexResult.written === 11, "Uint8Array.setFromHex written count");
assert(bytes.every((value, index) => value === hexTarget[index]), "Uint8Array.setFromHex bytes");

const base64Target = new Uint8Array(16);
const base64Result = base64Target.setFromBase64("SGVsbG8gV29ybGQ=");
assert(base64Result.read === 16, "Uint8Array.setFromBase64 read count");
assert(base64Result.written === 11, "Uint8Array.setFromBase64 written count");
assert(bytes.every((value, index) => value === base64Target[index]), "Uint8Array.setFromBase64 bytes");

console.log("Uint8Array base64/hex tests completed!");
