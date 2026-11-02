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
  assert.ok(Buffer.isBuffer(simple.stdout));
  assert.ok(Buffer.isBuffer(simple.stderr));
  assert.strictEqual(simple.stdout.toString('utf8'), 'Hello, world!\n');
  assert.strictEqual(simple.stdout.toString('hex'), '48656c6c6f2c20776f726c64210a');
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
  assert.strictEqual(binaryOutput.stdout.toString('hex'), 'ff0041');
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
  assert.strictEqual(
    await $`printf multiline-pipe |

    cat`.text(),
    'multiline-pipe'
  );
  assert.strictEqual(
    await $`false ||

    echo multiline-or`.text(),
    'multiline-or\n'
  );
  assert.strictEqual(
    await $`true &&

    echo multiline-and`.text(),
    'multiline-and\n'
  );
  assert.strictEqual(await $`seq 1 10000 | wc -l`.text(), '10000\n');
  assert.strictEqual(await $`yes | head -1`.text(), 'y\n');

  const emptyCommand = await $`${''}`.nothrow();
  assert.strictEqual(emptyCommand.exitCode, 127);
  assert.match(decode(emptyCommand.stderr), /command not found/);
  assert.strictEqual(await $`i=0; while [ $i -lt 3 ]; do echo $i; i=$((i + 1)); done`.text(), '0\n1\n2\n');
  assert.strictEqual(await $`whi\
le false; do :; done; echo after`.text(), 'after\n');
  assert.strictEqual(await $`name=value sh -c 'echo $name'`.text(), 'value\n');
  assert.strictEqual(await $`NA\
ME=joined; echo $NAME`.text(), 'joined\n');
  assert.strictEqual(await $`echo "$HOME"`.text(), `${process.env.HOME}\n`);
  assert.strictEqual(await $`echo $((1 + 2))`.text(), '3\n');
  assert.strictEqual(await $`cd ${tmpDir}; echo $(pwd)`.text(), `${tmpDir}\n`);
  assert.strictEqual(await $`echo '$HOME'`.text(), '$HOME\n');
  assert.strictEqual(await $`echo \$HOME`.text(), '$HOME\n');
  assert.strictEqual(await $`echo name=value`.text(), 'name=value\n');
  assert.strictEqual(await $`x=outer; f() { local x=inner; echo \${x}; }; f; echo \${x:-unset}`.text(), 'inner\nouter\n');

  assert.strictEqual(await $`x=1; echo \${x:-unset} \${y:-default}`.text(), '1 default\n');
  assert.strictEqual(await $`for f in ${['a b', 'c']}; do echo "[$f]"; done`.text(), '[a b]\n[c]\n');
  assert.strictEqual(await $`echo ${'*'} ${'a  b'}`.text(), '* a  b\n');
  assert.strictEqual(await $`x=${'a b'}; echo "$x" \${#x}`.text(), 'a b 3\n');
  assert.strictEqual(await $`case foo in ${'f*'}) echo glob;; foo) echo literal;; esac`.text(), 'literal\n');
  assert.strictEqual(await $`echo $((${5} + 1))`.text(), '6\n');
  assert.strictEqual(await $`echo \`echo backquoted\``.text(), 'backquoted\n');
  assert.strictEqual(
    await $`cat <<EOF
value=${'interpolated'} \${HOME:+home}
EOF`.text(),
    'value=interpolated home\n'
  );
  assert.strictEqual(await $`printf '%s\n' c b a | sort | head -n 1`.text(), 'a\n');
  assert.strictEqual(await $`f() { echo "fn:$1"; }; ${'f'} ${'arg'}`.text(), 'fn:arg\n');
  const loopPath = path.join(tmpDir, 'loop.txt');
  await $`for i in 1 2 3; do echo $i; done > ${loopPath}`;
  assert.strictEqual(fs.readFileSync(loopPath, 'utf8'), '1\n2\n3\n');
  assert.strictEqual(await $`while read -r line; do echo "<$line>"; done < ${loopPath}`.text(), '<1>\n<2>\n<3>\n');
  const errexit = await $`set -e; false; echo unreachable`.nothrow();
  assert.strictEqual(errexit.exitCode, 1);
  assert.strictEqual(errexit.text(), '');
  const arithError = await $`echo $((1 / 0)); echo same-line
echo next-line`.nothrow();
  assert.strictEqual(arithError.text(), 'next-line\n');
  assert.match(decode(arithError.stderr), /division by 0/);

  assert.strictEqual(await $`yes | while read -r line; do echo $line; break; done`.text(), 'y\n');
  assert.strictEqual(await $`printf 'header\na\nb\n' | { read -r first; cat; }`.text(), 'a\nb\n');
  assert.strictEqual(await $`(echo one; sleep 0.05; echo two) | while read -r l; do echo "got $l"; done`.text(), 'got one\ngot two\n');
  assert.strictEqual(await $`seq 3 | while read -r n; do echo $n; done | sort -r`.text(), '3\n2\n1\n');
  assert.strictEqual(await $`shopt -s lastpipe; echo kept | read -r v; echo $v`.text(), 'kept\n');
  assert.strictEqual(await $`echo tee-me | tee >(tr a-z A-Z) >/dev/null`.text(), 'TEE-ME\n');
  assert.strictEqual(await $`diff <(printf 'a\n') <(printf 'a\n') && echo same`.text(), 'same\n');

  assert.strictEqual(await $`seq 20000 | cat | while read -r l; do echo $l; done | while read -r l; do echo $l; done | while read -r l; do echo $l; done | while read -r l; do echo $l; done | while read -r l; do echo $l; done | while read -r l; do echo $l; done | tail -n 1`.text(), '20000\n');
  assert.strictEqual(await $`n=0; while :; do echo $((n++)); done | head -n 2`.text(), '0\n1\n');
  const realPid = await $`sleep 5 & echo $!; kill $!; wait`.nothrow();
  const pid = Number(realPid.text().trim());
  assert.ok(pid > 0 && pid < 1 << 30, `expected a real pid, got ${realPid.text()}`);
  assert.throws(() => process.kill(pid, 0));
  for (let i = 0; i < 3; i++) {
    assert.strictEqual(await $`read -t 0.05 x < <(sleep 1); echo st=$?; echo fresh | (read -r y; echo $y)`.text(), 'st=142\nfresh\n');
  }
  const umaskBefore = process.umask();
  await $`umask 077; ulimit -n 64; true`;
  assert.strictEqual(process.umask(), umaskBefore);
  const umaskFile = path.join(tmpDir, 'umask-file');
  await $`(umask 077; : > ${umaskFile}); ulimit -n 100; sh -c 'ulimit -n' > ${umaskFile}.limit`;
  assert.strictEqual(fs.statSync(umaskFile).mode & 0o777, 0o600);
  assert.strictEqual(fs.readFileSync(`${umaskFile}.limit`, 'utf8'), '100\n');
  assert.strictEqual(await $`coproc { read -r line; echo "got:$line"; }; echo ping >&\${COPROC[1]}; read -r -u \${COPROC[0]} reply; echo $reply`.text(), 'got:ping\n');
  assert.strictEqual(await $`trap 'echo trapped' USR1; kill -USR1 $$; sleep 0.05; echo after`.text(), 'trapped\nafter\n');

  const syntaxErrorPath = path.join(tmpDir, 'syntax-error.txt');
  await expectShellRejection(() => $`echo must-not-run > ${syntaxErrorPath}; while true; do echo bad`, /expected 'done'/, SyntaxError);
  assert.strictEqual(fs.existsSync(syntaxErrorPath), false);

  const cwdResult = await $`pwd; cd ${tmpDir}; pwd; pwd -P`;
  const cwdLines = cwdResult.text().trim().split('\n');
  assert.strictEqual(cwdLines.length, 3);
  assert.strictEqual(cwdLines[1], tmpDir);
  assert.strictEqual(cwdLines[2], fs.realpathSync(tmpDir));

  if (process.platform === 'win32') {
    const originalHome = process.env.HOME;
    const originalUserProfile = process.env.USERPROFILE;
    const originalHomeDrive = process.env.HOMEDRIVE;
    const originalHomePath = process.env.HOMEPATH;
    const restore = (name, value) => {
      if (value === undefined) delete process.env[name];
      else process.env[name] = value;
    };
    try {
      delete process.env.HOME;
      process.env.USERPROFILE = tmpDir;
      assert.strictEqual((await $`cd; pwd`).text().trim(), fs.realpathSync(tmpDir));

      delete process.env.USERPROFILE;
      process.env.HOMEDRIVE = tmpDir.slice(0, 2);
      process.env.HOMEPATH = tmpDir.slice(2);
      assert.strictEqual((await $`cd; pwd`).text().trim(), fs.realpathSync(tmpDir));
    } finally {
      restore('HOME', originalHome);
      restore('USERPROFILE', originalUserProfile);
      restore('HOMEDRIVE', originalHomeDrive);
      restore('HOMEPATH', originalHomePath);
    }
  }

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

  const stderrPath = path.join(tmpDir, 'stderr.txt');
  const stderrRedirect = await $`ls ${path.join(tmpDir, 'missing-fd')} 2>${stderrPath}`.nothrow();
  assert.notStrictEqual(stderrRedirect.exitCode, 0);
  assert.strictEqual(stderrRedirect.stderr.length, 0);
  assert.ok(fs.readFileSync(stderrPath, 'utf8').length > 0);
  const badFd = await $`printf x 2>&7`.nothrow();
  assert.strictEqual(badFd.exitCode, 1);
  assert.match(decode(badFd.stderr), /bad file descriptor/i);

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
  assert.match(decode(missingCommand.stderr), /ant-shell-command-that-does-not-exist: command not found/);
  const missingInPipeline = await $`ant-shell-command-that-does-not-exist | cat`.nothrow();
  assert.strictEqual(missingInPipeline.exitCode, 0);
  assert.match(decode(missingInPipeline.stderr), /command not found/);
  const missingAtPipelineEnd = await $`printf input | ant-shell-command-that-does-not-exist`.nothrow();
  assert.strictEqual(missingAtPipelineEnd.exitCode, 127);
  assert.match(decode(missingAtPipelineEnd.stderr), /command not found/);
  const silencedMissing = await $`ant-shell-command-that-does-not-exist 2>/dev/null`.nothrow();
  assert.strictEqual(silencedMissing.exitCode, 127);
  assert.strictEqual(silencedMissing.stderr.length, 0);
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
  assert.ok(Buffer.isBuffer(thrown.stdout));
  assert.ok(Buffer.isBuffer(thrown.stderr));
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
    await expectShellRejection(invoke, /expected a command|unexpected (separator|token)/i, SyntaxError);
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
