const e = new TextEncoder();
const d = new TextDecoder();

function NaNcode(string) {
  const encoded = Array.from(e.encode(string));
  encoded.unshift(0, 0, 0, 0, 0, 0);
  while (encoded.length % 6) encoded.push(0);

  const bytes = [];
  let i = 0;

  while (encoded.length) {
    bytes.push(encoded.shift());
    i++;
    if (!(i % 6)) bytes.push(248, 127);
  }

  return Array.from(new Float64Array(new Uint8Array(bytes).buffer));
}

function unNaNcode(NaNs) {
  const raw = new Uint8Array(new Float64Array(NaNs).buffer);
  let count = 0;

  for (let i = 0; i < raw.length; i++) {
    if (i % 8 < 6 && raw[i] !== 0) count++;
  }

  const bytes = new Uint8Array(count);

  let j = 0;
  for (let i = 0; i < raw.length; i++) {
    if (i % 8 < 6 && raw[i] !== 0) bytes[j++] = raw[i];
  }

  return d.decode(bytes);
}

const NaNcoded = NaNcode('Hello, World! :D');
console.log(NaNcoded, unNaNcode(NaNcoded));
