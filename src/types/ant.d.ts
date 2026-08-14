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

interface SymbolConstructor {
  readonly inspect: symbol;
}

interface AntPoolInfo {
  used: number;
  capacity: number;
  blocks: number;
}

interface AntStringPoolInfo extends AntPoolInfo {
  pooled: AntPoolInfo;
  largeLive: AntPoolInfo;
  largeReusable: AntPoolInfo;
  largeQuarantine: AntPoolInfo;
}

interface AntPoolStats {
  rope: AntPoolInfo;
  symbol: AntPoolInfo;
  bigint: AntPoolInfo;
  string: AntStringPoolInfo;
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
  total: number;
}

interface AntStatsResult {
  pools: AntPoolStats;
  alloc: AntAllocStats;
  external: AntExternalMemory;
  intern: {
    count: number;
    bytes: number;
  };
  vm?: {
    stackSize: number;
    stackUsed: number;
    maxFrames: number;
    framesUsed: number;
  };
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

interface AntCStringOptions {
  entry: string;
  returns: 'string';
}

interface AntCUint32Options {
  entry: string;
  returns: 'uint32';
}

type AntCArgumentType =
  | 'int8'
  | 'uint8'
  | 'int16'
  | 'uint16'
  | 'int'
  | 'int32'
  | 'uint32'
  | 'int64'
  | 'uint64'
  | 'float'
  | 'double';

interface AntCStringFunctionOptions extends AntCStringOptions {
  args: readonly AntCArgumentType[];
}

interface AntCUint32FunctionOptions extends AntCUint32Options {
  args: readonly AntCArgumentType[];
}

interface AntUnsafe {
  c(strings: TemplateStringsArray, ...values: unknown[]): number;
  c(options: AntCUint32FunctionOptions):
    (strings: TemplateStringsArray, ...values: unknown[]) => (...args: number[]) => number;
  c(options: AntCStringFunctionOptions):
    (strings: TemplateStringsArray, ...values: unknown[]) => (...args: number[]) => string | null;
  c(options: AntCUint32Options):
    (strings: TemplateStringsArray, ...values: unknown[]) => number;
  c(options: AntCStringOptions):
    (strings: TemplateStringsArray, ...values: unknown[]) => string | null;
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

interface AntWebSocketOptions {
  idleTimeout?: number;
  maxPayloadLength?: number;
  perMessageDeflate?: boolean | object;
}

interface AntServeOptions {
  fetch(request: Request, server: AntServer): Response | Promise<Response>;
  port?: number;
  hostname?: string;
  unix?: string;
  idleTimeout?: number;
  requestTimeout?: number;
  websocket?: AntWebSocketOptions;
  tls?: unknown;
}

interface AntRequestIP {
  address: string;
  port: number;
}

interface AntWebSocketUpgrade {
  socket: WebSocket;
  response: Response;
}

interface AntEventSourceStream {
  response: Response;
  send(data: string): void;
  comment(text: string): void;
  close(): void;
}

interface AntServer {
  hostname: string;
  port: number;
  url?: string;
  unix?: string;
  requestIP(request: Request): AntRequestIP | null;
  timeout(request: Request, seconds: number): void;
  stop(force?: boolean): Promise<void>;
  upgradeWebSocket(request: Request): AntWebSocketUpgrade;
  eventSource(): AntEventSourceStream;
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
  unsafe: AntUnsafe;
  stats(): AntStatsResult;
  suppressReporting(): void;

  sleep(seconds: number): void;
  msleep(milliseconds: number): void;
  usleep(microseconds: number): void;

  signal(signum: number, handler: (signum: number) => void): void;
  serve(options: AntServeOptions): AntServer;
}

declare const Ant: AntStatic;
