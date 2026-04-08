type ProcessEnv = {
  [key: string]: string | undefined;
} & {
  toObject(): Record<string, string>;
  toString(): string;
};

interface Features {
  uv: boolean;
  tls: string;
  typescript: 'transform' | 'none';
}

interface Versions {
  node: string;
  ant: string;
  v8: string;
  uv: string;
  modules: string;
}

interface Release {
  name: string;
}

interface MemoryUsage {
  rss: number;
  heapTotal: number;
  heapUsed: number;
  external: number;
  arrayBuffers: number;
}

interface CpuUsage {
  user: number;
  system: number;
}

interface ReadStream {
  isTTY: boolean;
  setRawMode(enable?: boolean): boolean;
  resume(): this;
  pause(): this;
  on(event: string, listener: EventListener): this;
  off(event: string, listener: EventListener): this;
  removeListener(event: string, listener: EventListener): this;
  removeAllListeners(event?: string): this;
}

interface WriteStream {
  isTTY: boolean;
  rows: number;
  columns: number;
  write(data: string): boolean;
  on(event: string, listener: EventListener): this;
  once(event: string, listener: EventListener): this;
  off(event: string, listener: EventListener): this;
  removeListener(event: string, listener: EventListener): this;
  removeAllListeners(event?: string): this;
  getWindowSize(): [number, number];
}

interface StderrStream extends Omit<WriteStream, 'rows' | 'columns' | 'getWindowSize'> {}

interface HrTime {
  (time?: [number, number]): [number, number];
  bigint(): bigint;
}

interface MemoryUsageFn {
  (): MemoryUsage;
  rss(): number;
}

interface Process {
  env: ProcessEnv;
  argv: string[];
  execArgv: string[];
  argv0: string;
  execPath: string;
  pid: number;
  ppid: number;
  platform: string;
  arch: string;
  version: string;
  versions: Versions;
  release: Release;
  features: Features;

  stdin: ReadStream;
  stdout: WriteStream;
  stderr: StderrStream;

  exit(code?: number): never;
  abort(): never;
  cwd(): string;
  chdir(directory: string): void;
  uptime(): number;
  hrtime: HrTime;
  memoryUsage: MemoryUsageFn;
  cpuUsage(previousValue?: CpuUsage): CpuUsage;
  kill(pid: number, signal?: number | string): true;
  umask(mask?: number): number;
  dlopen(module: { exports?: unknown }, filename: string): void;

  on(event: string, listener: EventListener): this;
  addListener(event: string, listener: EventListener): this;
  once(event: string, listener: EventListener): this;
  off(event: string, listener: EventListener): this;
  removeListener(event: string, listener: EventListener): this;
  removeAllListeners(event?: string): this;
  emit(event: string, ...args: unknown[]): boolean;
  listenerCount(event: string): number;
  setMaxListeners(n: number): this;
  getMaxListeners(): number;

  getuid(): number;
  geteuid(): number;
  getgid(): number;
  getegid(): number;
  getgroups(): number[];
  setuid(id: number | string): void;
  setgid(id: number | string): void;
  seteuid(id: number | string): void;
  setegid(id: number | string): void;
  setgroups(groups: Array<number | string>): void;
  initgroups(user: string, extraGroup: number | string): void;
}

declare const process: Process;
