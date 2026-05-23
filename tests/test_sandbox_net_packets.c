#include "../src/sandbox/backends/darwin/net_internal.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
#if defined(__aarch64__) && defined(__APPLE__)
  unsigned char raw16[2] = { 0x12, 0x34 };
  assert(ant_net_load16(raw16) == 0x1234);

  unsigned char out16[2] = { 0 };
  ant_net_store16(out16, 0xabcd);
  assert(out16[0] == 0xab);
  assert(out16[1] == 0xcd);

  unsigned char out32[4] = { 0 };
  ant_net_store32(out32, 0x01020304);
  assert(out32[0] == 0x01);
  assert(out32[1] == 0x02);
  assert(out32[2] == 0x03);
  assert(out32[3] == 0x04);

  unsigned char one_word[2] = { 0x00, 0x01 };
  assert(ant_net_csum(one_word, sizeof(one_word), 0) == 0xfffe);

  unsigned char all_ones[2] = { 0xff, 0xff };
  assert(ant_net_csum(all_ones, sizeof(all_ones), 0) == 0x0000);

  unsigned char odd_byte[1] = { 0x01 };
  assert(ant_net_csum(odd_byte, sizeof(odd_byte), 0) == 0xfeff);

  unsigned char tcp[20] = { 0 };
  ant_net_store16(tcp, 1234);
  ant_net_store16(tcp + 2, 443);
  tcp[12] = 5u << 4u;
  tcp[13] = ANT_TCP_SYN;
  assert(ant_net_l4_csum(ANT_NET_GUEST_IP, 0x5db8d822u, ANT_IP_TCP, tcp, sizeof(tcp)) == 0x676c);
#endif
  return 0;
}
