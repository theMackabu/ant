// @ant: compact_on_build
import { main } from '../../../pkg/cli.ts';

main(process.argv.slice(2)).then(code => {
  process.exitCode = code;
});
