type AntType =
  | 'undefined'
  | 'null'
  | 'boolean'
  | 'number'
  | 'bigint'
  | 'string'
  | 'symbol'
  | 'object'
  | 'array'
  | 'function'
  | 'cfunc'
  | 'closure'
  | 'promise'
  | 'generator'
  | 'err'
  | 'typedarray'
  | 'ffi'
  | 'ntarg';

type AntHost =
  | 'cygwin'
  | 'darwin'
  | 'dragonfly'
  | 'emscripten'
  | 'freebsd'
  | 'gnu'
  | 'haiku'
  | 'linux'
  | 'netbsd'
  | 'openbsd'
  | 'windows'
  | 'sunos'
  | 'os/2';

interface AntPoolInfo {
  used: number;
  capacity: number;
  blocks: number;
}

interface AntPoolStats {
  rope: AntPoolInfo;
  symbol: AntPoolInfo;
  bigint: AntPoolInfo;
  string: AntPoolInfo;
  totalUsed: number;
  totalCapacity: number;
}

interface AntExternalMemory {
  buffers: number;
  code: number;
  total: number;
}

interface AntAllocStats {
  objectCount: number;
  objects: number;
  overflow: number;
  extraSlots: number;
  promises: number;
  proxies: number;
  exotic: number;
  arrays: number;
  shapes: number;
  closures: number;
  upvalues: number;
  propRefs: number;
  total: number;
}

interface AntStatsResult {
  pools: AntPoolStats;
  alloc: AntAllocStats;
  external: AntExternalMemory;
  cstack: number;
  rss?: number;
  virtualSize?: number;
}

interface AntRaw {
  readonly stack: string;
  typeof(t: unknown): number;
  ctorPropFeedback(fn: Function): AntCtorPropFeedback;
  gcMarkProfile(): AntGcMarkProfile;
  gcMarkProfileEnable(enabled?: boolean): boolean;
  gcMarkProfileReset(): void;
}

interface AntCtorPropFeedback {
  samples: number;
  overflowFrom: number;
  inobjLimit: number;
  inobjLimitFrozen: boolean;
  slackRemaining: number;
  bins: number[];
  name?: string;
  filename?: string;
}

interface AntGcMarkProfile {
  enabled: boolean;
  collections: number;
  funcVisits: number;
  childEdges: number;
  constSlots: number;
  timeNs: number;
  timeMs: number;
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
  host: AntHost;

  inspect(...args: unknown[]): void;
  typeof(t: unknown): AntType | '??';

  raw: AntRaw;
  stats(): AntStatsResult;

  sleep(seconds: number): void;
  msleep(milliseconds: number): void;
  usleep(microseconds: number): void;

  signal(signum: number, handler: (signum: number) => void): void;
  serve<T extends HttpContext = HttpContext>(port: number, handler?: (ctx: T) => void | Promise<void>): number;
}

declare const Ant: AntStatic;
