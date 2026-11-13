#pragma once

#include "types.h"
#include "modules/microtasks.h"

struct timer_entry;
struct microtask_entry;
struct immediate_entry;

typedef struct {
  struct timer_entry *timers;
  struct microtask_entry *next_ticks;
  struct microtask_entry *next_ticks_tail;
  struct microtask_entry *next_ticks_processing;
  struct microtask_entry *microtasks;
  struct microtask_entry *microtasks_tail;
  struct microtask_entry *microtasks_processing;
  struct immediate_entry *immediates;
  struct immediate_entry *immediates_tail;
  struct immediate_entry *immediates_processing;
  struct microtask_entry *free_jobs;
  
  uint32_t free_job_count;
  int next_timer_id;
  int next_immediate_id;
  int active_timer_count;
  int active_refed_timer_count;
} ant_timer_state_t;

ant_value_t timers_library(ant_t *js);
ant_value_t timers_promises_library(ant_t *js);

void init_timer_module(ant_t *js);
void cleanup_timer_module(ant_t *js);
void process_immediates(ant_t *js);

int has_pending_timers(ant_t *js);
int has_pending_immediates(ant_t *js);
int has_active_immediates(ant_t *js);
