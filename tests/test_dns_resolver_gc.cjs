const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const dns = require('node:dns');
const { spawn, spawnSync } = require('node:child_process');

if (spawnSync('python3', ['--version']).status !== 0 || !fs.existsSync('/dev/fd')) {
  console.log('dns resolver gc skipped: python3 or /dev/fd is not available');
  process.exit(0);
}

const fixture = path.join(__dirname, 'fixtures', 'dns_server.py');
const server = spawn('python3', [fixture], { stdio: ['pipe', 'pipe', 'inherit'] });
const ROUNDS = 12;

function openDescriptors() {
  return fs.readdirSync('/dev/fd').length;
}

function churn() {
  const junk = [];
  for (let i = 0; i < 1500000; i++) junk.push({ i });
  return junk.length;
}

function query(address, name, done) {
  const resolver = new dns.Resolver({ timeout: 300, tries: 1 });
  resolver.setServers([address]);
  resolver.resolve4(name, (err, records) => {
    churn();
    churn();
    done(err, records);
  });
}

server.stdout.once('data', chunk => {
  const address = `127.0.0.1:${String(chunk).trim()}`;
  const before = openDescriptors();
  let round = 0;

  const finish = () => {
    setTimeout(() => {
      try {
        assert.strictEqual(openDescriptors(), before, 'a resolver collected inside its own callback leaves its socket open');
        console.log('ok');
      } finally {
        server.stdin.end();
      }
    }, 50);
  };

  const next = () => {
    if (round === ROUNDS) return finish();
    const missing = round++ % 2 === 1;
    query(address, missing ? 'missing.fixture.test' : 'fixture.test', (err, records) => {
      try {
        if (missing) assert.strictEqual(err.code, 'ENOTFOUND');
        else assert.deepStrictEqual(records, ['192.0.2.1', '192.0.2.2']);
      } catch (e) {
        server.stdin.end();
        throw e;
      }
      next();
    });
  };

  next();
});
