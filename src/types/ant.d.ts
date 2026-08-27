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
  | 'promise'
  | 'generator'
  | 'err'
  | 'typedarray'
  | 'functioninfo'
  | 'map'
  | 'set'
  | 'weakmap'
  | 'weakset'
  | 'sourcecode';

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

type AntCNumberType =
  | 'int8'
  | 'uint8'
  | 'int16'
  | 'uint16'
  | 'int'
  | 'int32'
  | 'uint32'
  | 'float'
  | 'double';

type AntCBigIntType = 'int64' | 'uint64';

type AntCArgumentType =
  | AntCNumberType
  | AntCBigIntType;

type AntCReturnType = 'string' | AntCArgumentType;
type AntCReturnValue<T extends AntCReturnType> =
  T extends 'string' ? string | null :
  T extends AntCBigIntType ? bigint : number;
type AntCArgumentValues<T extends readonly AntCArgumentType[]> = {
  [K in keyof T]: T[K] extends AntCBigIntType ? bigint : number;
};

interface AntCOptions<TReturn extends AntCReturnType> {
  entry: string;
  returns: TReturn;
}

interface AntCFunctionOptions<
  TReturn extends AntCReturnType,
  TArgs extends readonly AntCArgumentType[],
> extends AntCOptions<TReturn> {
  args: TArgs;
}

interface AntUnsafe {
  c(strings: TemplateStringsArray, ...values: unknown[]): number;
  c<
    TReturn extends AntCReturnType,
    const TArgs extends readonly AntCArgumentType[],
  >(options: AntCFunctionOptions<TReturn, TArgs>):
    (strings: TemplateStringsArray, ...values: unknown[]) =>
      (...args: AntCArgumentValues<TArgs>) => AntCReturnValue<TReturn>;
  c<TReturn extends AntCReturnType>(options: AntCOptions<TReturn>):
    (strings: TemplateStringsArray, ...values: unknown[]) => AntCReturnValue<TReturn>;
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

interface AntServerReloadOptions {
  fetch(request: Request, server: AntServer): Response | Promise<Response>;
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
  reload(options: AntServerReloadOptions): void;
  upgradeWebSocket(request: Request): AntWebSocketUpgrade;
  eventSource(): AntEventSourceStream;
}

interface AntCronOptions {
  tz?: string;
}

type AntCronSchedule =
  | '@yearly'
  | '@annually'
  | '@monthly'
  | '@weekly'
  | '@daily'
  | '@midnight'
  | '@hourly'
  | '* * * * *'
  | '0 * * * *'
  | '0 0 * * *'
  | '0 0 * * 0'
  | '0 0 1 * *'
  | '0 0 1 1 *'
  | `${string} ${string} ${string} ${string} ${string}`
  | (string & {});

interface AntCronJob {
  readonly cron: string;
  ref(): AntCronJob;
  stop(): AntCronJob;
  unref(): AntCronJob;
  [Symbol.dispose](): void;
}

interface AntCronController {
  readonly cron: string;
  readonly type: 'scheduled';
  readonly scheduledTime: number;
}

interface AntCron {
  (
    schedule: AntCronSchedule,
    handler: (this: AntCronJob) => unknown,
    options?: AntCronOptions,
  ): AntCronJob;
  (path: string, schedule: AntCronSchedule, title: string): Promise<void>;
  parse(
    expression: AntCronSchedule,
    relativeDate?: Date | number,
    options?: AntCronOptions,
  ): Date | null;
  remove(title: string): Promise<void>;
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
  cron: AntCron;
}

declare const Ant: AntStatic;
