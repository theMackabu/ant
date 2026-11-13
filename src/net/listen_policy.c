#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <utarray.h>

#include "isolate.h"
#include "utils.h"
#include "net/listen_policy.h"

struct ant_listen_policy {
  UT_array forwards;
};

static const UT_icd listen_port_icd = { sizeof(uint16_t), NULL, NULL, NULL };

void Ant_Net_ListenPolicy_SetForwards(ant_t *js, const uint16_t *ports, uint32_t count) {
  Ant_Net_ListenPolicy_Clear(js);
  js->listen_policy = try_oom(sizeof(*js->listen_policy));
  utarray_init(&js->listen_policy->forwards, &listen_port_icd);
  for (uint32_t i = 0; i < count; i++) utarray_push_back(&js->listen_policy->forwards, &ports[i]);
}

void Ant_Net_ListenPolicy_Inherit(ant_t *js, const ant_t *parent) {
  if (!parent || !parent->listen_policy) return;
  const UT_array *forwards = &parent->listen_policy->forwards;
  Ant_Net_ListenPolicy_SetForwards(js, (const uint16_t *)utarray_front(forwards), utarray_len(forwards));
}

void Ant_Net_ListenPolicy_Clear(ant_t *js) {
  if (!js->listen_policy) return;
  utarray_done(&js->listen_policy->forwards);
  free(js->listen_policy);
  js->listen_policy = NULL;
}

bool Ant_Net_ListenPolicy_Restricted(const ant_t *js) {
  return js->listen_policy != NULL;
}

bool Ant_Net_ListenPolicy_PortAllowed(const ant_t *js, int port) {
  if (!js->listen_policy) return true;
  if (port <= 0 || port > UINT16_MAX) return false;

  const UT_array *forwards = &js->listen_policy->forwards;
  const uint16_t *ports = (const uint16_t *)utarray_front(forwards);
  for (unsigned i = 0; i < utarray_len(forwards); i++) {
    if (ports[i] == (uint16_t)port) return true;
  }
  return false;
}
