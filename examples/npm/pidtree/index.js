import pidtree from 'pidtree';
import { spawn } from 'node:child_process';

async function run() {
  console.log('1. Looking up own pid with pidtree...');
  const self = process.pid;
  const selfTree = await pidtree(self, { root: true });
  if (!Array.isArray(selfTree) || !selfTree.includes(self)) {
    console.log('FAIL — could not find own pid %d in tree: %o', self, selfTree);
    process.exit(1);
  }
  console.log('   OK — found own pid %d in tree', self);

  console.log('2. Spawning a child and looking it up...');
  const child = spawn('sleep', ['10']);
  const childPid = child.pid;

  await new Promise(r => setTimeout(r, 100));

  const childTree = await pidtree(self, { root: true });
  if (!childTree.includes(childPid)) {
    console.log('FAIL — child pid %d not found in tree: %o', childPid, childTree);
    child.kill();
    process.exit(1);
  }
  console.log('   OK — child pid %d found in tree', childPid);

  console.log('3. Testing { root: false } excludes root...');
  const noRoot = await pidtree(self);
  if (noRoot.includes(self)) {
    console.log('FAIL — root pid should be excluded');
    child.kill();
    process.exit(1);
  }
  if (!noRoot.includes(childPid)) {
    console.log('FAIL — child pid %d not found without root: %o', childPid, noRoot);
    child.kill();
    process.exit(1);
  }
  console.log('   OK — root excluded, child present');

  console.log('4. Testing { advanced: true } format...');
  const advanced = await pidtree(self, { root: true, advanced: true });
  const selfEntry = advanced.find(e => e.pid === self);
  const childEntry = advanced.find(e => e.pid === childPid);
  if (!selfEntry || !childEntry) {
    console.log('FAIL — missing entries in advanced output: %o', advanced);
    child.kill();
    process.exit(1);
  }
  if (childEntry.ppid !== self) {
    console.log('FAIL — child ppid should be %d, got %d', self, childEntry.ppid);
    child.kill();
    process.exit(1);
  }
  console.log('   OK — advanced format correct (child ppid=%d)', childEntry.ppid);

  console.log('5. Testing pid -1 lists all pids...');
  const all = await pidtree(-1);
  if (!Array.isArray(all) || all.length < 2) {
    console.log('FAIL — expected many pids, got %d', all ? all.length : 0);
    child.kill();
    process.exit(1);
  }
  console.log('   OK — got %d pids from system', all.length);

  console.log('6. Testing stdio: "inherit" on inner spawn...');
  const inheritChild = spawn('echo', ['hello'], { stdio: 'inherit' });
  if (inheritChild.stdin !== null || inheritChild.stdout !== null || inheritChild.stderr !== null) {
    console.log('FAIL — stdio streams should be null with inherit');
    child.kill();
    process.exit(1);
  }
  await new Promise(r => inheritChild.on('close', r));
  console.log('   OK — inherited stdio, streams are null');

  child.kill();
  await new Promise(r => child.on('close', r));

  console.log('\nAll good.');
}

run().catch(err => {
  console.error('FAIL —', err.message);
  process.exit(1);
});
