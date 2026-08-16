import assert from 'node:assert';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { $ } from 'ant:shell';

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-shell-'));

try {
  const simple = await $`echo "Hello, world!"`;
  assert.strictEqual(simple.stdout, 'Hello, world!\n');
  assert.strictEqual(simple.stderr, '');
  assert.strictEqual(simple.exitCode, 0);
  assert.strictEqual(simple.signalCode, null);
  assert.strictEqual(await $`printf text`.text(), 'text');
  assert.deepStrictEqual(await $`printf "line1\nline2\nline3\n"`.lines(), [
    'line1',
    'line2',
    'line3'
  ]);

  const unsafe = 'one; echo interpolated-source-was-executed';
  assert.strictEqual(await $`echo ${unsafe}`.text(), `${unsafe}\n`);
  assert.strictEqual(await $`printf '<%s>' ${['one two', 'three']}`.text(), '<one two><three>');

  assert.strictEqual(await $`false && echo bad; true && echo and; false || echo or`.text(), 'and\nor\n');
  assert.strictEqual(await $`printf abc | wc -c`.text(), '3\n');
  assert.strictEqual(await $`seq 1 10000 | wc -l`.text(), '10000\n');
  assert.strictEqual(await $`yes | head -1`.text(), 'y\n');

  const cwdResult = await $`pwd; cd ${tmpDir}; pwd`;
  const cwdLines = cwdResult.stdout.trim().split('\n');
  assert.strictEqual(cwdLines.length, 2);
  assert.strictEqual(cwdLines[1], fs.realpathSync(tmpDir));

  const redirectPath = path.join(tmpDir, 'redirect.txt');
  await $`printf hello > ${redirectPath}`;
  await $`printf world >> ${redirectPath}`;
  assert.strictEqual(await $`cat < ${redirectPath}`.text(), 'helloworld');
  assert.strictEqual(fs.readFileSync(redirectPath, 'utf8'), 'helloworld');
  assert.notStrictEqual((await $`cd ${redirectPath}`.nothrow()).exitCode, 0);

  const merged = await $`ls ${path.join(tmpDir, 'missing')} 2>&1`.nothrow();
  assert.notStrictEqual(merged.exitCode, 0);
  assert.strictEqual(merged.stderr, '');
  assert.ok(merged.stdout.length > 0);

  const mergedPath = path.join(tmpDir, 'merged.txt');
  const mergedToFile = await $`ls ${path.join(tmpDir, 'missing-a')} > ${mergedPath} 2>&1`.nothrow();
  assert.strictEqual(mergedToFile.stdout, '');
  assert.strictEqual(mergedToFile.stderr, '');
  assert.ok(fs.readFileSync(mergedPath, 'utf8').length > 0);

  const stdoutPath = path.join(tmpDir, 'stdout-only.txt');
  const duplicatedFirst = await $`ls ${path.join(tmpDir, 'missing-b')} 2>&1 > ${stdoutPath}`.nothrow();
  assert.ok(duplicatedFirst.stdout.length > 0);
  assert.strictEqual(duplicatedFirst.stderr, '');
  assert.strictEqual(fs.readFileSync(stdoutPath, 'utf8'), '');

  const failed = await $`exit 7`.nothrow();
  assert.strictEqual(failed.exitCode, 7);
  const missingCommand = await $`ant-shell-command-that-does-not-exist`.nothrow();
  assert.strictEqual(missingCommand.exitCode, 127);
  assert.ok(missingCommand.stderr.includes('ENOENT'));
  const missingInPipeline = await $`ant-shell-command-that-does-not-exist | cat`.nothrow();
  assert.strictEqual(missingInPipeline.exitCode, 0);
  assert.ok(missingInPipeline.stderr.includes('ENOENT'));
  const missingAtPipelineEnd = await $`printf input | ant-shell-command-that-does-not-exist`.nothrow();
  assert.strictEqual(missingAtPipelineEnd.exitCode, 127);
  assert.ok(missingAtPipelineEnd.stderr.includes('ENOENT'));
  let thrown;
  try {
    await $`exit 8`;
  } catch (error) {
    thrown = error;
  }
  assert.ok(thrown instanceof Error);
  assert.strictEqual(thrown.exitCode, 8);
  assert.strictEqual(await $`exit 9`.catch(error => error.exitCode), 9);

  async function cached(value) {
    return $`printf %s ${value}`.text();
  }
  assert.strictEqual(await cached('first'), 'first');
  assert.strictEqual(await cached('second'), 'second');

  assert.strictEqual(await $('printf string-argument').text(), 'string-argument');
  let syntaxThrown = false;
  try {
    $`echo "unterminated`;
  } catch (error) {
    syntaxThrown = error instanceof SyntaxError;
  }
  assert.strictEqual(syntaxThrown, true);
} finally {
  fs.rmSync(tmpDir, { recursive: true, force: true });
}

console.log('ant:shell Silver frontend tests passed');
