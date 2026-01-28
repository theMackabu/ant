// Test that `this` is preserved correctly in coroutine/async contexts

class Widget {
  constructor(name) {
    this.name = name;
    this._value = 42;
    this._boundMethod = this._boundMethod.bind(this);
  }
  
  _boundMethod() {
    return this._value;
  }
  
  unboundMethod() {
    return this._value;
  }
  
  callBound() {
    return this._boundMethod();
  }
  
  callUnbound() {
    return this.unboundMethod();
  }
}

let passed = 0;
let failed = 0;

function test(name, fn) {
  try {
    const result = fn();
    if (result instanceof Promise) {
      result.then(() => {
        console.log(`✓ ${name}`);
        passed++;
      }).catch(e => {
        console.log(`✗ ${name}`);
        console.log(`  Error: ${e.message}`);
        failed++;
      });
    } else {
      console.log(`✓ ${name}`);
      passed++;
    }
  } catch (e) {
    console.log(`✗ ${name}`);
    console.log(`  Error: ${e.message}`);
    failed++;
  }
}

function assertEqual(a, b, msg) {
  if (a !== b) throw new Error(msg || `Expected ${b}, got ${a}`);
}

console.log('=== Coroutine/Async this preservation tests ===\n');

// Sync tests
console.log('-- Synchronous --');

test('Direct method call preserves this', () => {
  const w = new Widget('test');
  assertEqual(w.callBound(), 42);
});

test('Bound method works', () => {
  const w = new Widget('test');
  assertEqual(w._boundMethod(), 42);
});

test('Unbound method via this works', () => {
  const w = new Widget('test');
  assertEqual(w.callUnbound(), 42);
});

// Test in callback context
console.log('\n-- In callback context --');

test('Method call in setTimeout callback', () => {
  return new Promise((resolve, reject) => {
    const w = new Widget('test');
    setTimeout(() => {
      try {
        assertEqual(w.callBound(), 42);
        resolve();
      } catch (e) {
        reject(e);
      }
    }, 10);
  });
});

test('Method call in Promise.then', () => {
  const w = new Widget('test');
  return Promise.resolve().then(() => {
    assertEqual(w.callBound(), 42);
  });
});

// Test in event handler context (simulated)
console.log('\n-- In event handler context --');

test('Method call from stored handler', () => {
  const w = new Widget('test');
  const handlers = [];
  
  handlers.push(() => {
    return w.callBound();
  });
  
  for (const h of handlers) {
    assertEqual(h(), 42);
  }
});

test('Bound method passed as callback', () => {
  const w = new Widget('test');
  const fn = w._boundMethod;
  assertEqual(fn(), 42);
});

// Test class with stdin-like event pattern
console.log('\n-- Event emitter pattern --');

class FakeScreen {
  constructor() {
    this._running = true;
    this._handlers = [];
    this._cleanup = this._cleanup.bind(this);
  }
  
  _cleanup() {
    if (!this._running) return 'already stopped';
    this._running = false;
    return 'cleaned up';
  }
  
  onKey(fn) {
    this._handlers.push(fn);
  }
  
  exit() {
    const result = this._cleanup();
    return result;
  }
  
  emitKey(key) {
    for (const h of this._handlers) {
      h(key);
    }
  }
}

test('Exit method calls _cleanup correctly', () => {
  const screen = new FakeScreen();
  let exitResult = null;
  
  screen.onKey((key) => {
    if (key === 'q') {
      exitResult = screen.exit();
    }
  });
  
  screen.emitKey('q');
  assertEqual(exitResult, 'cleaned up');
});

// Test with async handler
console.log('\n-- Async event handlers --');

test('Exit in async handler', async () => {
  const screen = new FakeScreen();
  let exitResult = null;
  
  screen.onKey(async (key) => {
    if (key === 'q') {
      await Promise.resolve(); // simulate async work
      exitResult = screen.exit();
    }
  });
  
  screen.emitKey('q');
  await Promise.resolve();
  await Promise.resolve(); // Give time for async handler
  assertEqual(exitResult, 'cleaned up');
});

// Test stdin data event pattern
console.log('\n-- stdin.on pattern --');

test('Method call from stdin data handler', () => {
  return new Promise((resolve, reject) => {
    const screen = new FakeScreen();
    let result = null;
    
    const handler = (chunk) => {
      const str = chunk.toString ? chunk.toString() : chunk;
      if (str === 'q') {
        try {
          result = screen.exit();
          assertEqual(result, 'cleaned up');
          resolve();
        } catch (e) {
          reject(e);
        }
      }
    };
    
    // Simulate stdin event
    setTimeout(() => handler('q'), 10);
  });
});

// Wait for async tests
setTimeout(() => {
  console.log(`\n=== Results: ${passed} passed, ${failed} failed ===`);
  process.exit(failed > 0 ? 1 : 0);
}, 200);
