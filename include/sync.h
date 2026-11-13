#pragma once

#ifdef _WIN32
#include <windows.h>

typedef SRWLOCK ant_mutex_t;
#define ANT_MUTEX_INIT SRWLOCK_INIT

static inline void ant_mutex_lock(ant_mutex_t *m) { AcquireSRWLockExclusive(m); }
static inline void ant_mutex_unlock(ant_mutex_t *m) { ReleaseSRWLockExclusive(m); }
#elif defined(__APPLE__)
#include <os/lock.h>

typedef os_unfair_lock ant_mutex_t;
#define ANT_MUTEX_INIT OS_UNFAIR_LOCK_INIT

static inline void ant_mutex_lock(ant_mutex_t *m) { os_unfair_lock_lock(m); }
static inline void ant_mutex_unlock(ant_mutex_t *m) { os_unfair_lock_unlock(m); }
#else
#include <pthread.h>

typedef pthread_mutex_t ant_mutex_t;
#define ANT_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER

static inline void ant_mutex_lock(ant_mutex_t *m) { pthread_mutex_lock(m); }
static inline void ant_mutex_unlock(ant_mutex_t *m) { pthread_mutex_unlock(m); }
#endif
