type AntType =
  | 'object'
  | 'prop'
  | 'string'
  | 'undefined'
  | 'null'
  | 'number'
  | 'boolean'
  | 'function'
  | 'coderef'
  | 'cfunc'
  | 'err'
  | 'array'
  | 'promise'
  | 'typedarray'
  | 'bigint'
  | 'propref'
  | 'symbol'
  | 'generator';

interface AntGcResult {
  heapBefore: number;
  heapAfter: number;
  usedBefore: number;
  usedAfter: number;
  freed: number;
  arenaBefore: number;
  arenaAfter: number;
  arenaFreed: number;
}

interface AntAllocResult {
  arenaSize: number;
  heapSize: number;
  freeBytes: number;
  usedBytes: number;
  totalBytes: number;
}

interface AntStatsResult {
  arenaUsed: number;
  arenaLwm: number;
  cstack: number;
  gcHeapSize: number;
  gcFreeBytes: number;
  gcUsedBytes: number;
}

interface AntRaw {
  typeof(t: unknown): number;
}

interface AntStatic {
  version: string;
  revision: string;
  buildDate: string;

  typeof(t: unknown): AntType | '??';
  raw: AntRaw;

  gc(): AntGcResult;
  alloc(): AntAllocResult;
  stats(): AntStatsResult;
  signal(signum: number, handler: (signum: number) => void): void;
  sleep(seconds: number): void;
  msleep(milliseconds: number): void;
  usleep(microseconds: number): void;
}

declare const Ant: AntStatic;
