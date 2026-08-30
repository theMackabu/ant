var __antBenchPrint =
  typeof print === 'function'
    ? print
    : function () {
        console.log(Array.prototype.join.call(arguments, ' '));
      };

function Run() {
  BenchmarkSuite.RunSuites({
    NotifyStep: ShowProgress,
    NotifyError: AddError,
    NotifyResult: AddResult,
    NotifyScore: AddScore
  });
}

var harnessErrorCount = 0;

function ShowProgress(name) {
  __antBenchPrint('progress', name);
}

function AddError(name, error) {
  __antBenchPrint('error', name, error);
  __antBenchPrint(error && error.stack ? error.stack : error);
  harnessErrorCount++;
}

function AddResult(name, result) {
  __antBenchPrint('result', name, result);
}

function AddScore(score) {
  __antBenchPrint('raw-score', 100 * BenchmarkSuite.GeometricMean(BenchmarkSuite.scores));
  __antBenchPrint('score', score);
}

try {
  Run();
} catch (error) {
  __antBenchPrint('error', 'Run', error && error.stack ? error.stack : error);
  harnessErrorCount++;
}

if (harnessErrorCount > 0) {
  throw new Error('benchmark had ' + harnessErrorCount + ' errors');
}
