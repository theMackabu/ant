declare module 'events' {
  class EventEmitter {
    on(event: string, listener: (...args: unknown[]) => void): this;
    addListener(event: string, listener: (...args: unknown[]) => void): this;
    once(event: string, listener: (...args: unknown[]) => void): this;
    off(event: string, listener: (...args: unknown[]) => void): this;
    removeListener(event: string, listener: (...args: unknown[]) => void): this;
    emit(event: string, ...args: unknown[]): boolean;
    removeAllListeners(event?: string): this;
    listenerCount(event: string): number;
    eventNames(): string[];
  }
}
