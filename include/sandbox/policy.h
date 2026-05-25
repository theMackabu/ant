#ifndef ANT_SANDBOX_POLICY_H
#define ANT_SANDBOX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

bool ant_sandbox_policy_forward_restricted(void);
bool ant_sandbox_policy_port_forwarded(int port);
void ant_sandbox_policy_set_forwards(const uint16_t *ports, uint32_t count);

#endif
