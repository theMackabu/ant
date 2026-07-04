// Regression: mapped (sloppy-mode) arguments objects alias VM stack slots
// via a (vm, frame index) pair. That pair must track the frame when an async
// activation materializes onto its own VM mid-await — previously it was
// captured as js->vm unconditionally, so detach-on-frame-exit dereferenced a
// garbage frame index and crashed (SIGBUS) whenever a sloppy function using
// `arguments` ran inside a resumed coroutine.
//
// Known pre-existing gap (also broken on master before this fix): live
// aliasing between params and arguments[i] is not maintained across an await
// suspension (reads reflect pre-suspension values). Sync-path aliasing works.
let failures = 0;
function check(name, actual, expected) {
  const ok = JSON.stringify(actual) === JSON.stringify(expected);
  if (!ok) { failures++; console.log('FAIL', name, actual, '!=', expected); }
  else console.log('ok', name);
}

async function usesArguments(a, b) {
  const before = [arguments[0], arguments[1], arguments.length];
  await new Promise(r => setTimeout(r, 5));
  return { before, after: [arguments[0], arguments[1], arguments.length] };
}

async function escapesArguments(x) {
  await new Promise(r => setTimeout(r, 5));
  return arguments; // detached on frame exit — the crash site
}

async function outer() {
  await new Promise(r => setTimeout(r, 5));
  return inner('deep', 42); // sloppy arguments inside a resumed coroutine
}
async function inner(p, q) {
  await new Promise(r => setTimeout(r, 5));
  return [arguments[0], arguments[1], arguments.length];
}

(async () => {
  const r = await usesArguments(1, 2);
  check('arguments before await', r.before, [1, 2, 2]);
  check('arguments readable after await', r.after, [1, 2, 2]);

  const args = await escapesArguments('kept', 'extra');
  check('escaped arguments detach', [args[0], args[1], args.length], ['kept', 'extra', 2]);

  check('arguments in nested resumed activation', await outer(), ['deep', 42, 2]);

  console.log(failures === 0 ? 'ALL PASS' : failures + ' FAILURES');
  if (failures) process.exitCode = 1;
})();
