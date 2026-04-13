declare module 'events' {
  interface Disposable {
    dispose(): void;
  }

  interface EventListenerOptions {
    signal?: AbortSignal;
  }

  class EventEmitter {
    on(event: string, listener: (...args: unknown[]) => void): this;
    addListener(event: string, listener: (...args: unknown[]) => void): this;
    once(event: string, listener: (...args: unknown[]) => void): this;
    off(event: string, listener: (...args: unknown[]) => void): this;
    removeListener(event: string, listener: (...args: unknown[]) => void): this;
    emit(event: string, ...args: unknown[]): boolean;
    removeAllListeners(event?: string): this;
    listenerCount(event: string): number;
    listeners(event: string): Array<(...args: unknown[]) => void>;
    eventNames(): string[];
  }

  function once(
    emitter: EventEmitter | EventTarget | AbortSignal,
    eventName: string | symbol,
    options?: EventListenerOptions
  ): Promise<unknown[]>;
  function on(
    emitter: EventEmitter | EventTarget,
    eventName: string | symbol,
    options?: EventListenerOptions
  ): AsyncIterableIterator<unknown[]>;
  function addAbortListener(signal: AbortSignal, listener: (event: Event) => void): Disposable;
  function setMaxListeners(n: number, ...eventTargets: Array<EventEmitter | EventTarget>): void;
  function getMaxListeners(eventTarget: EventEmitter | EventTarget): number;
}

declare module 'ant:events' {
  export * from 'events';
}

declare module 'node:events' {
  export * from 'events';
}
