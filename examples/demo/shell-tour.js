#!/usr/bin/env ant

import { $ } from 'ant:shell';

const section = async (title, promise) => {
  const output = await promise;
  console.log(`\x1b[1;36m== ${title}\x1b[0m`);
  process.stdout.write(output.text());
};

await section(
  'variables and parameter expansion',
  $`file=archive.tar.gz
    echo "name=\${file%%.*} ext=\${file#*.} upper=\${file^^}"
    echo "default=\${missing:-fallback} length=\${#file}"`
);

await section(
  'arithmetic and C-style loops',
  $`total=0
    for ((i = 1; i <= 10; i++)); do (( total += i * i )); done
    echo "sum of squares 1..10 = $total"
    echo "2**40 = $((2 ** 40)), hex ff = $((16#ff))"`
);

await section(
  'functions, local scope, and recursion',
  $`fib() {
      local n=$1
      (( n < 2 )) && { echo $n; return; }
      echo $(( $(fib $((n - 1))) + $(fib $((n - 2))) ))
    }
    for n in 5 10 15; do printf 'fib(%d) = %d\n' $n "$(fib $n)"; done`
);

await section(
  'case and [[ ]] pattern matching',
  $`for f in main.c notes.md build.sh photo.JPG; do
      case $f in
        *.c|*.h) kind=source ;;
        *.md) kind=docs ;;
        *.sh) kind=script ;;
        *) kind=other ;;
      esac
      [[ $f =~ ^([a-z]+)\. ]] && printf '%-10s %-7s stem=%s\n' "$f" $kind "\${BASH_REMATCH[1]}"
    done`
);

await section(
  'indexed and associative arrays',
  $`langs=(c javascript rust zig)
    declare -A year=([c]=1972 [javascript]=1995 [rust]=2015 [zig]=2016)
    echo "\${#langs[@]} languages, last one: \${langs[-1]}"
    for lang in "\${langs[@]}"; do echo "  $lang -> \${year[$lang]}"; done`
);

await section(
  'here-documents and while read',
  $`while IFS=: read -r name role; do
      echo "$name works as $role"
    done <<EOF
ada:engineer
grace:admiral
EOF`
);

await section(
  'pipelines, brace expansion, and process substitution',
  $`printf '%s\n' {banana,apple,cherry} | sort | tr a-z A-Z
    diff <(printf 'a\nb\n') <(printf 'a\nc\n') | grep '^[<>]'`
);

await section(
  'traps and exit status',
  $`trap 'echo "cleanup ran (status $?)"' EXIT
    false || echo "recovered from a failure"
    (exit 3); echo "subshell exited with $?"`
);
