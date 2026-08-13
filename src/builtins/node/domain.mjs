import { EventEmitter } from 'node:events';

export const _stack = [];
export let active = null;

function setActive(domain) {
  active = domain || null;
  process.domain = active;
}

export class Domain extends EventEmitter {
  constructor() {
    super();
    this.members = [];
  }

  enter() {
    _stack.push(this);
    setActive(this);
  }

  exit() {
    const index = _stack.lastIndexOf(this);
    if (index !== -1) _stack.splice(index);
    setActive(_stack.length === 0 ? null : _stack[_stack.length - 1]);
  }

  add(emitter) {
    if (!emitter || this.members.includes(emitter)) return;
    if (emitter.domain && emitter.domain !== this && typeof emitter.domain.remove === 'function') {
      emitter.domain.remove(emitter);
    }
    this.members.push(emitter);
    emitter.domain = this;
  }

  remove(emitter) {
    const index = this.members.indexOf(emitter);
    if (index !== -1) this.members.splice(index, 1);
    if (emitter && emitter.domain === this) emitter.domain = null;
  }

  run(fn, ...args) {
    this.enter();
    try {
      return fn(...args);
    } finally {
      this.exit();
    }
  }

  bind(fn) {
    const domain = this;
    function bound(...args) {
      try {
        return domain.run(() => fn.apply(this, args));
      } catch (error) {
        domain.emit('error', error);
      }
    }
    bound.domain = domain;
    return bound;
  }

  intercept(fn) {
    const domain = this;
    return this.bind(function intercepted(error, ...args) {
      if (error) {
        domain.emit('error', error);
        return;
      }
      return fn.apply(this, args);
    });
  }
}

export function create() {
  return new Domain();
}

export const createDomain = create;

const domain = { _stack, Domain, createDomain, create };
Object.defineProperty(domain, 'active', {
  enumerable: true,
  configurable: true,
  get: () => active,
  set: setActive
});

export default domain;
