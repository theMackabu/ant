import { arena, freedSet, die } from './memory.h';

export type ptr = number;
export type union = 'ptr' | 'int' | 'float' | 'char*' | 'unknown';

export function malloc(size: number): ptr {
  const p = arena.brk;
  arena.brk += size;
  for (let i = 0; i < size; i++) freedSet.delete(p + i);
  return p;
}

export function dealloc(p: ptr, i: ptr) {
  delete arena.mem[p + i];
  freedSet.add(p + i);
}

export function free(p: ptr, size: number): void {
  if (freedSet.has(p)) die(p, 'pointer being freed was not allocated');
  for (let i = 0; i < size; i++) dealloc(p, i);
}

export function memcpy(dst: ptr, src: ptr, n: number): void {
  for (let i = 0; i < n; i++) arena.mem[dst + i] = arena.mem[src + i];
}

export interface struct_def<T extends string = string> {
  fields: Record<T, { offset: number; type: union }>;
  size: number;
}

export function typedef<T extends string>(fields: { name: T; type: union }[]): struct_def<T> {
  const result = {} as Record<T, { offset: number; type: union }>;
  fields.forEach((f, i) => {
    result[f.name] = { offset: i, type: f.type };
  });
  return { fields: result, size: fields.length };
}

export function $<T extends string>(def: struct_def<T>, field: T): number {
  return def.fields[field].offset;
}
