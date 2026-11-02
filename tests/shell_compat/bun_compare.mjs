const { $ } = typeof Bun !== 'undefined' ? await import('bun') : await import('ant:shell');
const results = {};
const q = p => (typeof p.quiet === 'function' ? p.quiet() : p);
async function record(name, fn) {
  try {
    results[name] = await fn();
  } catch (error) {
    results[name] = { threw: error && error.constructor && error.constructor.name, exitCode: error && error.exitCode, stdout: error && error.stdout && Buffer.from(error.stdout).toString(), stderrEmpty: error && error.stderr ? error.stderr.length === 0 : null };
  }
}
await record('text', () => q($`echo hello`).text());
await record('interp', () => q($`echo ${'a b'} ${['c', 'd']}`).text());
await record('no-split', () => q($`printf '<%s>' ${'x  y'}`).text());
await record('no-glob', () => q($`echo ${'*'}`).text());
await record('injection', () => q($`echo ${'; echo pwned'}`).text());
await record('json', () => q($`echo '{"a":1}'`).json());
await record('lines', () => q($`printf 'a\nb\n'`).lines().then ? q($`printf 'a\nb\n'`).text() : null);
await record('lines-iter', async () => { const out = []; for await (const l of q($`printf 'x\ny\n'`).lines()) out.push(l); return out; });
await record('exitCode-nothrow', async () => (await q($`exit 3`).nothrow()).exitCode);
await record('throws', () => q($`exit 4`));
await record('throws-false', async () => (await q($`exit 5`).throws(false)).exitCode);
await record('stderr', async () => Buffer.from((await q($`echo err >&2`)).stderr).toString());
await record('stdout-buffer', async () => Buffer.isBuffer((await q($`echo x`)).stdout));
await record('env', () => q($`echo $FOO`.env({ FOO: 'bar', PATH: process.env.PATH })).text());
await record('cwd', () => q($`pwd`.cwd('/tmp')).text());
await record('stdin-buffer', () => q($`cat < ${Buffer.from('buf-in')}`).text());
await record('stdin-response', () => q($`cat < ${new Response('resp-in')}`).text());
await record('stdin-blob', () => q($`cat < ${new Blob(['blob-in'])}`).text());
await record('stdout-into-buffer', async () => { const b = Buffer.alloc(5); await q($`echo hello > ${b}`); return b.toString(); });
await record('raw', () => q($`echo ${{ raw: '"raw text"' }}`).text());
await record('escape', () => $.escape('$(foo) `bar` "baz"'));
await record('braces', () => $.braces('echo {1,2,3}'));
await record('pipe', () => q($`echo abc | tr a-z A-Z`).text());
await record('and-or', () => q($`false && echo no || echo yes`).text());
await record('cmdsub', () => q($`echo $(echo inner)`).text());
await record('var', () => q($`FOO=1; echo $FOO`).text());
await record('redirect-file', async () => { await q($`echo filedata > /tmp/ant-buncmp.txt`); return q($`cat /tmp/ant-buncmp.txt`).text(); });
await record('cd', () => q($`cd /tmp && pwd`).text());
await record('glob', () => q($`echo /etc/hos*`).text());
await record('not-found', async () => { const r = await q($`ant-buncmp-missing-cmd`).nothrow(); return r.exitCode; });
await record('blob-output', async () => (await q($`echo blob`).blob()).text());
await record('bytes', async () => Array.from(await q($`printf AB`).bytes()));
await record('arrayBuffer', async () => (await q($`printf ABC`).arrayBuffer()).byteLength);
await record('global-nothrow', async () => { $.nothrow(); const r = await q($`exit 6`); $.throws(true); return r.exitCode; });
await record('shell-instance', async () => { const sh = new $.Shell(); sh.cwd('/'); return q(sh`pwd`).text(); });
await record('ShellError', async () => { try { await q($`exit 7`); } catch (e) { return [e instanceof $.ShellError, e.exitCode, typeof e.text]; } });
console.log(JSON.stringify(results, null, 1));
