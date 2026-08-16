import assert from 'node:assert';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { $ } from 'ant:shell';

const decoder = new TextDecoder();
const decode = bytes => decoder.decode(bytes);

async function expectShellRejection(invoke, pattern, ErrorType = Error) {
  let failure;
  try {
    await invoke();
  } catch (error) {
    failure = error;
  }
  assert.ok(failure instanceof ErrorType, `expected ${ErrorType.name}, got ${failure}`);
  assert.match(String(failure.message), pattern);
}

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-shell-'));

try {
  const simple = await $`echo "Hello, world!"`;
  assert.ok(simple.stdout instanceof Uint8Array);
  assert.ok(simple.stderr instanceof Uint8Array);
  assert.strictEqual(decode(simple.stdout), 'Hello, world!\n');
  assert.strictEqual(decode(simple.stderr), '');
  assert.strictEqual(simple.exitCode, 0);
  assert.strictEqual(simple.signalCode, null);
  assert.strictEqual(simple.constructor.name, 'ShellOutput');
  assert.strictEqual(simple.text(), 'Hello, world!\n');
  assert.deepStrictEqual(simple.lines(), ['Hello, world!']);
  assert.strictEqual(simple.arrayBuffer(), simple.stdout.buffer);
  assert.notStrictEqual(simple.bytes(), simple.stdout);
  assert.strictEqual(simple.bytes().buffer, simple.stdout.buffer);
  assert.ok(simple.blob() instanceof Blob);
  assert.strictEqual(await simple.blob().text(), 'Hello, world!\n');
  const jsonOutput = await $`printf '{"ok":true}'`;
  assert.deepStrictEqual(jsonOutput.json(), { ok: true });
  const binaryOutput = await $`printf %b ${'\\377\\000A'}`;
  assert.deepStrictEqual(Array.from(binaryOutput.stdout), [255, 0, 65]);
  assert.deepStrictEqual(Array.from(binaryOutput.bytes()), [255, 0, 65]);
  const binaryRedirectPath = path.join(tmpDir, 'binary-output.bin');
  await $`printf %b ${'\\377\\000A'} > ${binaryRedirectPath}`;
  assert.deepStrictEqual(Array.from(fs.readFileSync(binaryRedirectPath)), [255, 0, 65]);
  assert.deepStrictEqual(Object.keys(simple).sort(), ['exitCode', 'signalCode', 'stderr', 'stdout']);
  const empty = await $`
  `;
  assert.strictEqual(empty.text(), '');
  assert.strictEqual(empty.stdout.length, 0);
  assert.strictEqual(empty.stderr.length, 0);
  assert.strictEqual(empty.exitCode, 0);
  assert.strictEqual(empty.signalCode, null);
  assert.strictEqual(await $`printf text`.text(), 'text');
  assert.deepStrictEqual(await $`printf '{"ok":true}'`.json(), { ok: true });
  assert.ok((await $`printf bytes`.bytes()) instanceof Uint8Array);
  assert.ok((await $`printf buffer`.arrayBuffer()) instanceof ArrayBuffer);
  assert.ok((await $`printf blob`.blob()) instanceof Blob);
  let finallyCalls = 0;
  const finallyResult = await $`printf finally`.finally(() => finallyCalls++);
  assert.strictEqual(finallyResult.text(), 'finally');
  assert.strictEqual(finallyCalls, 1);
  let rejectedFinallyCalls = 0;
  let finallyFailure;
  try {
    await $`exit 18`.finally(() => rejectedFinallyCalls++);
  } catch (error) {
    finallyFailure = error;
  }
  assert.ok(finallyFailure instanceof Error);
  assert.strictEqual(finallyFailure.exitCode, 18);
  assert.strictEqual(rejectedFinallyCalls, 1);
  assert.deepStrictEqual(await $`printf "line1\nline2\nline3\n"`.lines(), ['line1', 'line2', 'line3']);

  const unsafe = 'one; echo interpolated-source-was-executed';
  assert.strictEqual(await $`echo ${unsafe}`.text(), `${unsafe}\n`);
  assert.strictEqual(await $`printf '<%s>' ${['one two', 'three']}`.text(), '<one two><three>');
  assert.strictEqual(await $`printf '<%s>' ${['one', 'two']}""`.text(), '<one,two>');

  const coercionFailure = {
    toString() {
      throw new Error('shell-coercion-boom');
    }
  };
  for (const invoke of [
    () => $`printf %s ${coercionFailure}`,
    () => $`printf %s ${[coercionFailure]}`,
    () => $`printf redirected > ${coercionFailure}`
  ]) {
    await expectShellRejection(invoke, /shell-coercion-boom/);
  }

  assert.strictEqual(await $`false && echo bad; true && echo and; false || echo or`.text(), 'and\nor\n');
  assert.strictEqual(await $`printf abc | wc -c`.text(), '3\n');
  assert.strictEqual(await $`: | cat`.text(), '');
  assert.strictEqual(await $`echo pipeline-builtin | cat`.text(), 'pipeline-builtin\n');
  const largePipelineBuiltin = 'x'.repeat(256 * 1024 + 17);
  assert.strictEqual(await $`echo ${largePipelineBuiltin} | wc -c`.text(), `${largePipelineBuiltin.length + 1}\n`);
  const ignoredPipelineBuiltin = await $`echo ${largePipelineBuiltin} | :`.nothrow();
  assert.strictEqual(ignoredPipelineBuiltin.exitCode, 0);
  assert.strictEqual(ignoredPipelineBuiltin.stderr.length, 0);
  assert.strictEqual(await $`cd ${tmpDir} | pwd`.text(), `${process.cwd()}\n`);
  assert.strictEqual((await $`false | true`.nothrow()).exitCode, 0);
  assert.strictEqual((await $`true | false`.nothrow()).exitCode, 1);
  assert.strictEqual((await $`exit 7 | cat`.nothrow()).exitCode, 0);
  assert.strictEqual((await $`printf ignored | exit 7`.nothrow()).exitCode, 7);
  const pipelineBuiltinRedirectPath = path.join(tmpDir, 'pipeline-builtin.txt');
  await $`printf ignored | echo redirected > ${pipelineBuiltinRedirectPath}`;
  assert.strictEqual(fs.readFileSync(pipelineBuiltinRedirectPath, 'utf8'), 'redirected\n');
  const failedPipelineBuiltin = await $`
    printf ignored | cd ${path.join(tmpDir, 'missing-pipeline-directory')} 2>&1
  `.nothrow();
  assert.strictEqual(failedPipelineBuiltin.exitCode, 1);
  assert.strictEqual(failedPipelineBuiltin.stderr.length, 0);
  assert.match(failedPipelineBuiltin.text(), /cd: .*missing-pipeline-directory/);
  assert.strictEqual(
    await $`echo continued | \
    cat`.text(),
    'continued\n'
  );
  assert.strictEqual(await $`seq 1 10000 | wc -l`.text(), '10000\n');
  assert.strictEqual(await $`yes | head -1`.text(), 'y\n');

  await expectShellRejection(() => $`${''}`, /executable cannot be empty/, TypeError);
  for (const [invoke, pattern] of [
    [() => $`while true; do echo bad; done`, /compound command.*while/i],
    [() => $`name=value echo bad`, /variable assignment/i],
    [() => $`echo "$HOME"`, /parameter expansion/i],
    [() => $`echo $((1 + 2))`, /arithmetic expansion/i],
    [() => $`echo $(pwd)`, /command substitution/i]
  ]) {
    await expectShellRejection(invoke, pattern, SyntaxError);
  }
  assert.strictEqual(await $`echo '$HOME'`.text(), '$HOME\n');
  assert.strictEqual(await $`echo \$HOME`.text(), '$HOME\n');
  assert.strictEqual(await $`echo name=value`.text(), 'name=value\n');

  const unsupportedCompoundPath = path.join(tmpDir, 'unsupported-compound.txt');
  await expectShellRejection(
    () => $`echo must-not-run > ${unsupportedCompoundPath}; while true; do echo bad; done`,
    /compound command.*while/i,
    SyntaxError
  );
  assert.strictEqual(fs.existsSync(unsupportedCompoundPath), false);

  const cwdResult = await $`pwd; cd ${tmpDir}; pwd`;
  const cwdLines = cwdResult.text().trim().split('\n');
  assert.strictEqual(cwdLines.length, 2);
  assert.strictEqual(cwdLines[1], fs.realpathSync(tmpDir));

  const redirectPath = path.join(tmpDir, 'redirect.txt');
  await $`printf hello > ${redirectPath}`;
  await $`printf world >> ${redirectPath}`;
  assert.strictEqual(await $`cat < ${redirectPath}`.text(), 'helloworld');
  assert.strictEqual(fs.readFileSync(redirectPath, 'utf8'), 'helloworld');
  assert.notStrictEqual((await $`cd ${redirectPath}`.nothrow()).exitCode, 0);

  const builtinRedirectPath = path.join(tmpDir, 'builtin-redirect.txt');
  const builtinRedirectText = 'x'.repeat(256 * 1024 + 17);
  let builtinRedirectYielded = false;
  const builtinRedirect = $`echo ${builtinRedirectText} > ${builtinRedirectPath}`;
  setTimeout(() => {
    builtinRedirectYielded = true;
  }, 0);
  await builtinRedirect;
  assert.strictEqual(builtinRedirectYielded, true);
  assert.strictEqual(fs.readFileSync(builtinRedirectPath, 'utf8'), `${builtinRedirectText}\n`);
  await $`echo tail >> ${builtinRedirectPath}`;
  assert.strictEqual(fs.readFileSync(builtinRedirectPath, 'utf8'), `${builtinRedirectText}\ntail\n`);
  await $`echo replacement > ${builtinRedirectPath}`;
  assert.strictEqual(fs.readFileSync(builtinRedirectPath, 'utf8'), 'replacement\n');
  await $`true > ${builtinRedirectPath}`;
  assert.strictEqual(fs.readFileSync(builtinRedirectPath, 'utf8'), '');
  await $`true >> ${builtinRedirectPath}`;
  assert.strictEqual(fs.readFileSync(builtinRedirectPath, 'utf8'), '');

  const firstBuiltinRedirectPath = path.join(tmpDir, 'builtin-first.txt');
  const finalBuiltinRedirectPath = path.join(tmpDir, 'builtin-final.txt');
  fs.writeFileSync(firstBuiltinRedirectPath, 'stale');
  await $`echo ordered > ${firstBuiltinRedirectPath} > ${finalBuiltinRedirectPath}`;
  assert.strictEqual(fs.readFileSync(firstBuiltinRedirectPath, 'utf8'), '');
  assert.strictEqual(fs.readFileSync(finalBuiltinRedirectPath, 'utf8'), 'ordered\n');

  const redirectionOnlyPath = path.join(tmpDir, 'redirection-only.txt');
  fs.writeFileSync(redirectionOnlyPath, 'stale');
  await $`> ${redirectionOnlyPath}`;
  assert.strictEqual(fs.readFileSync(redirectionOnlyPath, 'utf8'), '');

  const missingRedirectPath = path.join(tmpDir, 'missing', 'output.txt');
  const failedBuiltinRedirect = await $`echo unwritten > ${missingRedirectPath}`.nothrow();
  assert.strictEqual(failedBuiltinRedirect.exitCode, 1);
  assert.match(decode(failedBuiltinRedirect.stderr), /no such file or directory/i);

  const unsupportedFdPath = path.join(tmpDir, 'unsupported-fd.txt');
  await expectShellRejection(() => $`printf must-not-run 2>${unsupportedFdPath}`, /numeric file descriptor/, SyntaxError);
  assert.strictEqual(fs.existsSync(unsupportedFdPath), false);

  const merged = await $`ls ${path.join(tmpDir, 'missing')} 2>&1`.nothrow();
  assert.notStrictEqual(merged.exitCode, 0);
  assert.strictEqual(merged.stderr.length, 0);
  assert.ok(merged.stdout.length > 0);

  const mergedPath = path.join(tmpDir, 'merged.txt');
  const mergedToFile = await $`ls ${path.join(tmpDir, 'missing-a')} > ${mergedPath} 2>&1`.nothrow();
  assert.strictEqual(mergedToFile.stdout.length, 0);
  assert.strictEqual(mergedToFile.stderr.length, 0);
  assert.ok(fs.readFileSync(mergedPath, 'utf8').length > 0);

  const stdoutPath = path.join(tmpDir, 'stdout-only.txt');
  const duplicatedFirst = await $`ls ${path.join(tmpDir, 'missing-b')} 2>&1 > ${stdoutPath}`.nothrow();
  assert.ok(duplicatedFirst.stdout.length > 0);
  assert.strictEqual(duplicatedFirst.stderr.length, 0);
  assert.strictEqual(fs.readFileSync(stdoutPath, 'utf8'), '');

  const supersededPath = path.join(tmpDir, 'superseded.txt');
  const finalPath = path.join(tmpDir, 'final.txt');
  await $`printf ordered > ${supersededPath} > ${finalPath}`;
  assert.strictEqual(fs.readFileSync(supersededPath, 'utf8'), '');
  assert.strictEqual(fs.readFileSync(finalPath, 'utf8'), 'ordered');

  const snapshottedStderrPath = path.join(tmpDir, 'snapshotted-stderr.txt');
  const laterStdoutPath = path.join(tmpDir, 'later-stdout.txt');
  await $`ls ${path.join(tmpDir, 'missing-c')} > ${snapshottedStderrPath} 2>&1 > ${laterStdoutPath}`.nothrow();
  assert.ok(fs.readFileSync(snapshottedStderrPath, 'utf8').length > 0);
  assert.strictEqual(fs.readFileSync(laterStdoutPath, 'utf8'), '');

  const largeRedirectPath = path.join(tmpDir, 'large-redirect.txt');
  await $`seq 1 250000 > ${largeRedirectPath}`;
  assert.ok(fs.statSync(largeRedirectPath).size > 1_000_000);
  assert.strictEqual(await $`wc -l < ${largeRedirectPath}`.text(), '250000\n');

  const failed = await $`exit 7`.nothrow();
  assert.strictEqual(failed.exitCode, 7);
  assert.strictEqual('exited' in failed, false);
  const exitedEarly = await $`printf before; exit 17; printf after`.nothrow();
  assert.strictEqual(exitedEarly.exitCode, 17);
  assert.strictEqual(exitedEarly.text(), 'before');
  const missingCommand = await $`ant-shell-command-that-does-not-exist`.nothrow();
  assert.strictEqual(missingCommand.exitCode, 127);
  assert.ok(decode(missingCommand.stderr).includes('ENOENT'));
  const missingInPipeline = await $`ant-shell-command-that-does-not-exist | cat`.nothrow();
  assert.strictEqual(missingInPipeline.exitCode, 0);
  assert.ok(decode(missingInPipeline.stderr).includes('ENOENT'));
  const missingAtPipelineEnd = await $`printf input | ant-shell-command-that-does-not-exist`.nothrow();
  assert.strictEqual(missingAtPipelineEnd.exitCode, 127);
  assert.ok(decode(missingAtPipelineEnd.stderr).includes('ENOENT'));
  let thrown;
  try {
    await $`exit 8`;
  } catch (error) {
    thrown = error;
  }
  assert.ok(thrown instanceof Error);
  assert.strictEqual(thrown.exitCode, 8);
  assert.ok(thrown.stdout instanceof Uint8Array);
  assert.ok(thrown.stderr instanceof Uint8Array);
  assert.strictEqual(await $`exit 9`.catch(error => error.exitCode), 9);

  async function cached(value) {
    return $`printf %s ${value}`.text();
  }
  assert.strictEqual(await cached('first'), 'first');
  assert.strictEqual(await cached('second'), 'second');

  await expectShellRejection(() => $`printf %s ${'argument\0suffix'}`, /NUL/, TypeError);
  await expectShellRejection(() => $`${'printf\0suffix'} ignored`, /NUL/, TypeError);

  let stringOverloadFailure;
  try {
    $('printf string-argument');
  } catch (error) {
    stringOverloadFailure = error;
  }
  assert.ok(stringOverloadFailure instanceof TypeError);
  assert.match(stringOverloadFailure.message, /tagged template/);
  let syntaxThrown = false;
  try {
    $`echo "unterminated`;
  } catch (error) {
    syntaxThrown = error instanceof SyntaxError;
  }
  assert.strictEqual(syntaxThrown, true);

  const invalidSeparatorPath = path.join(tmpDir, 'invalid-separator.txt');
  for (const invoke of [() => $`; echo unexpected > ${invalidSeparatorPath}`, () => $`echo one;; echo unexpected > ${invalidSeparatorPath}`]) {
    await expectShellRejection(invoke, /expected a command|unexpected separator/i, SyntaxError);
  }
  assert.strictEqual(fs.existsSync(invalidSeparatorPath), false);
  assert.strictEqual(
    await $`

echo newline-separated

`.text(),
    'newline-separated\n'
  );
} finally {
  fs.rmSync(tmpDir, { recursive: true, force: true });
}

console.log('ant:shell Silver frontend tests passed');
