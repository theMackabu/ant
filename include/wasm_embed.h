#ifndef ANT_WASM_EMBED_H
#define ANT_WASM_EMBED_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef ANT_WASM_EMBED

#if !defined(__wasm__)
#error "ANT_WASM_EMBED requires a WebAssembly target"
#endif

#define ANT_WASM_IMPORT(module_literal, name_literal) \
  __attribute__((import_module(module_literal), import_name(name_literal)))

#define ANT_WASM_EXPORT(name_literal) \
  __attribute__((export_name(name_literal), used, visibility("default")))

ANT_WASM_IMPORT("ant", "host_call")
uint32_t ant_wasm_host_call(
  int32_t function_id, const char *arguments, 
  uint32_t arguments_length, uint32_t *response_length
);

ANT_WASM_IMPORT("ant", "now_ms")
double ant_wasm_now_ms(void);

ANT_WASM_IMPORT("ant", "random_fill")
int32_t ant_wasm_random_fill(void *buffer, uint32_t length);

bool ant_wasm_should_interrupt(ant_t *js);
void ant_wasm_microtasks_reset(ant_t *js);

#endif

#endif
