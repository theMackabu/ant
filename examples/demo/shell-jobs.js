#!/usr/bin/env ant

import { $ } from 'ant:shell';

const started = Date.now();

const jobs = await $`
  work() { sleep "$1"; echo "task $2 finished after $1s"; }
  work 0.3 A & a=$!
  work 0.1 B &
  work 0.2 C &
  echo "launched 3 jobs, first pid $a"
  wait -n; echo "first to finish is done"
  wait; echo "all jobs done"
`.text();
process.stdout.write(jobs);
console.log(`elapsed ${Date.now() - started}ms (sequential would be 600ms)\n`);

const killed = await $`
  sleep 30 & pid=$!
  kill -0 $pid && echo "sleep $pid is running"
  kill $pid; wait $pid
  echo "killed it, wait returned $?"
`.text();
process.stdout.write(killed + '\n');

const streaming = await $`
  yes 'stream' | while read -r word; do
    (( n++ ))
    echo "$n:$word"
  done | head -3
  echo "infinite producer stopped by head, status \${PIPESTATUS[*]}"
`.nothrow();
process.stdout.write(streaming.text() + '\n');

const coproc = await $`
  coproc CALC { while read -r expr; do echo $(( expr )); done; }
  for e in '6*7' '2**16' '100/3'; do
    echo "$e" >&\${CALC[1]}
    read -r answer <&\${CALC[0]}
    echo "$e = $answer"
  done
`.text();
process.stdout.write(coproc);
