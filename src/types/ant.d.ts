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

interface HttpContext {
  request: {
    method: string;
    url: string;
    path: string;
    query: string;
    body: string;
    headers: Record<string, string>;
  };
  response: {
    status(code: number): HttpContext['response'];
    header(name: string, value: string): HttpContext['response'];
    send(body: string): void;
    json(data: unknown): void;
    redirect(url: string, status?: number): void;
  };
  store: Record<string, unknown>;
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

  serve(port: number, handler?: (ctx: HttpContext) => void | Promise<void>): number;
}

declare const Ant: AntStatic;
