const c = Ant.unsafe.c({
  entry: 'unpack_bits',
  args: ['uint8'],
  returns: 'uint32'
});

function unpack_bits(num) {
  const result = c`
  #include <stdint.h>

  uint32_t unpack_bits(uint8_t num) {
    uint32_t x = num;
    x = (x | (x << 12)) & 0x000F000F;
    x = (x | (x <<  6)) & 0x03030303;
    x = (x | (x <<  3)) & 0x11111111;
    return x;
  }
  `;

  return result(num).toString(16).padStart(8, '0');
}

for (let i = 0; i <= 20; i++) {
  console.log(`bits(${i}): ${unpack_bits(i)}`);
}
