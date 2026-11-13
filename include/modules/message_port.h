#pragma once

#include <stdbool.h>
#include "types.h"
#include "modules/wire.h"

typedef void (*ant_port_message_fn)(
  ant_t *js,
  ant_value_t target,
  ant_value_t value,
  ant_value_t ports
);

__attribute__((cold)) 
void init_message_port_module(ant_t *js);
void cleanup_message_port_module(ant_t *js);
void ant_port_end_release(ant_port_end_t *end);

bool ant_message_port_is(ant_value_t value);
bool ant_port_end_pair(ant_port_end_t **a, ant_port_end_t **b);
bool ant_message_port_mirror_global(ant_t *js, ant_value_t port);
ant_value_t ant_message_event_create(ant_t *js, const char *type, ant_value_t data, ant_value_t ports);

void ant_port_binding_drain(ant_port_binding_t *binding);
void ant_port_binding_close(ant_port_binding_t *binding);
void ant_port_binding_free(ant_port_binding_t *binding);

ant_value_t ant_message_port_receive(ant_t *js, ant_value_t port);
ant_value_t ant_message_port_post(ant_t *js, ant_value_t port, ant_value_t value, ant_value_t transfer);
ant_value_t ant_port_end_post(ant_t *js, ant_port_end_t *end, ant_value_t value, ant_value_t transfer);
ant_value_t ant_port_binding_post(ant_t *js, ant_port_binding_t *binding, ant_value_t value, ant_value_t transfer);
ant_port_binding_t *ant_port_end_bind(ant_t *js, ant_port_end_t *end, ant_value_t target, ant_port_message_fn on_message);

ant_value_t ant_message_port_proto(ant_t *js);
ant_value_t ant_message_port_adopt(ant_t *js, ant_port_end_t *end);
ant_port_end_t *ant_message_port_take(ant_t *js, ant_value_t port);
