declare module 'ant:sandbox' {
  interface SandboxOptions {
    mount?: string | string[];
    write?: string | string[];
    forward?: string | string[];
    cwd?: string;
    memory?: number | `${number}${'kb' | 'mb' | 'gb' | 'kib' | 'mib' | 'gib'}`;
    memoryMb?: number;
    timeoutMs?: number;
    cpuTimeMs?: number;
    bootTimeoutMs?: number;
    verbose?: boolean;
    tty?: boolean;
    ttyRows?: number;
    ttyCols?: number;
    color?: 'auto' | 'force' | 'strip' | 'preserve';
  }

  type MessageHandler = (message: unknown) => void;

  interface SandboxStats {
    cpuTimeMs: number;
    wallTimeMs: number;
    residentMemory?: number;
  }

  class Sandbox {
    constructor(options?: SandboxOptions | string);
    onmessage?: MessageHandler;
    on(event: 'message', handler: MessageHandler): this;
    send(value: unknown): void;
    receive<T = unknown>(): Promise<T>;
    once<T = unknown>(type: string): Promise<T>;
    readonly messages: AsyncIterable<unknown>;
    stats(): SandboxStats;
    run(entry: string, argv?: string[]): Promise<number>;
    eval(source: string): Promise<unknown>;
    close(): Promise<void>;
    terminate(): Promise<void>;
  }

  interface SandboxParentPort {
    onmessage?: MessageHandler;
    on(event: 'message', handler: MessageHandler): this;
    send(value: unknown): void;
    close(): void;
  }

  const parentPort: SandboxParentPort | undefined;
}
