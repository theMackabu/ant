'use strict';

const fs = require('node:fs');
const readline = require('node:readline');

const mode = process.argv[2];
const done = value => console.log(JSON.stringify(value));

const modes = {
  data() {
    const chunks = [];
    process.stdin.on('data', chunk => chunks.push(chunk));
    process.stdin.on('end', () => {
      const all = Buffer.concat(chunks);
      setImmediate(() => done({ bytes: all.length, text: all.toString(), buffers: chunks.every(Buffer.isBuffer) }));
    });
  },

  encoding() {
    process.stdin.setEncoding('utf8');
    let text = '';
    let strings = true;
    process.stdin.on('data', chunk => { strings = strings && typeof chunk === 'string'; text += chunk; });
    process.stdin.on('end', () => done({ text, strings }));
  },

  async iterate() {
    const chunks = [];
    for await (const chunk of process.stdin) chunks.push(chunk);
    done({ text: Buffer.concat(chunks).toString() });
  },

  async iterateBreak() {
    let first = null;
    for await (const chunk of process.stdin) { first = chunk.length > 0; break; }
    done({ first });
  },

  readable() {
    const chunks = [];
    process.stdin.on('readable', () => {
      let chunk;
      while ((chunk = process.stdin.read()) !== null) chunks.push(chunk);
    });
    process.stdin.on('end', () => done({ text: Buffer.concat(chunks).toString() }));
  },

  pause() {
    const chunks = [];
    process.stdin.on('data', chunk => {
      chunks.push(chunk);
      process.stdin.pause();
      setTimeout(() => process.stdin.resume(), 1);
    });
    process.stdin.on('end', () => done({ text: Buffer.concat(chunks).toString() }));
  },

  readline() {
    const lines = [];
    const rl = readline.createInterface({ input: process.stdin });
    rl.on('line', line => lines.push(line));
    rl.on('close', () => done({ lines }));
  },

  syncFd() {
    done({ text: fs.readFileSync(0, 'utf8') });
  },

  syncDevStdin() {
    done({ text: fs.readFileSync('/dev/stdin').toString() });
  },

  untouched() {
    done({ ok: true });
  },
};

modes[mode]();
