#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <runtime.h>
#include <arena.h>

static struct ant_runtime runtime = {0};
struct ant_runtime *const rt = &runtime;

static code_block_t *code_arena_head = NULL;
static code_block_t *code_arena_current = NULL;

static code_block_t *code_arena_new_block(size_t min_size) {
  size_t capacity = CODE_ARENA_BLOCK_SIZE;
  if (min_size > capacity) capacity = min_size;
  
  code_block_t *block = (code_block_t *)ANT_GC_MALLOC_ATOMIC(sizeof(code_block_t) + capacity);
  if (!block) return NULL;
  
  block->next = NULL;
  block->used = 0;
  block->capacity = capacity;
  return block;
}

const char *code_arena_alloc(const char *code, size_t len) {
  if (!code || len == 0) return NULL;
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
  
  return dest;
}

void code_arena_reset(void) {
  code_arena_head = NULL;
  code_arena_current = NULL;
}

struct ant_runtime *ant_runtime_init(struct js *js, int argc, char **argv, struct arg_file *ls_p) {
  runtime.js = js;
  runtime.ant_obj = js_newobj(js);
  runtime.flags = 0;
  
  runtime.argc = argc;
  runtime.argv = argv;
  runtime.ls_fp = ls_p->count > 0 ? ls_p->filename[0] : NULL;
  
  js_set(js, js_glob(js), "global", js_glob(js));
  js_set(js, js_glob(js), "window", js_glob(js));
  js_set(js, js_glob(js), "globalThis", js_glob(js));
  js_set(js, js_glob(js), "Ant", runtime.ant_obj);

  return &runtime;
}