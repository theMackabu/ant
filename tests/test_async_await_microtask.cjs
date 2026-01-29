var jobQueue = [];

function setTimeout(cb, time, cbarg) {
  var speedup = 10;
  var runTime = Date.now() + time / speedup;
  if (!jobQueue[runTime]) {
    jobQueue[runTime] = [];
  }
  jobQueue[runTime].push(function () {
    cb(cbarg);
  });
}

function flushQueue() {
  var curTime = Date.now();
  var empty = true;
  for (var runTime in jobQueue) {
    empty = false;
    if (curTime >= runTime) {
      var jobs = jobQueue[runTime];
      delete jobQueue[runTime];
      jobs.forEach(function (job) {
        job();
      });
    }
  }
  if (!empty) {
    Promise.resolve().then(flushQueue);
  }
}

function asyncTestPassed() {
  console.log('test_async_await_microtask.cjs: OK');
}

function testCode() {
  (async function () {
    await Promise.resolve();
    console.log('after');
    var a1 = await new Promise(function (resolve) {
      setTimeout(resolve, 800, 'foo');
    });
    var a2 = await new Promise(function (resolve) {
      setTimeout(resolve, 800, 'bar');
    });
    if (a1 + a2 === 'foobar') {
      asyncTestPassed();
    } else {
      console.log('test_async_await_microtask.cjs: FAIL');
    }
  })();
}

try {
  testCode();
  Promise.resolve().then(flushQueue);
} catch (e) {
  console.log('test_async_await_microtask.cjs: exception: ' + e);
}
