function generateUUID() {
  const bytes = Ant.Crypto.randomBytes(16);

  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;

  const hex = function (i) {
    const val = bytes[i];
    const digits = '0123456789abcdef';
    const hi = (val >> 4) & 0x0f;
    const lo = val & 0x0f;
    return digits[hi] + digits[lo];
  };

  return (
    hex(0) +
    hex(1) +
    hex(2) +
    hex(3) +
    '-' +
    hex(4) +
    hex(5) +
    '-' +
    hex(6) +
    hex(7) +
    '-' +
    hex(8) +
    hex(9) +
    '-' +
    hex(10) +
    hex(11) +
    hex(12) +
    hex(13) +
    hex(14) +
    hex(15)
  );
}

console.log('builtin:', Ant.Crypto.randomUUID());
console.log('engine:', generateUUID());
