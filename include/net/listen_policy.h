#ifndef ANT_NET_LISTEN_POLICY_H
#define ANT_NET_LISTEN_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

bool Ant_Net_ListenPolicy_Restricted(const ant_t *js);
bool Ant_Net_ListenPolicy_PortAllowed(const ant_t *js, int port);

void Ant_Net_ListenPolicy_SetForwards(ant_t *js, const uint16_t *ports, uint32_t count);
void Ant_Net_ListenPolicy_Inherit(ant_t *js, const ant_t *parent);
void Ant_Net_ListenPolicy_Clear(ant_t *js);

#endif
