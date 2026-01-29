declare function setTimeout<T extends any[]>(callback: (...args: T) => void, delay: number, ...args: T): number;
declare function setInterval<T extends any[]>(callback: (...args: T) => void, delay: number, ...args: T): number;

declare function clearTimeout(timerId: number): void;
declare function clearInterval(timerId: number): void;

declare function setImmediate(callback: () => void): number;
declare function clearImmediate(immediateId: number): void;
declare function queueMicrotask(callback: () => void): void;
