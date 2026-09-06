const print = console.log;

function Run() {
  BenchmarkSuite.RunSuites({
    NotifyStep: ShowProgress,
    NotifyError: AddError,
    NotifyResult: AddResult,
    NotifyScore: AddScore
  });
}

let harnessErrorCount = 0;

function ShowProgress(name) {
  print('progress', name);
}

function AddError(name, error) {
  print('error', name, error);
  print(error.stack);
  harnessErrorCount++;
}

function AddResult(name, result) {
  print('result', name, result);
}

function AddScore(score) {
  print('raw-score', 100 * BenchmarkSuite.GeometricMean(BenchmarkSuite.scores));
  print('score', score);
  if (globalThis.__ANT_BENCH_REPORT_STATS__) {
    print('allocation-stats', JSON.stringify(Ant.stats().alloc));
  }
}

try {
  Run();
} catch (e) {
  print('*** Run() failed');
  print(e.stack || e);
}

if (harnessErrorCount > 0) {
  throw new Error('benchmark had ' + harnessErrorCount + ' errors');
}
