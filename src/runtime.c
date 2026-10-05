#include "internal.h"
#include "cage.h"
#include "runtime.h"
#include "descriptors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>
#ifndef ANT_WASM_EMBED
#include <argtable3.h>
#endif

#if defined(ANT_WASM_EMBED)
#define ant_getpid() 0
#elif defined(_WIN32)
#include <process.h>
#define ant_getpid _getpid
#else
#include <unistd.h>
#define ant_getpid getpid
#endif

struct code_intern {
  const char *ptr;
  size_t len;
  UT_hash_handle hh;
};

struct code_block {
  struct code_block *next;
  size_t used;
  size_t capacity;
  size_t alloc_size;
  char data[];
};

typedef struct code_block code_block_t;
typedef struct code_intern code_intern_t;

static_assert(
  (CODE_ARENA_ALIGNMENT & (CODE_ARENA_ALIGNMENT - 1u)) == 0,
  "code arena alignment must be a power of two"
);
static_assert(
  offsetof(code_block_t, data) % CODE_ARENA_ALIGNMENT == 0,
  "code arena block payload must satisfy the arena alignment"
);

static code_block_t *arena_new_block(size_t min_size) {
  size_t capacity = CODE_ARENA_BLOCK_SIZE;
  if (min_size > capacity) capacity = min_size;

  size_t alloc_size = sizeof(code_block_t) + capacity;
  code_block_t *block = ant_cage_alloc(alloc_size, _Alignof(code_block_t));
  
  if (!block) return NULL;
  block->next = NULL;
  block->used = 0;
  block->capacity = capacity;
  block->alloc_size = alloc_size;
  
  return block;
}

static void *arena_bump(ant_code_arena_t *arena, size_t size) {
  const size_t align_mask = (size_t)CODE_ARENA_ALIGNMENT - 1u;
  size = (size + align_mask) & ~align_mask;
  
  code_block_t *current = arena->current;
  size_t used = current ? (current->used + align_mask) & ~align_mask : 0;
    
  if (!current || used + size > current->capacity) {
    code_block_t *kept = current ? current->next : NULL;
    if (kept && size <= kept->capacity) {
      kept->used = 0;
      arena->current = kept;
    } else {
      code_block_t *block = arena_new_block(size);
      
      if (!block) return NULL;
      block->next = kept;
      if (!arena->head) arena->head = block;
      else if (current) current->next = block;
      
      arena->current = block;
    }
    
    used = 0;
  }

  void *ptr = &arena->current->data[used];
  arena->current->used = used + size;
  
  return ptr;
}

static size_t arena_get_memory(const ant_code_arena_t *arena) {
  size_t total = 0;
  for (code_block_t *b = arena->head; b; b = b->next)
    total += sizeof(code_block_t) + b->capacity;
  return total;
}

static void arena_free(ant_code_arena_t *arena) {
  for (code_block_t *b = arena->head, *next; b; b = next) {
    next = b->next;
    ant_cage_free(b, b->alloc_size);
  }
  arena->head = arena->current = NULL;
}

const char *code_arena_alloc(ant_t *js, const char *code, size_t len) {
  if (!code || len == 0) return NULL;

  code_intern_t *found = NULL;
  HASH_FIND(hh, js->arenas.interns, code, len, found);
  if (found) return found->ptr;

  char *dest = arena_bump(&js->arenas.code, len + 1);
  if (!dest) return NULL;
  
  memcpy(dest, code, len);
  dest[len] = '\0';

  code_intern_t *entry = malloc(sizeof(*entry));
  if (entry) {
    entry->ptr = dest;
    entry->len = len;
    HASH_ADD_KEYPTR(hh, js->arenas.interns, entry->ptr, entry->len, entry);
  }

  return dest;
}

void *code_arena_bump(ant_t *js, size_t size) {
  return arena_bump(&js->arenas.code, size);
}

size_t code_arena_get_memory(ant_t *js) {
  return arena_get_memory(&js->arenas.code);
}

void *parse_arena_bump(ant_t *js, size_t size) {
  return arena_bump(&js->arenas.parse, size);
}

size_t parse_arena_get_memory(ant_t *js) {
  return arena_get_memory(&js->arenas.parse);
}

code_arena_mark_t parse_arena_mark(ant_t *js) {
  code_block_t *current = js->arenas.parse.current;
  return (code_arena_mark_t){ .block = current, .used = current ? current->used : 0 };
}

void parse_arena_rewind(ant_t *js, code_arena_mark_t mark) {
  ant_code_arena_t *arena = &js->arenas.parse;
  code_block_t *target = (code_block_t *)mark.block;
  code_block_t *kept = target ? target->next : arena->head;
  code_block_t *rest = kept;

  if (kept && kept->capacity == CODE_ARENA_BLOCK_SIZE) {
    rest = kept->next;
    kept->used = 0;
    kept->next = NULL;
  } else kept = NULL;

  while (rest) {
    code_block_t *next = rest->next;
    ant_cage_free(rest, rest->alloc_size);
    rest = next;
  }

  if (!target) {
    arena->head = arena->current = kept;
    return;
  }

  target->used = mark.used <= target->capacity ? mark.used : target->capacity;
  target->next = kept;
  arena->current = target;
}

void code_arenas_destroy(ant_t *js) {
  code_intern_t *entry, *tmp;
  HASH_ITER(hh, js->arenas.interns, entry, tmp) {
    HASH_DEL(js->arenas.interns, entry);
    free(entry);
  }
  
  js->arenas.interns = NULL;
  arena_free(&js->arenas.code);
  arena_free(&js->arenas.parse);
}

void ant_runtime_init(ant_t *js, int argc, char **argv, struct arg_file *ls_p) {
  ant_value_t global = js_glob(js);

  js->primordials = js_mkundef();
  for (size_t i = 0; i < ANT_PRIMORDIAL_COUNT; i++)
    js->primordial_values[i] = js_mkundef();
  js->Ant = js_newobj(js);
  js->runtime.flags = 0;
  js->runtime.argc = argc;
  js->runtime.argv = argv;
  js->runtime.pid = (int)ant_getpid();
#ifdef ANT_WASM_EMBED
  (void)ls_p;
  js->runtime.ls_fp = NULL;
#else
  js->runtime.ls_fp = (ls_p && ls_p->count > 0) ? ls_p->filename[0] : NULL;
#endif

  js_set(js, global, "onerror", js_mknull());
  js_set_descriptor(js, global, "onerror", 7, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "onunhandledrejection", js_mknull());
  js_set_descriptor(js, global, "onunhandledrejection", 20, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "onrejectionhandled", js_mknull());
  js_set_descriptor(js, global, "onrejectionhandled", 18, JS_DESC_W | JS_DESC_C);
  
  js_set(js, global, "self", global);
  js_set_descriptor(js, global, "self", 4, JS_DESC_W | JS_DESC_C);
  
  js_set(js, global, "window", global);
  js_set_descriptor(js, global, "window", 6, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "global", global);
  js_set_descriptor(js, global, "global", 6, JS_DESC_W | JS_DESC_E | JS_DESC_C);

  js_set(js, global, "globalThis", global);
  js_set_descriptor(js, global, "globalThis", 10, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "Ant", js->Ant);
  js_set_descriptor(js, global, "Ant", 3, JS_DESC_E);
}

void ant_runtime_set_argv(ant_t *js, int argc, char **argv) {
  if (argc < 0 || (argc > 0 && argv == NULL)) {
    js->runtime.argc = 0;
    js->runtime.argv = NULL;
    return;
  }
  js->runtime.argc = argc;
  js->runtime.argv = argv;
}
