function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

async function main() {
  const trace = [];

  async function leaf(label) {
    const local = `leaf-${label}`;
    trace.push(`leaf-before:${local}`);
    const resumed = await new Promise((resolve) => setTimeout(() => resolve(label.toUpperCase()), 0));
    trace.push(`leaf-after:${local}:${resumed}`);
    return `${local}:${resumed}`;
  }

  async function middle(label) {
    const local = `middle-${label}`;
    trace.push(`middle-enter:${local}`);
    try {
      const value = await leaf(label);
      trace.push(`middle-after:${local}:${value}`);
      return `${local}:${value}`;
    } finally {
      trace.push(`middle-finally:${local}`);
    }
  }

  async function top(label) {
    const local = `top-${label}`;
    trace.push(`top-enter:${local}`);
    try {
      const value = await middle(label);
      trace.push(`top-after:${local}:${value}`);
      return `${local}:${value}`;
    } finally {
      trace.push(`top-finally:${local}`);
      await Promise.resolve(local);
      trace.push(`top-finally-after-await:${local}`);
    }
  }

  const result = await top('x');

  assertEq(
    result,
    'top-x:middle-x:leaf-x:X',
    'nested async result should preserve caller stack state'
  );

  assertEq(
    trace.join(','),
    [
      'top-enter:top-x',
      'middle-enter:middle-x',
      'leaf-before:leaf-x',
      'leaf-after:leaf-x:X',
      'middle-after:middle-x:leaf-x:X',
      'middle-finally:middle-x',
      'top-after:top-x:middle-x:leaf-x:X',
      'top-finally:top-x',
      'top-finally-after-await:top-x',
    ].join(','),
    'materialized slice should preserve frames, handlers, and locals above the await'
  );

  console.log('nested async stack slice resumes with handlers and locals intact');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
