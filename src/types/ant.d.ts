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
  req: {
    method: string;
    uri: string;
    query: string;
    body: string;
    header(name: string): string | undefined;
  };
  res: {
    header(name: string, value: string): void;
    status(code: number): void;
    body(body: string, status?: number, contentType?: string): void;
    html(body: string, status?: number): void;
    json(data: unknown, status?: number): void;
    notFound(): void;
    redirect(url: string, status?: number): void;
  };
  set(key: string, value: unknown): void;
  get(key: string): unknown;
}

interface AntStatic {
  version: string;
  target: string;
  revision: string;
  buildDate: string;

  typeof(t: unknown): AntType | '??';
  raw: AntRaw;

  gc(): AntGcResult;
  alloc(): AntAllocResult;
  stats(): AntStatsResult;

  sleep(seconds: number): void;
  msleep(milliseconds: number): void;
  usleep(microseconds: number): void;

  signal(signum: number, handler: (signum: number) => void): void;
  serve<T extends HttpContext = HttpContext>(port: number, handler?: (ctx: T) => void | Promise<void>): number;
}

declare const Ant: AntStatic;
