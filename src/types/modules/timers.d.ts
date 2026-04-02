declare module 'timers' {
  export const setTimeout: typeof globalThis.setTimeout;
  export const clearTimeout: typeof globalThis.clearTimeout;
  export const setInterval: typeof globalThis.setInterval;
  export const clearInterval: typeof globalThis.clearInterval;
  export const setImmediate: typeof globalThis.setImmediate;
  export const clearImmediate: typeof globalThis.clearImmediate;
  export const queueMicrotask: typeof globalThis.queueMicrotask;
}

declare module 'ant:timers' {
  export * from 'timers';
}

declare module 'node:timers' {
  export * from 'timers';
}
