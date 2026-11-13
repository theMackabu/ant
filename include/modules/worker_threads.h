#pragma once

#include "types.h"

uint64_t ant_worker_thread_id(ant_t *js);
ant_value_t ant_worker_exit(ant_t *js, int code);

__attribute__((cold)) ant_value_t worker_threads_library(ant_t *js);
__attribute__((cold)) void init_web_worker_module(ant_t *js);

bool ant_worker_is_worker(ant_t *js);
bool ant_worker_report_uncaught(ant_t *js, ant_value_t reason);

void cleanup_worker_threads_module(ant_t *js);
void ant_worker_messaging_listen(ant_t *js);
