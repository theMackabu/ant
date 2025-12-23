#ifndef ATOMICS_H
#define ATOMICS_H

#include <stdint.h>
#include <pthread.h>

void init_atomics_module(void);

typedef struct WaitQueueEntry {
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  int32_t *address;
  int notified;
  struct WaitQueueEntry *next;
} WaitQueueEntry;

typedef struct {
  WaitQueueEntry *head;
  pthread_mutex_t lock;
} WaitQueue;

void wait_queue_init(WaitQueue *queue);
void wait_queue_cleanup(WaitQueue *queue);

void wait_queue_add(WaitQueue *queue, WaitQueueEntry *entry);
void wait_queue_remove(WaitQueue *queue, WaitQueueEntry *entry);
int wait_queue_notify(WaitQueue *queue, int32_t *address, int count);

#endif
