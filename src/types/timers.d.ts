declare function setTimeout(callback: () => void, delay: number): number;
declare function clearTimeout(timerId: number): void;
declare function setInterval(callback: () => void, delay: number): number;
declare function clearInterval(timerId: number): void;
declare function setImmediate(callback: () => void): number;
declare function clearImmediate(immediateId: number): void;
declare function queueMicrotask(callback: () => void): void;
