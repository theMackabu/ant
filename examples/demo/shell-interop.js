#!/usr/bin/env ant

import { $ } from 'ant:shell';

const files = ['notes final.txt', 'a;rm -rf ~', '$(whoami)'];
console.log('interpolated values are arguments, never shell code:');
process.stdout.write(await $`printf '  <%s>\n' ${files}`.text());

const csv = Buffer.from('name,score\nada,93\ngrace,88\nlinus,71\n');
const top = await $`tail -n +2 < ${csv} | sort -t, -k2 -nr | head -1 | cut -d, -f1`.text();
console.log(`\nbuffer as stdin, top scorer: ${top.trim()}`);

const response = new Response(JSON.stringify({ items: [1, 2, 3] }));
const parsed = await $`cat < ${response}`.json();
console.log(`Response as stdin, parsed JSON: ${JSON.stringify(parsed)}`);

const target = new Uint8Array(13);
await $`printf 'into a buffer' > ${target}`;
console.log(`redirected into a Uint8Array: ${JSON.stringify(Buffer.from(target).toString())}`);

console.log(`\nper-command env and cwd: ${(await $`echo "$GREETING from $(pwd)"`.env({ GREETING: 'hello', PATH: process.env.PATH }).cwd('/tmp').text()).trim()}`);

console.log('\nlines as an async iterator:');
for await (const line of $`printf 'one\ntwo\nthree\n'`.lines()) if (line) console.log(`  ${line}`);

const failed = await $`echo partial; exit 3`.nothrow();
console.log(`\nnothrow result: exit ${failed.exitCode}, stdout ${JSON.stringify(failed.text())}`);
try {
  await $`echo 'to stderr' >&2; exit 2`;
} catch (error) {
  console.log(`ShellError: exit ${error.exitCode}, stderr ${JSON.stringify(error.stderr.toString())}`);
}

console.log(`\nhelpers: $.braces -> ${JSON.stringify($.braces('img-{1..3}.{png,jpg}'))}`);
console.log(`         $.escape -> ${$.escape('it\'s $HOME')}`);

console.log('\nlive output with .quiet(false):');
await $`for i in 1 2 3; do echo "  tick $i"; sleep 0.1; done`.quiet(false);
