// @ant: compact_on_build
import ops from 'ant:internal/shell_ops';
import { parseTemplate, parseSource } from 'ant:internal/shell/parser';
import { compileProgram, braceExpand } from 'ant:internal/shell/compiler';
import { R, Ctx, registry, ShellExit, ShellReturn, toShellError, runExitTrap, errorMessage, releaseSignalTraps } from 'ant:internal/shell/runtime';
import 'ant:internal/shell/builtins';

const compileCache = new WeakMap();
const sourceCache = new Map();
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const decoder = new TextDecoder();

function lower(ast, label) {
  const source = compileProgram(ast);
  if (ops.debugEnabled) {
    const plan = JSON.stringify(ast);
    console.error(`[shell:compile] ${label} JavaScript (${source.length} bytes)\n${source}\n[shell:compile] AST (${plan.length} bytes)\n${plan}`);
  }
  return new AsyncFunction('R', 'c0', 'V', source);
}

registry.compile = (text, name, extglob = false) => {
  const key = (extglob ? '+' : '-') + text;
  let fn = sourceCache.get(key);
  if (fn) return fn;
  fn = lower(parseSource(text, name, 0, extglob), name);
  if (sourceCache.size >= 512) sourceCache.delete(sourceCache.keys().next().value);
  sourceCache.set(key, fn);
  return fn;
};

async function execute(fn, values, options) {
  const c = Ctx.top(options);
  let status;
  try {
    try {
      await fn(R, c, values);
      status = c.status;
    } catch (error) {
      if (error instanceof ShellExit || error instanceof ShellReturn) status = error.status;
      else {
        const shellError = toShellError(error);
        if (!shellError) throw error;
        await errorMessage(c, shellError.message);
        status = shellError.fatal ? 127 : shellError.status;
      }
    }
    status = await runExitTrap(c, status);
  } finally {
    if (c.root.owned) for (const fd of c.root.owned) ops.close(fd);
    releaseSignalTraps(c.root);
  }
  return {
    stdout: c.root.out.bytes(),
    stderr: c.root.err.bytes(),
    exitCode: status,
    signalCode: status > 128 ? c.root.signal : null
  };
}

function outputText(bytes, encoding) {
  if (encoding && encoding !== 'utf8' && encoding !== 'utf-8') return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString(encoding);
  return decoder.decode(bytes);
}

const outputMethods = {
  text(encoding) {
    return outputText(this.stdout, encoding);
  },
  json() {
    return JSON.parse(this.text());
  },
  arrayBuffer() {
    return this.stdout.buffer;
  },
  bytes() {
    return new Uint8Array(this.stdout.buffer, this.stdout.byteOffset, this.stdout.byteLength);
  },
  blob() {
    return new Blob([this.stdout]);
  },
  lines() {
    const lines = this.text().split('\n');
    if (lines.length && lines[lines.length - 1] === '') lines.pop();
    return lines;
  }
};

class ShellOutput {
  constructor() {
    throw new TypeError('Illegal constructor');
  }
}

Object.assign(ShellOutput.prototype, outputMethods);

class ShellError extends Error {
  constructor(output) {
    super(`Shell command failed with exit code ${output.exitCode}`);
    this.name = 'ShellError';
    this.exitCode = output.exitCode;
    this.stdout = output.stdout;
    this.stderr = output.stderr;
    this.signalCode = output.signalCode;
  }
}

Object.assign(ShellError.prototype, outputMethods);

function shellOutput(result) {
  Object.setPrototypeOf(result, ShellOutput.prototype);
  return result;
}

class ShellPromise {
  constructor(fn, values, defaults) {
    this.state = { fn, values, env: defaults.env, cwd: defaults.cwd, throws: defaults.throws, quiet: defaults.quiet, promise: null };
    queueMicrotask(() => this.run());
  }

  run() {
    const state = this.state;
    if (!state.promise) {
      state.promise = execute(state.fn, state.values, { env: state.env, cwd: state.cwd, print: !state.quiet }).then(result => {
        const output = shellOutput(result);
        if (output.exitCode === 0 || !state.throws) return output;
        throw new ShellError(output);
      });
    }
    return state.promise;
  }

  then(onFulfilled, onRejected) {
    return this.run().then(onFulfilled, onRejected);
  }

  catch(onRejected) {
    return this.run().catch(onRejected);
  }

  finally(onFinally) {
    return this.run().finally(onFinally);
  }

  nothrow() {
    this.state.throws = false;
    return this;
  }

  throws(value = true) {
    this.state.throws = !!value;
    return this;
  }

  quiet(value = true) {
    this.state.quiet = !!value;
    return this;
  }

  env(values) {
    this.state.env = values === undefined ? null : values;
    return this;
  }

  cwd(directory) {
    this.state.cwd = directory === undefined ? null : directory;
    return this;
  }

  text(encoding) {
    return this.run().then(output => output.text(encoding));
  }

  json() {
    return this.run().then(output => output.json());
  }

  arrayBuffer() {
    return this.run().then(output => output.arrayBuffer());
  }

  bytes() {
    return this.run().then(output => output.bytes());
  }

  blob() {
    return this.run().then(output => output.blob());
  }

  lines() {
    const promise = this.run().then(output => output.lines());
    const run = this.run();
    promise[Symbol.asyncIterator] = async function* () {
      const output = await run;
      for (const line of output.text().split('\n')) yield line;
    };
    return promise;
  }
}

Object.defineProperty(ShellOutput, 'name', { value: 'ShellOutput' });
Object.defineProperty(ShellPromise, 'name', { value: 'ShellPromise' });
Object.defineProperty(ShellError, 'name', { value: 'ShellError' });

function templateSegments(strings) {
  if (!Array.isArray(strings)) throw new TypeError('$() must be used as a tagged template');
  const segments = Array.isArray(strings.raw) ? strings.raw : strings;
  if (segments.length === 0) throw new TypeError('$() must be used as a tagged template');
  for (const segment of segments) {
    if (typeof segment !== 'string') throw new TypeError('$() must be used as a tagged template');
  }
  return segments;
}

function rawValue(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value) && typeof value.raw === 'string' && Object.keys(value).length === 1 ? value.raw : null;
}

function compiled(strings, values) {
  const segments = templateSegments(strings);
  let raws = null;
  let key = '';
  for (let i = 0; i < values.length; i++) {
    const raw = rawValue(values[i]);
    if (raw === null) continue;
    (raws ??= new Map()).set(i, raw);
    key += `${i}:${raw.length}:${raw}`;
  }
  let entry = compileCache.get(strings);
  if (!entry) {
    entry = { plain: null, raw: null };
    compileCache.set(strings, entry);
  }
  if (!raws) return (entry.plain ??= lower(parseTemplate(segments), 'template'));
  entry.raw ??= new Map();
  let fn = entry.raw.get(key);
  if (!fn) {
    fn = lower(parseTemplate(segments, raws), 'template');
    if (entry.raw.size >= 64) entry.raw.delete(entry.raw.keys().next().value);
    entry.raw.set(key, fn);
  }
  return fn;
}

const ESCAPE_NEEDED = /[^A-Za-z0-9_@%+=:,./-]/;

function escape(value) {
  const text = String(value);
  if (text === '') return '""';
  if (!ESCAPE_NEEDED.test(text)) return text;
  return `"${text.replace(/[\\$`"]/g, '\\$&')}"`;
}

function braces(pattern) {
  return braceExpand([{ t: 'lit', v: String(pattern), q: false }]).map(word => word.map(part => part.v).join(''));
}

function createShell(defaults) {
  const shell = function $(strings, ...values) {
    return new ShellPromise(compiled(strings, values), values, shell.defaults);
  };
  shell.defaults = defaults;
  shell.env = values => {
    shell.defaults = { ...shell.defaults, env: values === undefined ? null : values };
    return shell;
  };
  shell.cwd = directory => {
    shell.defaults = { ...shell.defaults, cwd: directory === undefined ? null : directory };
    return shell;
  };
  shell.nothrow = () => {
    shell.defaults = { ...shell.defaults, throws: false };
    return shell;
  };
  shell.throws = (value = true) => {
    shell.defaults = { ...shell.defaults, throws: !!value };
    return shell;
  };
  shell.quiet = (value = true) => {
    shell.defaults = { ...shell.defaults, quiet: !!value };
    return shell;
  };
  shell.escape = escape;
  shell.braces = braces;
  shell.Shell = function Shell() {
    if (!new.target) throw new TypeError("Class constructor Shell cannot be invoked without 'new'");
    return createShell({ env: null, cwd: null, throws: true, quiet: true });
  };
  shell.ShellError = ShellError;
  shell.ShellPromise = ShellPromise;
  shell.ShellOutput = ShellOutput;
  return shell;
}

export const $ = createShell({ env: null, cwd: null, throws: true, quiet: true });

export { ShellError, ShellPromise, ShellOutput };

export default { $ };
