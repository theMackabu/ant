#include "ant.h"
#include "runtime.h"
#include "descriptors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>
#include <argtable3.h>

#ifdef _WIN32
#include <process.h>
#define ant_getpid _getpid
#else
#include <unistd.h>
#define ant_getpid getpid
#endif

typedef struct {
  const char *ptr;
  size_t len;
  UT_hash_handle hh;
} intern_entry_t;

typedef struct code_block {
  struct code_block *next;
  size_t used;
  size_t capacity;
  char data[];
} code_block_t;

static struct ant_runtime runtime = {0};
struct ant_runtime *const rt = &runtime;

static code_block_t *code_arena_head = NULL;
static code_block_t *code_arena_current = NULL;
static intern_entry_t *code_interns = NULL;

static void code_interns_prune_for_block_range(
  const code_block_t *block,
  size_t start_offset
) {
  if (!block) return;

  const char *start = block->data + start_offset;
  const char *end = block->data + block->capacity;
  intern_entry_t *entry = NULL;
  intern_entry_t *tmp = NULL;

  HASH_ITER(hh, code_interns, entry, tmp)
  if (entry->ptr >= start && entry->ptr < end) {
    HASH_DEL(code_interns, entry);
    free(entry);
  }
}

static void code_interns_prune_for_blocks(code_block_t *first) {
  for (
    code_block_t *block = first; 
    block; block = block->next
  ) code_interns_prune_for_block_range(block, 0);
}

static code_block_t *code_arena_new_block(size_t min_size) {
  size_t capacity = CODE_ARENA_BLOCK_SIZE;
  if (min_size > capacity) capacity = min_size;

  code_block_t *block = malloc(sizeof(code_block_t) + capacity);
  if (!block) return NULL;

  block->next = NULL;
  block->used = 0;
  block->capacity = capacity;
  return block;
}

const char *code_arena_alloc(const char *code, size_t len) {
  if (!code || len == 0) return NULL;

  intern_entry_t *found = NULL;
  HASH_FIND(hh, code_interns, code, len, found);
  if (found) return found->ptr;

  size_t alloc_size = len + 1;
  if (!code_arena_current || code_arena_current->used + alloc_size > code_arena_current->capacity) {
    code_block_t *new_block = code_arena_new_block(alloc_size);
    if (!new_block) return NULL;
    if (!code_arena_head) code_arena_head = new_block;
    else if (code_arena_current) code_arena_current->next = new_block;
    code_arena_current = new_block;
  }

  char *dest = &code_arena_current->data[code_arena_current->used];
  memcpy(dest, code, len);
  dest[len] = '\0';
  code_arena_current->used += alloc_size;

  intern_entry_t *entry = malloc(sizeof(*entry));
  if (entry) {
    entry->ptr = dest;
    entry->len = len;
    HASH_ADD_KEYPTR(hh, code_interns, entry->ptr, entry->len, entry);
  }

  return dest;
}

void *code_arena_bump(size_t size) {
  size = (size + 7) & ~(size_t)7;
  if (!code_arena_current || code_arena_current->used + size > code_arena_current->capacity) {
    code_block_t *new_block = code_arena_new_block(size);
    if (!new_block) return NULL;
    if (!code_arena_head) code_arena_head = new_block;
    else if (code_arena_current) code_arena_current->next = new_block;
    code_arena_current = new_block;
  }
  void *ptr = &code_arena_current->data[code_arena_current->used];
  code_arena_current->used += size;
  return ptr;
}

size_t code_arena_get_memory(void) {
  size_t total = 0;
  for (code_block_t *b = code_arena_head; b; b = b->next)
    total += sizeof(code_block_t) + b->capacity;
  return total;
}

code_arena_mark_t code_arena_mark(void) {
  code_arena_mark_t mark = {0};
  mark.block = code_arena_current;
  mark.used = code_arena_current ? code_arena_current->used : 0;
  return mark;
}

void code_arena_rewind(code_arena_mark_t mark) {
  code_block_t *target = (code_block_t *)mark.block;

  if (!target) {
    code_interns_prune_for_blocks(code_arena_head);
    code_block_t *block = code_arena_head;
    
    while (block) {
      code_block_t *next = block->next;
      free(block); block = next;
    }
    
    code_arena_head = NULL;
    code_arena_current = NULL;
    
    return;
  }

  size_t clamped_used = mark.used <= target->capacity ? mark.used : target->capacity;
  code_interns_prune_for_block_range(target, clamped_used);
  code_interns_prune_for_blocks(target->next);

  if (mark.used <= target->capacity) target->used = mark.used;
  code_block_t *b = target->next;
  
  while (b) {
    code_block_t *next = b->next;
    free(b); b = next;
  }
  
  target->next = NULL;
  code_arena_current = target;
}

void code_arena_reset(void) {
  intern_entry_t *entry, *tmp;
  HASH_ITER(hh, code_interns, entry, tmp) {
    HASH_DEL(code_interns, entry);
    free(entry);
  }
  code_interns = NULL;

  code_block_t *block = code_arena_head;
  while (block) {
    code_block_t *next = block->next;
    free(block);
    block = next;
  }
  
  code_arena_head = NULL;
  code_arena_current = NULL;
}

void destroy_runtime(ant_t *js) {
  if (rt->js == js) memset(&runtime, 0, sizeof(runtime));
}

struct ant_runtime *ant_runtime_init(ant_t *js, int argc, char **argv, struct arg_file *ls_p) {
  ant_value_t global = js_glob(js);
  
  runtime = (struct ant_runtime){
    .js = js,
    .ant_obj = js_newobj(js),
    .flags = 0, .argc = argc, .argv = argv,
    .pid = (int)ant_getpid(),
    .ls_fp = (ls_p && ls_p->count > 0) ? ls_p->filename[0] : NULL,
  };

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
  js_set_descriptor(js, global, "global", 6, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "globalThis", global);
  js_set_descriptor(js, global, "globalThis", 10, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "Ant", runtime.ant_obj);
  js_set_descriptor(js, global, "Ant", 3, JS_DESC_E);

  return &runtime;
}
