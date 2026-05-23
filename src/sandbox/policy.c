#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>

#include "sandbox/policy.h"

#define ANT_SANDBOX_POLICY_MAX_FORWARDS 32u

static bool g_forward_policy = false;
static uint16_t g_forward_ports[ANT_SANDBOX_POLICY_MAX_FORWARDS];
static uint32_t g_forward_count = 0;

void ant_sandbox_policy_set_forwards(const uint16_t *ports, uint32_t count) {
  g_forward_policy = true;
  if (count > ANT_SANDBOX_POLICY_MAX_FORWARDS) count = ANT_SANDBOX_POLICY_MAX_FORWARDS;
  g_forward_count = count;
  for (uint32_t i = 0; i < count; i++) g_forward_ports[i] = ports[i];
}

bool ant_sandbox_policy_port_forwarded(int port) {
  if (!g_forward_policy) return true;
  if (port <= 0 || port > UINT16_MAX) return false;
  for (uint32_t i = 0; i < g_forward_count; i++) {
    if (g_forward_ports[i] == (uint16_t)port) return true;
  }
  return false;
}
