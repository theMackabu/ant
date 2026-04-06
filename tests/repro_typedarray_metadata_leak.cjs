'use strict';

const total = 600000;
const step = 100000;
let checksum = 0;

for (let i = 0; i < total; i++) {
  const buf = Buffer.alloc(32);
  buf[0] = i & 255;
  checksum ^= buf[0];

  if ((i + 1) % step === 0) {
    console.log(`allocated ${i + 1}`);
  }
}

console.log(`OK: allocated ${total} buffers checksum=${checksum}`);
