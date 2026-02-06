import { Screen, colors, codes, pad, truncate } from '../examples/tui/tuey.js';

const screen = new Screen({ fullscreen: false, hideCursor: false });

const logs = [
  { time: '10:23:45', level: 'INFO', message: 'Application started' },
  { time: '10:23:48', level: 'WARN', message: 'Cache directory not found, creating...' },
  { time: '10:24:05', level: 'ERROR', message: 'Failed to load plugin: missing-plugin' },
  { time: '10:24:06', level: 'WARN', message: 'Running with reduced functionality' },
  { time: '10:24:10', level: 'INFO', message: 'Ready to accept connections' },
];

function render() {
  screen.write(2, 14, colors.bold + colors.magenta + '┌─ Recent Activity ───────────────────────┐' + codes.reset);
  for (let i = 0; i < 5; i++) {
    const log = logs[i];
    const levelColor = log.level === 'ERROR' ? colors.red : log.level === 'WARN' ? colors.yellow : colors.green;
    screen.write(2, 15 + i, colors.magenta + '│' + codes.reset);
    screen.write(4, 15 + i, `${colors.dim}${log.time}${codes.reset} ${levelColor}${log.level}${codes.reset} ${truncate(log.message, 30)}`);
    screen.write(45, 15 + i, colors.magenta + '│' + codes.reset);
  }
  screen.write(2, 20, colors.magenta + '└──────────────────────────────────────────┘' + codes.reset);
}

for (let frame = 0; frame < 50000; frame++) {
  render();
}

console.log('OK');
