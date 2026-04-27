import { test, testDeep, summary } from './helpers.js';

console.log('Explicit Resource Management Tests\n');

test('Symbol.dispose exists', typeof Symbol.dispose, 'symbol');
test('Symbol.asyncDispose exists', typeof Symbol.asyncDispose, 'symbol');
test('SuppressedError constructor exists', typeof SuppressedError, 'function');

const suppressed = new SuppressedError('error', 'suppressed', 'message');
test('SuppressedError name', suppressed.name, 'SuppressedError');
test('SuppressedError message', suppressed.message, 'message');
test('SuppressedError error property', suppressed.error, 'error');
test('SuppressedError suppressed property', suppressed.suppressed, 'suppressed');
test('SuppressedError instanceof Error', suppressed instanceof Error, true);
test('SuppressedError callable', SuppressedError('e', 's') instanceof SuppressedError, true);

let stackLog = [];
const stack = new DisposableStack();
const used = {
  [Symbol.dispose]() {
    stackLog.push('use');
  }
};
test('DisposableStack use returns resource', stack.use(used), used);
test(
  'DisposableStack adopt returns value',
  stack.adopt('value', value => stackLog.push('adopt:' + value)),
  'value'
);
test(
  'DisposableStack defer returns undefined',
  stack.defer(() => stackLog.push('defer')),
  undefined
);
test('DisposableStack move returns stack', stack.move() instanceof DisposableStack, true);

const stack2 = new DisposableStack();
stack2.use({
  [Symbol.dispose]() {
    stackLog.push('second use');
  }
});
stack2.adopt('second', value => stackLog.push(value));
stack2.defer(() => stackLog.push('second defer'));
stack2.dispose();
testDeep('DisposableStack disposes LIFO', stackLog.slice(-3), ['second defer', 'second', 'second use']);

let suppressedErr1 = new Error('first');
let suppressedErr2 = new Error('second');
let suppressedCaught;
try {
  const throwingStack = new DisposableStack();
  throwingStack.use({
    [Symbol.dispose]() {
      throw suppressedErr2;
    }
  });
  throwingStack.use({
    [Symbol.dispose]() {
      throw suppressedErr1;
    }
  });
  throwingStack.dispose();
} catch (e) {
  suppressedCaught = e;
}
test('DisposableStack suppression error', suppressedCaught.error, suppressedErr2);
test('DisposableStack suppression suppressed', suppressedCaught.suppressed, suppressedErr1);

let asyncLog = [];
const asyncStack = new AsyncDisposableStack();
const asyncResource = {
  async [Symbol.asyncDispose]() {
    asyncLog.push('use');
  }
};
test('AsyncDisposableStack use returns resource', asyncStack.use(asyncResource), asyncResource);
test(
  'AsyncDisposableStack adopt returns value',
  asyncStack.adopt('value', async value => asyncLog.push('adopt:' + value)),
  'value'
);
test(
  'AsyncDisposableStack defer returns undefined',
  asyncStack.defer(async () => asyncLog.push('defer')),
  undefined
);
await asyncStack.disposeAsync();
testDeep('AsyncDisposableStack disposes LIFO', asyncLog, ['defer', 'adopt:value', 'use']);

function usingBlock(resource) {
  {
    using _ = resource;
  }
}

const usingResource = {
  disposed: false,
  [Symbol.dispose]() {
    this.disposed = true;
  }
};
usingBlock(usingResource);
test('using block disposes', usingResource.disposed, true);

function usingReturn(resource) {
  {
    using _ = resource;
    return 'done';
  }
}

const returnResource = {
  disposed: false,
  [Symbol.dispose]() {
    this.disposed = true;
  }
};
test('using return value', usingReturn(returnResource), 'done');
test('using return disposes', returnResource.disposed, true);

let throwingReturnCalls = 0;
const throwingReturnError = new Error('dispose return');
try {
  (function () {
    {
      using _ = {
        [Symbol.dispose]() {
          throwingReturnCalls++;
          throw throwingReturnError;
        }
      };
      return 'skipped';
    }
  })();
} catch (e) {
  test('using return cleanup throws original error', e, throwingReturnError);
}
test('using return cleanup throws once', throwingReturnCalls, 1);

function usingFunctionScope(resource) {
  using _ = resource;
  test('using function scope alive before return', resource.disposed, false);
  return 'function';
}

const functionScopeResource = {
  disposed: false,
  [Symbol.dispose]() {
    this.disposed = true;
  }
};
test('using function scope return value', usingFunctionScope(functionScopeResource), 'function');
test('using function scope disposes on return', functionScopeResource.disposed, true);

let breakResource;
while (true) {
  {
    using _ = (breakResource = {
      disposed: false,
      [Symbol.dispose]() {
        this.disposed = true;
      }
    });
    break;
  }
}
test('using break disposes', breakResource.disposed, true);

let continueResource;
let continueCount = 0;
while (continueCount++ < 1) {
  {
    using _ = (continueResource = {
      disposed: false,
      [Symbol.dispose]() {
        this.disposed = true;
      }
    });
    continue;
  }
}
test('using continue disposes', continueResource.disposed, true);

async function awaitUsingBlock(resource) {
  {
    await using _ = resource;
  }
}

const awaitUsingResource = {
  disposed: false,
  async [Symbol.asyncDispose]() {
    this.disposed = true;
  }
};
await awaitUsingBlock(awaitUsingResource);
test('await using block disposes', awaitUsingResource.disposed, true);

async function awaitUsingReturn(resource) {
  {
    await using _ = resource;
    return 'async';
  }
}

const awaitReturnResource = {
  disposed: false,
  async [Symbol.asyncDispose]() {
    this.disposed = true;
  }
};
test('await using return value', await awaitUsingReturn(awaitReturnResource), 'async');
test('await using return disposes', awaitReturnResource.disposed, true);

let throwingAwaitReturnCalls = 0;
const throwingAwaitReturnError = new Error('async dispose return');
try {
  await (async function () {
    {
      await using _ = {
        async [Symbol.asyncDispose]() {
          throwingAwaitReturnCalls++;
          throw throwingAwaitReturnError;
        }
      };
      return 'skipped';
    }
  })();
} catch (e) {
  test('await using return cleanup throws original error', e, throwingAwaitReturnError);
}
test('await using return cleanup throws once', throwingAwaitReturnCalls, 1);

const forOfLog = [];
for (using item of [
  {
    [Symbol.dispose]() {
      forOfLog.push('a');
    }
  },
  {
    [Symbol.dispose]() {
      forOfLog.push('b');
    }
  }
]);
testDeep('using for-of disposes each iteration', forOfLog, ['a', 'b']);

const forOfBreakLog = [];
for (using item of [
  {
    [Symbol.dispose]() {
      forOfBreakLog.push('break');
    }
  }
]) {
  break;
}
testDeep('using for-of break disposes current item', forOfBreakLog, ['break']);

const forOfThrowLog = [];
try {
  for (using item of [
    {
      [Symbol.dispose]() {
        forOfThrowLog.push('throw');
      }
    }
  ]) {
    throw new Error('body');
  }
} catch (e) {
  test('using for-of body throw preserved', e.message, 'body');
}
testDeep('using for-of throw disposes current item', forOfThrowLog, ['throw']);

const awaitForOfLog = [];
for (await using item of [
  {
    async [Symbol.asyncDispose]() {
      awaitForOfLog.push('a');
    }
  },
  {
    async [Symbol.asyncDispose]() {
      awaitForOfLog.push('b');
    }
  }
]);
testDeep('await using for-of disposes each iteration', awaitForOfLog, ['a', 'b']);

const awaitForOfBreakLog = [];
for (await using item of [
  {
    async [Symbol.asyncDispose]() {
      awaitForOfBreakLog.push('break');
    }
  }
]) {
  break;
}
testDeep('await using for-of break disposes current item', awaitForOfBreakLog, ['break']);

const err1 = new Error('dispose 1');
const err2 = new Error('dispose 2');
const err3 = new Error('body');
let usingSuppressed;
try {
  {
    using _1 = {
        [Symbol.dispose]() {
          throw err1;
        }
      },
      _2 = {
        [Symbol.dispose]() {
          throw err2;
        }
      };
    throw err3;
  }
} catch (e) {
  usingSuppressed = e;
}

test('using suppression outer error', usingSuppressed.error, err1);
test('using suppression inner error', usingSuppressed.suppressed.error, err2);
test('using suppression body error', usingSuppressed.suppressed.suppressed, err3);

summary();
