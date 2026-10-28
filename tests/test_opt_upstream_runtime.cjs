const assert = require('node:assert');

const repeat = 30;
const functionCache = new Map();

function cachedFunction(...args) {
  const key = args.map(String).join('\u0000');
  let f = functionCache.get(key);
  if (!f) {
    f = new Function(...args);
    functionCache.set(key, f);
  }
  return f;
}

const jit = {
  enabled: false,
  prepare() {},
  compile() {
    return false;
  },
  neverCompile() {},
};

globalThis.cachedFunction = cachedFunction;
globalThis.jit = jit;

function describeForComparison(value, depth = 0) {
  if (typeof value === 'bigint') return `${value}n`;
  if (typeof value === 'symbol') return value.toString();
  if (typeof value === 'string') return JSON.stringify(value);
  if (typeof value === 'function') return `function ${value.name}`;
  if (Object.is(value, -0)) return '-0';
  if (typeof value !== 'object' || value === null) return String(value);
  if (depth > 2) return '...';
  if (Array.isArray(value)) {
    const elements = [];
    for (let i = 0; i < value.length && i < 16; ++i) elements.push(i in value ? describeForComparison(value[i], depth + 1) : '<hole>');
    return `[${elements.join(', ')}]`;
  }
  if (value instanceof Error) return `${value.constructor.name}: ${value.message}`;
  const properties = Object.keys(value).slice(0, 16).map((key) => `${key}: ${describeForComparison(value[key], depth + 1)}`);
  return `{${properties.join(', ')}}`;
}

function compareCompiledWithInterpreter(factory, { warmUp, probes = {}, warmUpCalls = 12 }) {
  const reference = new Function(`return (${factory})`)()();
  const compiled = cachedFunction(`return (${factory})`)()();
  const outcome = (f, makeArguments, index) => {
    const log = [];
    let result;
    try {
      result = `returned ${describeForComparison(f(...makeArguments(log, index)))}`;
    } catch (e) {
      result = `threw ${describeForComparison(e)}`;
    }
    return `${result} [${log.join(', ')}]`;
  };
  const failures = [];
  const compare = (label, makeArguments, index) => {
    const expected = outcome(reference, makeArguments, index);
    const actual = outcome(compiled, makeArguments, index);
    if (expected !== actual) failures.push(`${label} (call ${index}): expected ${expected}, got ${actual}`);
  };
  for (let i = 0; i < warmUpCalls; ++i) compare('warm up', warmUp, i);
  compare('compiled', warmUp, warmUpCalls);
  for (const [label, makeArguments] of Object.entries(probes)) {
    for (let i = 0; i < 2; ++i) compare(label, makeArguments, i);
    for (let i = 0; i < 2; ++i) compare(`warm up after ${label}`, warmUp, i);
  }
  return failures;
}

function expect(target) {
  const fail = (message) => {
    throw new Error(message);
  };
  return {
    toBe(value) {
      if (!Object.is(target, value)) fail(`expected ${describeForComparison(value)}, got ${describeForComparison(target)}`);
    },
    toEqual(value) {
      if (describeForComparison(target) !== describeForComparison(value))
        fail(`expected ${describeForComparison(value)}, got ${describeForComparison(target)}`);
    },
    toBeNaN() {
      if (!Number.isNaN(target)) fail(`expected NaN, got ${describeForComparison(target)}`);
    },
    toBeTrue() {
      if (target !== true) fail(`expected true, got ${describeForComparison(target)}`);
    },
    toThrow(type) {
      try {
        target();
      } catch (e) {
        if (type && !(e instanceof type)) fail(`expected ${type.name}, got ${describeForComparison(e)}`);
        return;
      }
      fail('expected an exception');
    },
  };
}

const tests = [];

function test(name, body) {
  tests.push([name, body]);
}

test("compiled comparisons of doubles that held int32 values stay exact", () => {
    const keys = new Float64Array([1, 2, 3, 4]);
    const countBelow = cachedFunction(
        "keys",
        "limit",
        `let count = 0;
        for (let i = 0; i < keys.length; ++i) {
            if (keys[i] < limit) count = (count + 1) | 0;
        }
        return count;`
    );
    jit.prepare(countBelow);
    for (let k = 0; k < 10; ++k) countBelow(keys, k);
    expect(jit.compile(countBelow)).toBe(jit.enabled);
    expect(countBelow(keys, 2.5)).toBe(2);
    keys.set([0.5, -0, NaN, 3.25]);
    expect(countBelow(keys, 0)).toBe(0);
    expect(countBelow(keys, 1)).toBe(2);
    expect(countBelow(keys, 3.5)).toBe(3);
    expect(countBelow(keys, Infinity)).toBe(3);
});

test("compiled arithmetic on doubles that held int32 values stays exact", () => {
    const values = new Float64Array([1, 2, 3, 4]);
    const sum = cachedFunction(
        "values",
        `let total = 0;
        for (let i = 0; i < values.length; ++i) total += values[i];
        return total;`
    );
    jit.prepare(sum);
    for (let k = 0; k < 10; ++k) sum(values);
    expect(jit.compile(sum)).toBe(jit.enabled);
    expect(sum(values)).toBe(10);
    values.set([0.5, 0.25, -0, 1e300]);
    expect(sum(values)).toBe(0.75 + 1e300);
    values.set([NaN, 1, 2, 3]);
    expect(sum(values)).toBeNaN();
});

test("unused int32 arithmetic keeps converting operands with effects", () => {
    const add = cachedFunction("a", "b", "const sum = a + b; return 1;");
    jit.prepare(add);
    for (let k = 0; k < 10; ++k) add(k, 2);
    jit.compile(add);
    let calls = 0;
    expect(add({ valueOf: () => ++calls }, 1)).toBe(1);
    expect(calls).toBe(1);
    expect(() => add(Symbol(), 1)).toThrow(TypeError);
    expect(() => add(1n, 1)).toThrow(TypeError);
});

test("doubles that are both compared and used as indices keep their checks", () => {
    const lookup = cachedFunction(
        "keys",
        "table",
        "limit",
        `let total = 0;
        for (let i = 0; i < keys.length; ++i) {
            if (keys[i] < limit) total += table[keys[i]];
        }
        return total;`
    );
    const keys = new Float64Array([0, 1, 2, 3]);
    const table = [10, 20, 30, 40];
    jit.prepare(lookup);
    for (let k = 0; k < 10; ++k) lookup(keys, table, 4);
    jit.compile(lookup);
    expect(lookup(keys, table, 4)).toBe(100);
    keys.set([1, 1, 2.5, 3]);
    expect(lookup(keys, table, 2)).toBe(40);
    expect(lookup(keys, table, 4)).toBeNaN();
});

test("compiled identities of doubles that held int32 values convert them", () => {
    const midpoint = cachedFunction("low", "high", "return ((low + high) / 2) | 0;");
    const plusZero = cachedFunction("values", "return values[0] + 0;");
    const shifted = cachedFunction("values", "return (values[0] << 0) + (values[0] >> 0) + (values[0] ^ 0);");
    const values = new Float64Array([4]);
    for (const f of [midpoint, plusZero, shifted]) jit.prepare(f);
    for (let k = 0; k < 10; ++k) {
        midpoint(k, k + 2);
        plusZero(values);
        shifted(values);
    }
    for (const f of [midpoint, plusZero, shifted]) expect(jit.compile(f)).toBe(jit.enabled);
    expect(midpoint(1, 2)).toBe(1);
    expect(midpoint(-1, -2)).toBe(-1);
    expect(midpoint(0, NaN)).toBe(0);
    expect(midpoint(2 ** 32, 2 ** 32 + 6)).toBe(3);
    values[0] = -0;
    expect(Object.is(plusZero(values), 0)).toBeTrue();
    values[0] = 2.5;
    expect(plusZero(values)).toBe(2.5);
    expect(shifted(values)).toBe(6);
    values[0] = -(2 ** 31) - 1.5;
    expect(shifted(values)).toBe(3 * (2 ** 31 - 1));
});

test("folds that give doubles checked to be int32 values int32 users keep the checks", () => {
    const sum = cachedFunction(
        "a",
        "b",
        "count",
        "start",
        `let y = +(a * b);
        let s = start;
        for (let i = 0; i < count; i++) {
            let t = y;
            if (i > 1000) t = y;
            y = t;
            s = s + y;
        }
        return s;`
    );
    jit.prepare(sum);
    for (let k = 0; k < 10; ++k) {
        sum(1.5, 2, 3, 0);
        sum(1.5, 2, 2, "a");
    }
    expect(jit.compile(sum)).toBe(jit.enabled);
    expect(sum(1.5, 2, 3, 0)).toBe(9);
    expect(sum(1.5, 2, 2, "a")).toBe("a33");
    expect(sum(1.25, 2, 2, 1)).toBe(6);
    expect(sum(1.25, 2, 2, "b")).toBe("b2.52.5");
});

test("large frames with many arithmetic slow paths", () => {
    let body = "let result = input;";
    for (let i = 0; i < 1000; ++i) body += `result = result + ${i + 0.123456789};`;
    body += "return result;";
    const sum = cachedFunction("input", body);
    const reference = cachedFunction("input", body);
    jit.neverCompile(reference);
    jit.prepare(sum);
    expect(sum(1)).toBe(reference(1));
    expect(sum("x")).toBe(reference("x"));
    expect(jit.compile(sum)).toBe(jit.enabled);
    expect(sum(1)).toBe(reference(1));
    expect(sum("x")).toBe(reference("x"));
    expect(sum({ valueOf: () => 2 })).toBe(reference(2));
    expect(() => sum(Symbol())).toThrow(TypeError);
    expect(sum(3)).toBe(reference(3));
});

test("exits in inlined getters", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            class Box {
                constructor(value) {
                    this.raw = value;
                }
                get doubled() {
                    return this.raw * 2;
                }
            }
            return value => {
                const box = new Box(value);
                return [box.doubled + 1, box.raw];
            };
        },
        {
            warmUp: (log, i) => [i],
            probes: {
                double: () => [0.5],
                string: () => ["4"],
                overflow: () => [2147483647],
                undefined: () => [undefined],
            },
        }
    );
    expect(failures).toEqual([]);
});

test("exits in inlined setters", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const prototype = {
                set value(v) {
                    if (v > 100) throw new RangeError(`too big: ${v}`);
                    this.stored = v + 1;
                },
            };
            return (value, log) => {
                const object = Object.create(prototype);
                try {
                    object.value = value;
                } catch (e) {
                    log.push(e.message);
                }
                return object.stored;
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: { throwing: log => [1000, log], double: log => [1.25, log], string: log => ["9", log] },
        }
    );
    expect(failures).toEqual([]);
});

test("an object holding another one across a loop", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            return values => {
                const outer = { inner: { count: 1 } };
                let sum = 0;
                for (const value of values) sum += value;
                if (typeof sum !== "number") return [sum, outer.inner.count];
                return [sum];
            };
        },
        {
            warmUp: (log, i) => [[i, 1]],
            probes: { double: () => [[0.5, 1]], string: () => [["s", 1]], empty: () => [[]] },
        }
    );
    expect(failures).toEqual([]);
});

test("an object holding another one written in a loop", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            return values => {
                const outer = { inner: { count: 0 } };
                let sum = 0;
                for (const value of values) {
                    if (value > 100) outer.inner = { count: value };
                    sum += value;
                }
                if (typeof sum !== "number") return [sum, outer.inner.count];
                return [sum];
            };
        },
        {
            warmUp: (log, i) => [[i, 1, i + 2]],
            probes: { double: () => [[0.5, 2.5]], string: () => [["s", 300]], big: () => [[200, 1]] },
        }
    );
    expect(failures).toEqual([]);
});

test("caller locals after an inlined callee throws", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const check = value => {
                if (typeof value !== "number") throw new TypeError(`not a number: ${typeof value}`);
                return value + 1;
            };
            return (value, log) => {
                let before = value * 3;
                let after = 0;
                try {
                    after = check(value) + before;
                    before = -1;
                } catch (e) {
                    log.push(`caught with before=${before} after=${after}`);
                } finally {
                    log.push(`finally with before=${before} after=${after}`);
                }
                return [before, after];
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: { string: log => ["x", log], undefined: log => [undefined, log], double: log => [2.5, log] },
        }
    );
    expect(failures).toEqual([]);
});

test("a method that first reads this inside a loop is inlined", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const object = {
                n: 2,
                loop() {
                    for (var i = 0; i < this.n; i++) {}
                    return this.n;
                },
            };
            return n => {
                object.n = n;
                return object.loop();
            };
        },
        { warmUp: (log, i) => [i % 4], probes: { string: () => ["3"], double: () => [2.5] } }
    );
    expect(failures).toEqual([]);
});

function makeCallee(parameters, body) {
    return cachedFunction("pad", ...parameters, `if (pad === 1) { ${"pad = pad + 1; ".repeat(16)}} ${body}`);
}

function testPrototypeCheck(check) {
    const callee = makeCallee(["target", "object", "hasOwnProperty"], `return [${check}, object.name];`);
    const caller = (target, object) => callee(undefined, target, object, Object.prototype.hasOwnProperty);
    const target = function target() {};
    const object = Object.create(target);
    jit.prepare(callee);
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) expect(caller({}, object)).toEqual([false, "target"]);
    jit.compile(callee);
    jit.compile(caller);
    expect(caller(target, { name: "other" })).toEqual([true, "other"]);
}

test("in", () => {
    testPrototypeCheck(`"prototype" in target`);
});

test("hasOwnProperty", () => {
    testPrototypeCheck(`hasOwnProperty.call(target, "prototype")`);
});

test("native results returned through inlined frames", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const inner = (array, value) => array.push(value) + Math.max(value, array.length);
            const middle = (array, value) => inner(array, value) * 2;
            return (value, log) => {
                const array = [1, 2];
                const result = middle(array, value);
                log.push(array.length);
                return [result, String(value).charCodeAt(0), array];
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: {
                double: log => [1.5, log],
                string: log => ["7", log],
                object: log => [{ valueOf: () => (log.push("valueOf"), 3) }, log],
                overflow: log => [2147483647, log],
            },
        }
    );
    expect(failures).toEqual([]);
});

test("the last initialization of a duplicate key wins in compiled code", () => {
    const constants = cachedFunction("return { a: 1, a: 2, a: 3 };");
    const mixed = cachedFunction("x", "return { a: x, a: 3 };");
    const mixedReversed = cachedFunction("x", "return { a: 3, a: x };");
    for (const f of [constants, mixed, mixedReversed]) jit.prepare(f);
    for (let i = 0; i < 5; ++i) {
        constants();
        mixed(i);
        mixedReversed(i);
    }
    for (const f of [constants, mixed, mixedReversed]) jit.compile(f);
    for (let i = 0; i < 5; ++i) {
        expect(constants().a).toBe(3);
        expect(mixed(i).a).toBe(3);
        expect(mixedReversed(i).a).toBe(i);
        expect(Object.keys(mixed(i))).toEqual(["a"]);
    }
});

test("slow path calls pass inputs that share the output register", () => {
    const out = [];
    class Shape0 {}
    function f0(p) {
        let v0 = new Shape0();
        let v1 = ((p !== p) | v0) >> (p * p == "x" in Object(v0));
        for (let i = 0; i < 3; ++i) {
            if ((p >>> v1) * ((v1 ^ 0) <= p - -2147483648)) {
                out.push("t" + i);
            } else {
                out.push(arguments.length);
                p *= (v1 - i != typeof v0) / (p | 0);
            }
        }
    }
    jit.prepare(f0);
    for (let i = 0; i < 5; ++i) f0(new Shape0());
    jit.compile(f0);
    out.length = 0;
    f0({
        valueOf() {
            return 4;
        },
    });
    expect(out).toEqual(["t0", "t1", "t2"]);
});

test("slow path calls keep spilled values when their output register is preserved by calls", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const names = Array.from({ length: 26 }, (_, i) => `a${i}`);
            const checks = [...names.slice(0, 18), "x", ...names.slice(18)].map(
                name => `if (${name} === null) return 0;`
            );
            const results = names.map(name => `${name} + 1`);
            return cachedFunction("x", ...names, `${checks.join("\n")} const y = x + 1; return [${results}, y].join();`);
        },
        {
            warmUp: (log, i) => Array(27).fill(i % 3 ? i : { valueOf: () => 5 }),
            probes: { object: () => [{ valueOf: () => 5 }, ...Array.from({ length: 26 }, (_, i) => i + 1)] },
        }
    );
    expect(failures).toEqual([]);
});

function runTests() {
  let failed = 0;
  for (const [name, body] of tests) {
    try {
      for (let i = 0; i < repeat; ++i) body();
      console.log(`ok ${name}`);
    } catch (e) {
      failed++;
      console.log(`FAIL ${name}: ${e.message}`);
    }
  }
  return failed;
}

if (process.argv[2] === 'child') {
  process.exitCode = runTests() ? 1 : 0;
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  delete base.FORCE_COLOR;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const reference = run({}, ['--jitless']);
  assert.strictEqual(reference.status, 0, reference.stdout + reference.stderr);

  const modes = [
    ['default', {}],
    ['verify', { ANT_DEBUG: 'opt:verify' }],
    ['force', { ANT_DEBUG: 'opt:force,verify' }],
    ['off', { ANT_DEBUG: 'opt:off' }],
  ];
  for (const [name, env] of modes) {
    const child = run(env);
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, reference.stdout, `${name}: output differs from the interpreter`);
    console.log(`PASS opt upstream runtime ${name}`);
  }
}
