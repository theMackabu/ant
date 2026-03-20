import type { ptr } from './stdlib.h';

import { mem, brk } from './memory.h';
import { $, malloc, free, memcpy, typedef } from './stdlib.h';

const da_t = typedef([
  { name: 'items', type: 'ptr' },
  { name: 'count', type: 'int' },
  { name: 'capacity', type: 'int' }
]);

function da_create(): ptr {
  const p = malloc(da_t.size);
  mem[p + $(da_t, 'items')] = 0;
  mem[p + $(da_t, 'count')] = 0;
  mem[p + $(da_t, 'capacity')] = 0;
  return p;
}

function da_append(da: ptr, item: unknown): void {
  let count = mem[da + $(da_t, 'count')] as number;
  let cap = mem[da + $(da_t, 'capacity')] as number;
  let items = mem[da + $(da_t, 'items')] as ptr;

  if (count >= cap) {
    const new_cap = cap === 0 ? 256 : cap * 2;
    const new_items = malloc(new_cap);
    if (items) {
      memcpy(new_items, items, count);
      free(items, cap);
    }
    mem[da + $(da_t, 'items')] = new_items;
    mem[da + $(da_t, 'capacity')] = new_cap;
    items = new_items;
  }

  mem[items + count] = item;
  mem[da + $(da_t, 'count')] = count + 1;
}

function da_get(da: ptr, i: number): unknown {
  return mem[(mem[da + $(da_t, 'items')] as ptr) + i];
}

function da_set(da: ptr, i: number, item: unknown): void {
  mem[(mem[da + $(da_t, 'items')] as ptr) + i] = item;
}

function da_pop(da: ptr): unknown {
  const count = (mem[da + $(da_t, 'count')] as number) - 1;
  mem[da + $(da_t, 'count')] = count;
  const items = mem[da + $(da_t, 'items')] as ptr;
  const item = mem[items + count];
  delete mem[items + count];
  return item;
}

function da_count(da: ptr): number {
  return mem[da + $(da_t, 'count')] as number;
}

function da_capacity(da: ptr): number {
  return mem[da + $(da_t, 'capacity')] as number;
}

function da_free(da: ptr): void {
  const items = mem[da + $(da_t, 'items')] as ptr;
  const cap = mem[da + $(da_t, 'capacity')] as number;
  if (items) free(items, cap);
  free(da, da_t.size);
}

const names = da_create();

da_append(names, 'tsoding');
da_append(names, 'ritchie');
da_append(names, 'kernighan');
da_append(names, 'pike');
da_append(names, 'carmack');

console.log(da_get(names, 0));
console.log(da_get(names, 4));
console.log(da_count(names));
console.log(da_capacity(names));

da_set(names, 1, 'thompson');
console.log(da_get(names, 1));

console.log(da_pop(names));
console.log(da_pop(names));
console.log(da_count(names));

da_append(names, 'lol');
da_append(names, 'handmade');
console.log(da_count(names));

for (let i = 0; i < da_count(names); i++) {
  console.log(i, da_get(names, i));
}

const nums = da_create();
for (let i = 0; i < 1000; i++) {
  da_append(nums, i * i);
}
console.log(da_count(nums));
console.log(da_capacity(nums));
console.log(da_get(nums, 999));

da_free(nums);

const vec3_t = typedef([
  { name: 'x', type: 'float' },
  { name: 'y', type: 'float' },
  { name: 'z', type: 'float' }
]);

const v = malloc(vec3_t.size);
mem[v + $(vec3_t, 'x')] = 1.0;
mem[v + $(vec3_t, 'y')] = 2.0;
mem[v + $(vec3_t, 'z')] = 3.0;

console.log(mem[v + $(vec3_t, 'x')]);
console.log(mem[v + $(vec3_t, 'y')]);
console.log(mem[v + $(vec3_t, 'z')]);

console.log('brk:', brk);
console.log('expecting error:');

da_free(names);

if (Math.random() < 0.5) da_free(names);
else console.log(da_count(names));
