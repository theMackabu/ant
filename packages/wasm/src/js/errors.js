const errorConstructors = { TypeError, SyntaxError, ReferenceError, RangeError };

export class AntTimeoutError extends Error {
  constructor(message = 'Execution timed out') {
    super(message);
    this.name = 'AntTimeoutError';
  }
}

export class AntDisposedError extends Error {
  constructor() {
    super('This Ant instance has been disposed');
    this.name = 'AntDisposedError';
  }
}

export function errorFrom(value) {
  const name = typeof value?.name === 'string' ? value.name : 'Error';
  const message = typeof value?.message === 'string' ? value.message : 'Ant evaluation failed';
  const error = new (Object.hasOwn(errorConstructors, name) ? errorConstructors[name] : Error)(message);
  error.name = name;
  if (typeof value?.stack === 'string') error.stack = value.stack;
  return error;
}

function readString(error, key) {
  try {
    const value = error[key];
    return typeof value === 'string' ? value : undefined;
  } catch {}
}

export function transferableError(error) {
  const isObject = error !== null && (typeof error === 'object' || typeof error === 'function');
  const name = (isObject && readString(error, 'name')) || 'Error';
  let message = isObject ? readString(error, 'message') : undefined;
  const stack = isObject ? readString(error, 'stack') : undefined;
  if (!message) {
    try {
      message = String(error);
    } catch {
      message = 'Host function failed';
    }
  }
  return stack === undefined ? { name, message } : { name, message, stack };
}
