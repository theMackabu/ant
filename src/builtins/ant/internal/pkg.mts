// @ant: compact_on_build
import { describe } from 'upm/src/util.ts';

import { quickRun } from '../../../pkg/quick-run.ts';
import { fail } from '../../../pkg/ui/output.ts';

quickRun(process.argv.slice(2)).then(
  code => {
    if (code === undefined) return import('ant:internal/pkg_main');
    process.exitCode = code;
  },
  error => {
    fail(describe(error));
    process.exitCode = 1;
  }
);
