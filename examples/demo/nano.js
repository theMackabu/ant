#!/usr/bin/env ant

import fs from 'node:fs';
import path from 'node:path';

const { stdin, stdout } = process;
let file = process.argv[2];
let highlight = file ? /\.(c|m)?(j|t)s$/.test(path.extname(file)) : false;

let lines = [''];
let modified = false;

let row = 0;
let col = 0;

let scrollY = 0;
let scrollX = 0;

let message = '';
let msgTimer = null;

let mode = 'edit';
let prompt = '';
let promptCb = null;
let promptBuf = '';
let promptKeys = null;
let searchTerm = '';

if (file) {
  try {
    const content = fs.readFileSync(file, 'utf8');
    lines = content.split('\n');
    if (lines[lines.length - 1] === '') lines.pop();
    if (!lines.length) lines = [''];
    setMessage(`[ Read ${lines.length} line${lines.length !== 1 ? 's' : ''} ]`);
  } catch {
    setMessage('[ New File ]');
  }
} else {
  setMessage('[ New File ]');
}

function setMessage(msg) {
  if (msgTimer) clearTimeout(msgTimer);
  message = msg;
  msgTimer = setTimeout(() => {
    message = '';
    draw();
  }, 1500);
}

function rows() {
  return stdout.rows - 4;
}

function cols() {
  return stdout.columns;
}

function clamp() {
  if (row < 0) row = 0;
  if (row >= lines.length) row = lines.length - 1;
  if (col < 0) col = 0;
  if (col > lines[row].length) col = lines[row].length;
  if (row < scrollY) scrollY = row;
  if (row >= scrollY + rows()) scrollY = row - rows() + 1;
  if (col < scrollX) scrollX = col;
  if (col >= scrollX + cols()) scrollX = col - cols() + 1;
}

function shortcutRow(shortcuts, w, totalCols) {
  const cols = totalCols || shortcuts.length;
  const cellW = Math.floor(w / cols);
  let out = '';

  for (let i = 0; i < shortcuts.length; i++) {
    const [key, label] = shortcuts[i];
    if (!key && !label) {
      out += ' '.repeat(cellW);
      continue;
    }
    const cell = `\x1b[7m${key}\x1b[0m ${label}`;
    const padLen = cellW - key.length - label.length - 1;
    out += cell + (padLen > 0 ? ' '.repeat(padLen) : '');
  }

  return out;
}

function draw() {
  const h = rows();
  const w = cols();
  let out = '\x1b[H';

  const titleL = `  ANT nano ${Ant.version.match(/^\d+\.\d+\.\d+/)[0]}`;
  const titleC = file || 'New Buffer';
  const titleR = modified ? 'Modified' : '';

  const cpad = Math.max(0, Math.floor((w - titleC.length) / 2) - titleL.length);
  const rpad = Math.max(0, w - titleL.length - cpad - titleC.length - titleR.length);
  out += `\x1b[7m${titleL}${' '.repeat(cpad)}${titleC}${' '.repeat(rpad)}${titleR}\x1b[0m`;

  const visible = lines.slice(scrollY, scrollY + h);
  const raw = visible.map(l => l.slice(scrollX, scrollX + w));
  let body;

  if (highlight) {
    const joined = raw.join('\n');
    body = Ant.highlight(joined).split('\n');
  } else {
    body = raw;
  }

  if (searchTerm) {
    const visRow = row - scrollY;
    const matchCol = col - scrollX;
    const matchEnd = matchCol + searchTerm.length;
    if (visRow >= 0 && visRow < body.length && body[visRow] && matchCol >= 0) {
      const rendered = body[visRow];
      let result = '';
      let visIdx = 0;
      let inHL = false;
      for (let bi = 0; bi < rendered.length; ) {
        if (rendered[bi] === '\x1b') {
          const end = rendered.indexOf('m', bi);
          if (end !== -1) {
            result += rendered.slice(bi, end + 1);
            bi = end + 1;
            continue;
          }
        }
        if (visIdx === matchCol && !inHL) {
          result += '\x1b[43;30m';
          inHL = true;
        }
        if (visIdx === matchEnd && inHL) {
          result += '\x1b[0m';
          inHL = false;
        }
        result += rendered[bi++];
        visIdx++;
      }
      if (inHL) result += '\x1b[0m';
      body[visRow] = result;
    }
  }

  for (let i = 0; i < h; i++) {
    out += `\x1b[${i + 2};1H\x1b[2K`;
    out += body[i] || '';
  }

  const statusRow = h + 2;
  out += `\x1b[${statusRow};1H\x1b[2K`;

  if (mode === 'yesno') {
    const q = 'Save modified buffer?';
    out += `\x1b[7m${q}${' '.repeat(Math.max(0, w - q.length))}\x1b[0m`;
  } else if (mode === 'prompt') {
    const promptText = prompt + promptBuf;
    out += `\x1b[7m${promptText}${' '.repeat(Math.max(0, w - promptText.length))}\x1b[0m`;
  } else if (message) {
    const pad = Math.max(0, Math.floor((w - message.length) / 2));
    out += ' '.repeat(pad) + `\x1b[7m${message}\x1b[0m`;
  }

  let row1, row2;

  if ((mode === 'prompt' || mode === 'yesno') && promptKeys) {
    row1 = promptKeys[0];
    row2 = promptKeys[1];
  } else {
    row1 = [
      ['^G', 'Help'],
      ['^X', 'Exit'],
      ['^O', 'Write'],
      ['^W', 'Search'],
      ['^K', 'Cut'],
      ['^U', 'Paste']
    ];
    row2 = [
      ['^A', 'Home'],
      ['^E', 'End'],
      ['^\\', 'Replace'],
      ['^T', 'Execute'],
      ['^J', 'Justify'],
      ['^C', 'Location']
    ];
  }

  out += `\x1b[${h + 3};1H\x1b[2K`;
  out += shortcutRow(row1, w);
  out += `\x1b[${h + 4};1H\x1b[2K`;
  out += shortcutRow(row2, w);

  if (mode === 'yesno') {
    out += `\x1b[${statusRow};${'Save modified buffer?'.length + 2}H`;
  } else if (mode === 'prompt') {
    out += `\x1b[${statusRow};${prompt.length + promptBuf.length + 1}H`;
  } else {
    const cy = row - scrollY + 2;
    const cx = col - scrollX + 1;
    out += `\x1b[${cy};${cx}H`;
  }

  stdout.write(out);
}

let cutBuf = [];

const writePromptKeys = [
  [
    ['^G', 'Help'],
    ['M-D', 'DOS Format'],
    ['M-A', 'Append'],
    ['M-B', 'Backup File'],
    ['^T', 'Browse']
  ],
  [
    ['^C', 'Cancel'],
    ['M-M', 'Mac Format'],
    ['M-P', 'Prepend'],
    ['^Q', 'Discard buffer'],
    ['', '']
  ]
];

function handleYesNo(buf) {
  const ch = buf[0];
  if (ch === 0x79 || ch === 0x59) {
    mode = 'edit';
    promptKeys = null;
    if (file) {
      save(file);
      quit();
      return;
    }
    startPrompt(
      'Write to File: ',
      name => {
        if (!name) {
          setMessage('[ No file name given ]');
          return;
        }
        save(name);
        quit();
      },
      writePromptKeys
    );
  } else if (ch === 0x6e || ch === 0x4e) {
    mode = 'edit';
    promptKeys = null;
    quit();
  } else if (ch === 0x03) {
    mode = 'edit';
    promptKeys = null;
    setMessage('[ Cancelled ]');
  }
}

function handleKey(buf) {
  if (mode === 'yesno') {
    handleYesNo(buf);
    return;
  }

  if (mode === 'prompt') {
    handlePrompt(buf);
    return;
  }

  message = '';
  const ch = buf[0];

  if (ch === 0x1b && buf[1] === 0x5b) {
    const code = buf[2];
    if (code === 0x41) row--;
    else if (code === 0x42) row++;
    else if (code === 0x43) col++;
    else if (code === 0x44) col--;
    else if (code === 0x48) col = 0;
    else if (code === 0x46) col = lines[row].length;
    else if (code === 0x35 && buf[3] === 0x7e) {
      row -= rows();
    } else if (code === 0x36 && buf[3] === 0x7e) {
      row += rows();
    }
    clamp();
    return;
  }

  if (ch === 0x18) {
    if (modified) {
      mode = 'yesno';
      promptKeys = [
        [
          ['Y', 'Yes'],
          ['', ''],
          ['', ''],
          ['', ''],
          ['', ''],
          ['', '']
        ],
        [
          ['N', 'No'],
          ['', ''],
          ['^C', 'Cancel'],
          ['', ''],
          ['', ''],
          ['', '']
        ]
      ];
    } else quit();
    return;
  }

  if (ch === 0x0f) {
    startPrompt(
      'Write to File: ',
      name => {
        const target = name || file;
        if (!target) {
          setMessage('[ No file name given ]');
          return;
        }
        save(target);
        setMessage(`[ Wrote ${lines.length} line${lines.length !== 1 ? 's' : ''} ]`);
      },
      writePromptKeys
    );
    return;
  }

  if (ch === 0x17) {
    startPrompt(
      'Search: ',
      term => {
        if (term) searchTerm = term;
        if (!searchTerm) return;
        for (let i = row; i < lines.length; i++) {
          const idx = lines[i].indexOf(searchTerm, i === row ? col + 1 : 0);
          if (idx !== -1) {
            row = i;
            col = idx;
            clamp();
            return;
          }
        }
        for (let i = 0; i <= row; i++) {
          const idx = lines[i].indexOf(searchTerm);
          if (idx !== -1) {
            row = i;
            col = idx;
            clamp();
            return;
          }
        }
        setMessage(`[ "${searchTerm}" not found ]`);
      },
      null,
      searchTerm
    );
    return;
  }

  if (ch === 0x0b) {
    cutBuf = [lines[row]];
    lines.splice(row, 1);
    if (!lines.length) lines = [''];
    modified = true;
    clamp();
    return;
  }

  if (ch === 0x15) {
    if (cutBuf.length) {
      lines.splice(row, 0, ...cutBuf);
      modified = true;
      clamp();
    }
    return;
  }

  if (ch === 0x01) {
    col = 0;
    return;
  }
  if (ch === 0x05) {
    col = lines[row].length;
    return;
  }

  if (ch === 0x7f) {
    if (col > 0) {
      lines[row] = lines[row].slice(0, col - 1) + lines[row].slice(col);
      col--;
    } else if (row > 0) {
      col = lines[row - 1].length;
      lines[row - 1] += lines[row];
      lines.splice(row, 1);
      row--;
    }
    modified = true;
    clamp();
    return;
  }

  if (ch === 0x0d) {
    const rest = lines[row].slice(col);
    lines[row] = lines[row].slice(0, col);
    lines.splice(row + 1, 0, rest);
    row++;
    col = 0;
    modified = true;
    clamp();
    return;
  }

  if (ch === 0x09 || ch >= 0x20) {
    const c = ch === 0x09 ? '\t' : String.fromCharCode(ch);
    lines[row] = lines[row].slice(0, col) + c + lines[row].slice(col);
    col++;
    modified = true;
    clamp();
    return;
  }
}

function startPrompt(msg, cb, keys, prefill) {
  mode = 'prompt';
  prompt = msg;
  promptBuf = prefill || '';
  promptCb = cb;
  promptKeys = keys || null;
}

function handlePrompt(buf) {
  const ch = buf[0];
  if (ch === 0x0d) {
    mode = 'edit';
    promptKeys = null;
    const cb = promptCb;
    promptCb = null;
    cb(promptBuf);
    return;
  }
  if (ch === 0x03) {
    mode = 'edit';
    promptCb = null;
    promptKeys = null;
    setMessage('[ Cancelled ]');
    return;
  }
  if (ch === 0x1b) return;
  if (ch === 0x7f) {
    promptBuf = promptBuf.slice(0, -1);
    return;
  }
  if (ch >= 0x20) {
    promptBuf += String.fromCharCode(ch);
  }
}

function save(target) {
  file = target;
  highlight = /\.(c|m)?(j|t)s$/.test(path.extname(file));
  fs.writeFileSync(file, lines.join('\n') + '\n');
  modified = false;
}

function quit() {
  stdin.setRawMode(false);
  stdout.write('\x1b[?1049l');
  stdout.write('\x1b[?25h');
  process.exit(0);
}

stdout.write('\x1b[?1049h');
stdout.write('\x1b[?25h');
stdin.setRawMode(true);
stdin.resume();
draw();

stdin.on('data', buf => {
  handleKey(buf);
  draw();
});

stdout.on('resize', () => draw());
