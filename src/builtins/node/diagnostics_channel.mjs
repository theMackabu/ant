const registry = new Map();

const TRACING_EVENTS = ['start', 'end', 'asyncStart', 'asyncEnd', 'error'];

function createInvalidArgType(name, expected) {
  const error = new TypeError(`The "${name}" argument must be of type ${expected}.`);
  error.code = 'ERR_INVALID_ARG_TYPE';
  return error;
}

function validateFunction(value, name) {
  if (typeof value !== 'function') throw createInvalidArgType(name, 'function');
}

function validateChannelName(name) {
  if (typeof name === 'string' || typeof name === 'symbol') return;
  throw createInvalidArgType('channel', 'string or symbol');
}

function normalizeIndex(length, index) {
  if (index < 0) return Math.max(length + index, 0);
  return index;
}

function createTracingChannels(name) {
  const prefix = `tracing:${name}`;
  return {
    start: channel(`${prefix}:start`),
    end: channel(`${prefix}:end`),
    asyncStart: channel(`${prefix}:asyncStart`),
    asyncEnd: channel(`${prefix}:asyncEnd`),
    error: channel(`${prefix}:error`)
  };
}

export class Channel {
  constructor(name) {
    this.name = name;
    this._subscribers = [];
    this._stores = undefined;
  }

  get hasSubscribers() {
    return this._subscribers.length > 0 || !!(this._stores && this._stores.size > 0);
  }

  subscribe(fn) {
    validateFunction(fn, 'subscription');
    this._subscribers.push(fn);
  }

  unsubscribe(fn) {
    const index = this._subscribers.indexOf(fn);
    if (index === -1) return false;
    this._subscribers.splice(index, 1);
    return true;
  }

  publish(message) {
    const subscribers = this._subscribers.slice();
    for (const fn of subscribers) fn(message, this.name);
  }

  bindStore(store, transform) {
    if (!this._stores) this._stores = new Map();
    this._stores.set(store, transform);
  }

  unbindStore(store) {
    if (!this._stores) return false;
    return this._stores.delete(store);
  }

  runStores(context, fn, thisArg, ...args) {
    if (!this._stores || this._stores.size === 0) {
      this.publish(context);
      return fn.apply(thisArg, args);
    }

    const entries = Array.from(this._stores.entries());
    const run = index => {
      if (index === entries.length) {
        this.publish(context);
        return fn.apply(thisArg, args);
      }

      const [store, transform] = entries[index];
      return store.run(transform ? transform(context) : context, () => run(index + 1));
    };

    return run(0);
  }
}

export class TracingChannel {
  constructor(channels) {
    for (const event of TRACING_EVENTS) this[event] = channels[event];
  }

  get hasSubscribers() {
    return TRACING_EVENTS.some(event => this[event].hasSubscribers);
  }

  subscribe(subscribers) {
    for (const event of TRACING_EVENTS) if (subscribers[event] !== undefined) this[event].subscribe(subscribers[event]);
  }

  unsubscribe(subscribers) {
    let ok = true;

    for (const event of TRACING_EVENTS) {
      if (subscribers[event] !== undefined && !this[event].unsubscribe(subscribers[event])) ok = false;
    }

    return ok;
  }

  traceSync(fn, context = {}, thisArg, ...args) {
    if (!this.hasSubscribers) return fn.apply(thisArg, args);

    const { start, end, error } = this;

    return start.runStores(context, () => {
      try {
        const result = fn.apply(thisArg, args);
        context.result = result;
        end.publish(context);
        return result;
      } catch (err) {
        context.error = err;
        error.publish(context);
        end.publish(context);
        throw err;
      }
    });
  }

  tracePromise(fn, context = {}, thisArg, ...args) {
    if (!this.hasSubscribers) return fn.apply(thisArg, args);
    const { start, end, asyncStart, asyncEnd, error } = this;

    const resolve = result => {
      context.result = result;
      asyncStart.publish(context);
      asyncEnd.publish(context);
      return result;
    };

    const reject = err => {
      context.error = err;
      error.publish(context);
      asyncStart.publish(context);
      asyncEnd.publish(context);
      return Promise.reject(err);
    };

    return start.runStores(context, () => {
      try {
        let promise = fn.apply(thisArg, args);
        if (!(promise instanceof Promise)) promise = Promise.resolve(promise);
        const wrapped = promise.then(resolve, reject);
        end.publish(context);
        return wrapped;
      } catch (err) {
        context.error = err;
        error.publish(context);
        end.publish(context);
        throw err;
      }
    });
  }

  traceCallback(fn, position = -1, context = {}, thisArg, ...args) {
    if (!this.hasSubscribers) return fn.apply(thisArg, args);

    const { start, end, asyncStart, asyncEnd, error } = this;
    const callbackIndex = normalizeIndex(args.length, position);
    const callback = args[callbackIndex];

    validateFunction(callback, 'callback');

    args.splice(callbackIndex, 1, function wrappedCallback(err, result) {
      if (err) {
        context.error = err;
        error.publish(context);
      } else context.result = result;

      return asyncStart.runStores(context, () => {
        try {
          return callback.apply(this, arguments);
        } finally {
          asyncEnd.publish(context);
        }
      });
    });

    return start.runStores(context, () => {
      try {
        const result = fn.apply(thisArg, args);
        end.publish(context);
        return result;
      } catch (err) {
        context.error = err;
        error.publish(context);
        end.publish(context);
        throw err;
      }
    });
  }
}

export function channel(name) {
  validateChannelName(name);

  let ch = registry.get(name);
  if (!ch) {
    ch = new Channel(name);
    registry.set(name, ch);
  }

  return ch;
}

export function subscribe(name, fn) {
  channel(name).subscribe(fn);
}

export function unsubscribe(name, fn) {
  return channel(name).unsubscribe(fn);
}

export function hasSubscribers(name) {
  const ch = registry.get(name);
  return ch ? ch.hasSubscribers : false;
}

export function tracingChannel(nameOrChannels) {
  if (typeof nameOrChannels === 'string') nameOrChannels = createTracingChannels(nameOrChannels);
  return new TracingChannel(nameOrChannels);
}

export default { channel, subscribe, unsubscribe, hasSubscribers, tracingChannel, Channel, TracingChannel };
