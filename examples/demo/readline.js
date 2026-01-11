import * as readline from 'node:readline';

const COLORS = {
  reset: '\x1b[0m',
  bold: '\x1b[1m',
  dim: '\x1b[2m',
  italic: '\x1b[3m',
  underline: '\x1b[4m',

  black: '\x1b[30m',
  red: '\x1b[31m',
  green: '\x1b[32m',
  yellow: '\x1b[33m',
  blue: '\x1b[34m',
  magenta: '\x1b[35m',
  cyan: '\x1b[36m',
  white: '\x1b[37m',

  bgBlack: '\x1b[40m',
  bgRed: '\x1b[41m',
  bgGreen: '\x1b[42m',
  bgYellow: '\x1b[43m',
  bgBlue: '\x1b[44m',
  bgMagenta: '\x1b[45m',
  bgCyan: '\x1b[46m',
  bgWhite: '\x1b[47m'
};

const c = (color, text) => `${COLORS[color]}${text}${COLORS.reset}`;

const LOGO = `
  ${c('yellow', '╔═══════════════════════════════════════════════════════════╗')}
  ${c('yellow', '║')}  ${c('bold', c('red', '🐜 Ant JavaScript'))} ${c('dim', '- Interactive Task Manager')} ${' '.repeat(11)} ${c('yellow', '║')}
  ${c('yellow', '╚═══════════════════════════════════════════════════════════╝')}
`;

const HELP = `  ${c('bold', 'Commands:')}
  ${c('green', 'add')} ${c('dim', '<task>')}      Add a new task
  ${c('green', 'list')}            List all tasks  
  ${c('green', 'done')} ${c('dim', '<id>')}       Mark task as complete
  ${c('green', 'remove')} ${c('dim', '<id>')}     Remove a task
  ${c('green', 'clear')}           Clear all tasks
  ${c('green', 'stats')}           Show statistics
  ${c('green', 'help')}            Show this help
  ${c('green', 'quit')}            Exit the app`;

class TaskManager {
  constructor() {
    this.tasks = [];
    this.nextId = 1;
  }

  add(description) {
    const task = {
      id: this.nextId++,
      description,
      done: false,
      createdAt: new Date()
    };
    this.tasks.push(task);
    return task;
  }

  list() {
    return this.tasks;
  }

  markDone(id) {
    const task = this.tasks.find(t => t.id === id);
    if (task) {
      task.done = true;
      task.completedAt = new Date();
      return task;
    }
    return null;
  }

  remove(id) {
    const idx = this.tasks.findIndex(t => t.id === id);
    if (idx !== -1) {
      return this.tasks.splice(idx, 1)[0];
    }
    return null;
  }

  clear() {
    const count = this.tasks.length;
    this.tasks = [];
    return count;
  }

  stats() {
    const total = this.tasks.length;
    const completed = this.tasks.filter(t => t.done).length;
    const pending = total - completed;
    return { total, completed, pending };
  }
}

function formatTask(task) {
  const checkbox = task.done ? c('green', '✓') : c('dim', '○');
  const id = c('dim', `#${task.id}`);
  const desc = task.done ? c('dim', task.description) : task.description;
  return `  ${checkbox} ${id} ${desc}`;
}

function progressBar(current, total, width = 20) {
  if (total === 0) return c('dim', '░'.repeat(width));
  const filled = Math.round((current / total) * width);
  const empty = width - filled;
  return c('green', '█'.repeat(filled)) + c('dim', '░'.repeat(empty));
}

function main() {
  const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    prompt: `${c('cyan', '❯')} `,
    historySize: 100,
    removeHistoryDuplicates: true
  });

  const manager = new TaskManager();

  console.log(LOGO);
  console.log(c('dim', '  Type "help" for available commands\n'));

  manager.add('Learn the Ant JavaScript runtime');
  manager.add('Build something awesome');
  manager.add('Read the documentation');

  rl.prompt();

  rl.on('line', input => {
    const line = input.trim();
    const [cmd, ...args] = line.split(/\s+/);
    const arg = args.join(' ');

    console.log();

    switch (cmd?.toLowerCase()) {
      case 'add': {
        if (!arg) {
          console.log(`  ${c('red', '✗')} Please provide a task description`);
          break;
        }
        const task = manager.add(arg);
        console.log(`  ${c('green', '✓')} Added: ${formatTask(task)}`);
        break;
      }

      case 'list':
      case 'ls': {
        const tasks = manager.list();
        if (tasks.length === 0) {
          console.log(`  ${c('dim', 'No tasks yet. Use "add <task>" to create one.')}`);
        } else {
          console.log(`  ${c('bold', 'Tasks:')}\n`);
          for (const task of tasks) {
            console.log(formatTask(task));
          }
        }
        break;
      }

      case 'done':
      case 'complete': {
        const id = parseInt(arg);
        if (isNaN(id)) {
          console.log(`  ${c('red', '✗')} Please provide a valid task ID`);
          break;
        }
        const task = manager.markDone(id);
        if (task) {
          console.log(`  ${c('green', '✓')} Completed: ${formatTask(task)}`);
        } else {
          console.log(`  ${c('red', '✗')} Task #${id} not found`);
        }
        break;
      }

      case 'remove':
      case 'rm':
      case 'delete': {
        const id = parseInt(arg);
        if (isNaN(id)) {
          console.log(`  ${c('red', '✗')} Please provide a valid task ID`);
          break;
        }
        const task = manager.remove(id);
        if (task) {
          console.log(`  ${c('yellow', '✗')} Removed: ${c('dim', task.description)}`);
        } else {
          console.log(`  ${c('red', '✗')} Task #${id} not found`);
        }
        break;
      }

      case 'clear': {
        const count = manager.clear();
        console.log(`  ${c('yellow', '✗')} Cleared ${count} task(s)`);
        break;
      }

      case 'stats': {
        const stats = manager.stats();
        const percent = stats.total > 0 ? Math.round((stats.completed / stats.total) * 100) : 0;

        console.log(`  ${c('bold', 'Statistics:')}\n`);
        console.log(`  Total:     ${c('cyan', stats.total)}`);
        console.log(`  Completed: ${c('green', stats.completed)}`);
        console.log(`  Pending:   ${c('yellow', stats.pending)}`);
        console.log();
        console.log(`  Progress:  ${progressBar(stats.completed, stats.total)} ${percent}%`);
        break;
      }

      case 'help':
      case '?': {
        console.log(HELP);
        break;
      }

      case 'quit':
      case 'exit':
      case 'q': {
        console.log(`  ${c('cyan', 'Goodbye!')} 👋`);
        rl.close();
        return;
      }

      case '': {
        break;
      }

      default: {
        console.log(`  ${c('red', '✗')} Unknown command: ${c('dim', cmd)}`);
        console.log(`  ${c('dim', 'Type "help" for available commands')}`);
      }
    }

    console.log();
    rl.prompt();
  });

  rl.on('close', () => {
    process.exit(0);
  });
}

main();
