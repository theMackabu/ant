declare module 'worker_threads' {
  interface MessagePort {
    postMessage(value: unknown): void;
    on(event: 'message', listener: (value: unknown) => void): this;
    once(event: 'message', listener: (value: unknown) => void): this;
    start(): void;
    close(): void;
    ref(): void;
    unref(): void;
    onmessage: ((event: { data: unknown }) => void) | undefined;
  }

  class MessageChannel {
    port1: MessagePort;
    port2: MessagePort;
  }

  class MessagePort {
    private constructor();
  }

  interface WorkerOptions {
    workerData?: unknown;
  }

  class Worker {
    constructor(filename: string | URL, options?: WorkerOptions);
    on(event: 'message' | 'exit', listener: (...args: unknown[]) => void): this;
    once(event: 'message' | 'exit', listener: (...args: unknown[]) => void): this;
    terminate(): Promise<number>;
    ref(): this;
    unref(): this;
    postMessage(value: unknown): void;
  }

  const isMainThread: boolean;
  const threadId: number;
  const parentPort: {
    postMessage(value: unknown): void;
    ref(): void;
    unref(): void;
  } | null;
  const workerData: unknown;
  const SHARE_ENV: symbol;

  function markAsUntransferable<T>(value: T): T;
  function receiveMessageOnPort(port: MessagePort): { message: unknown } | undefined;
  function setEnvironmentData(key: unknown, value: unknown): void;
  function getEnvironmentData(key: unknown): unknown;
  function moveMessagePortToContext(port: MessagePort, contextifiedSandbox: unknown): MessagePort;
}

declare module 'ant:worker_threads' {
  export * from 'worker_threads';
}

declare module 'node:worker_threads' {
  export * from 'worker_threads';
}
