import { Screen, List, ProgressBar, Input, Table, colors, box, keys, codes, modal, confirm, pad, padCenter, truncate, visibleLength } from './tuey.js';

const screen = new Screen({ fullscreen: true, hideCursor: true });

const tasks = [
  { id: 1, name: 'Build TUI library', status: 'done', priority: 'high' },
  { id: 2, name: 'Add modal support', status: 'done', priority: 'high' },
  { id: 3, name: 'Implement themes', status: 'in_progress', priority: 'medium' },
  { id: 4, name: 'Write documentation', status: 'todo', priority: 'low' },
  { id: 5, name: 'Add animation support', status: 'todo', priority: 'low' },
  { id: 6, name: 'Create widget system', status: 'in_progress', priority: 'medium' },
  { id: 7, name: 'Performance optimization', status: 'todo', priority: 'high' },
  { id: 8, name: 'Add mouse support', status: 'todo', priority: 'low' },
  { id: 9, name: 'Create color picker', status: 'todo', priority: 'medium' },
  { id: 10, name: 'Build file browser', status: 'in_progress', priority: 'high' }
];

const logs = [
  { time: '10:23:45', level: 'INFO', message: 'Application started' },
  { time: '10:23:46', level: 'DEBUG', message: 'Loading configuration...' },
  { time: '10:23:47', level: 'INFO', message: 'Config loaded successfully' },
  { time: '10:23:48', level: 'WARN', message: 'Cache directory not found, creating...' },
  { time: '10:23:49', level: 'INFO', message: 'Cache initialized' },
  { time: '10:24:01', level: 'DEBUG', message: 'Connecting to database...' },
  { time: '10:24:02', level: 'INFO', message: 'Database connection established' },
  { time: '10:24:05', level: 'ERROR', message: 'Failed to load plugin: missing-plugin' },
  { time: '10:24:06', level: 'WARN', message: 'Running with reduced functionality' },
  { time: '10:24:10', level: 'INFO', message: 'Ready to accept connections' }
];

let state = {
  view: 'dashboard',
  taskFilter: 'all',
  searchMode: false,
  searchQuery: '',
  cpuUsage: 45,
  memUsage: 62,
  diskUsage: 78,
  networkIn: 0,
  networkOut: 0,
  settingsIndex: 0
};

const settings = [
  { key: 'refreshRate', label: 'Refresh Rate', type: 'cycle', options: [500, 1000, 2000, 5000], value: 1000, format: v => `${v}ms` },
  { key: 'logInterval', label: 'Log Interval', type: 'cycle', options: [1000, 2000, 5000, 10000], value: 2000, format: v => `${v / 1000}s` },
  { key: 'logLevel', label: 'Min Log Level', type: 'cycle', options: ['DEBUG', 'INFO', 'WARN', 'ERROR'], value: 'DEBUG', format: v => v },
  { key: 'boxStyle', label: 'Box Style', type: 'cycle', options: ['rounded', 'light', 'heavy', 'double'], value: 'rounded', format: v => v },
  { key: 'confirmQuit', label: 'Confirm on Quit', type: 'toggle', value: true, format: v => v ? 'On' : 'Off' },
  { key: 'simulateStats', label: 'Simulate Stats', type: 'toggle', value: true, format: v => v ? 'On' : 'Off' },
  { key: 'maxLogs', label: 'Max Log Entries', type: 'cycle', options: [50, 100, 250, 500], value: 100, format: v => String(v) }
];

function getSetting(key) {
  return settings.find(s => s.key === key).value;
}

function cycleSetting(index, direction) {
  const s = settings[index];
  if (s.type === 'toggle') {
    s.value = !s.value;
  } else if (s.type === 'cycle') {
    const idx = s.options.indexOf(s.value);
    const next = (idx + direction + s.options.length) % s.options.length;
    s.value = s.options[next];
  }
  applySetting(s.key);
}

function applySetting(key) {
  if (key === 'refreshRate') {
    clearInterval(statsTimer);
    statsTimer = setInterval(updateStats, getSetting('refreshRate'));
  } else if (key === 'logInterval') {
    clearInterval(logTimer);
    logTimer = setInterval(addRandomLog, getSetting('logInterval'));
  } else if (key === 'logLevel') {
    const levels = ['DEBUG', 'INFO', 'WARN', 'ERROR'];
    const minIdx = levels.indexOf(getSetting('logLevel'));
    const filtered = logs.filter(l => levels.indexOf(l.level) >= minIdx);
    logList.setItems(filtered);
  } else if (key === 'maxLogs') {
    const max = getSetting('maxLogs');
    while (logs.length > max) logs.shift();
    logList.setItems(logs);
  }
}

const taskList = new List({
  items: tasks,
  x: 2,
  y: 5,
  width: 50,
  height: 12,
  selectedStyle: colors.bgGray + colors.white,
  renderItem: task => {
    const statusIcon =
      task.status === 'done'
        ? `${colors.green}✓${codes.reset}`
        : task.status === 'in_progress'
          ? `${colors.yellow}◐${codes.reset}`
          : `${colors.gray}○${codes.reset}`;
    const priorityColor = task.priority === 'high' ? colors.red : task.priority === 'medium' ? colors.yellow : colors.gray;
    return ` ${statusIcon} ${task.name} ${priorityColor}[${task.priority}]${codes.reset}`;
  }
});

const logList = new List({
  items: logs,
  x: 2,
  y: 5,
  width: screen.width - 4,
  height: 15,
  selectedStyle: colors.bgGray + colors.white,
  renderItem: log => {
    const levelColor = log.level === 'ERROR' ? colors.red : log.level === 'WARN' ? colors.yellow : log.level === 'DEBUG' ? colors.cyan : colors.green;
    return `${colors.dim}${log.time}${codes.reset} ${levelColor}${pad(log.level, 5)}${codes.reset} ${log.message}`;
  }
});

const cpuBar = new ProgressBar({ width: 25, filledStyle: colors.green, showPercent: true });
const memBar = new ProgressBar({ width: 25, filledStyle: colors.blue, showPercent: true });
const diskBar = new ProgressBar({ width: 25, filledStyle: colors.yellow, showPercent: true });

const searchInput = new Input({ width: 30, placeholder: 'Search tasks...' });

function filterTasks() {
  let filtered = tasks;
  if (state.taskFilter !== 'all') {
    filtered = filtered.filter(t => t.status === state.taskFilter);
  }
  if (state.searchQuery) {
    const q = state.searchQuery.toLowerCase();
    filtered = filtered.filter(t => t.name.toLowerCase().includes(q));
  }
  taskList.setItems(filtered);
}

function drawHeader() {
  const tabs = [
    { key: '1', name: 'Dashboard', view: 'dashboard' },
    { key: '2', name: 'Tasks', view: 'tasks' },
    { key: '3', name: 'Logs', view: 'logs' },
    { key: '4', name: 'Settings', view: 'settings' }
  ];

  let tabLine = ' ';
  for (const tab of tabs) {
    const isActive = state.view === tab.view;
    const style = isActive ? colors.bgWhite + colors.black + colors.bold : colors.dim;
    tabLine += `${style} ${tab.key}:${tab.name} ${codes.reset} `;
  }
  screen.write(0, 0, tabLine);
}

function drawFooter() {
  const w = screen.width;
  const h = screen.height;

  const help =
    state.view === 'tasks' ? ' ↑↓:Navigate  Enter:Toggle  a:All  t:Todo  p:Progress  d:Done  /:Search  q:Quit ' : ' 1-4:Switch tabs  ?:Help  q:Quit ';

  screen.write(0, h - 1, colors.bgGray + colors.white + pad(help, w) + codes.reset);
}

function drawDashboard() {
  const w = screen.width;
  const sbw = 42;
  const tbw = 44;
  const rbw = 55;
  const rcw = rbw - 4;
  const bs = box[getSetting('boxStyle')];

  screen.box(2, 3, sbw, 5, bs, 'System Status', colors.bold + colors.cyan, colors.cyan);

  cpuBar.setValue(state.cpuUsage);
  memBar.setValue(state.memUsage);
  diskBar.setValue(state.diskUsage);

  screen.write(4, 4, `CPU:    ${cpuBar.render()}`);
  screen.write(4, 5, `Memory: ${memBar.render()}`);
  screen.write(4, 6, `Disk:   ${diskBar.render()}`);

  screen.box(2, 9, tbw, 6, bs, 'Task Summary', colors.bold + colors.yellow, colors.yellow);

  const done = tasks.filter(t => t.status === 'done').length;
  const inProgress = tasks.filter(t => t.status === 'in_progress').length;
  const todo = tasks.filter(t => t.status === 'todo').length;

  screen.write(4, 10, `${colors.green}✓ Completed:${codes.reset}   ${done}`);
  screen.write(4, 11, `${colors.yellow}◐ In Progress:${codes.reset} ${inProgress}`);
  screen.write(4, 12, `${colors.gray}○ Todo:${codes.reset}        ${todo}`);

  const progress = new ProgressBar({
    value: done,
    max: tasks.length,
    width: 25,
    filledStyle: colors.green,
    showPercent: true
  });

  screen.write(4, 13, `Progress: ${progress.render()}`);

  screen.box(2, 16, rbw, 7, bs, 'Recent Activity', colors.bold + colors.magenta, colors.magenta);

  const recentLogs = logs.slice(-5).reverse();
  for (let i = 0; i < recentLogs.length; i++) {
    const log = recentLogs[i];
    const levelColor = log.level === 'ERROR' ? colors.red : log.level === 'WARN' ? colors.yellow : log.level === 'DEBUG' ? colors.cyan : colors.green;
    screen.write(4, 17 + i, `${colors.dim}${log.time}${codes.reset} ${levelColor}${pad(log.level, 5)}${codes.reset} ${truncate(log.message, rcw - 16)}`);
  }

  const qw = 30;
  const qx = w - qw - 2;
  if (qx > rbw + 4) {
    screen.box(qx, 3, qw, 10, bs, 'Quick Stats', colors.bold + colors.green, colors.green);
    screen.write(qx + 2, 5, `${colors.bold}Uptime:${codes.reset} 2h 34m`);
    screen.write(qx + 2, 6, `${colors.bold}Network In:${codes.reset} ${state.networkIn} KB/s`);
    screen.write(qx + 2, 7, `${colors.bold}Network Out:${codes.reset} ${state.networkOut} KB/s`);
    screen.write(qx + 2, 8, `${colors.bold}Active Users:${codes.reset} 42`);
    screen.write(qx + 2, 9, `${colors.bold}Requests/s:${codes.reset} 1,234`);
    screen.write(qx + 2, 10, `${colors.bold}Errors:${codes.reset} ${colors.red}3${codes.reset}`);
  }
}

function drawTasks() {
  const filterLabel =
    state.taskFilter === 'all' ? 'All' : state.taskFilter === 'todo' ? 'Todo' : state.taskFilter === 'in_progress' ? 'In Progress' : 'Done';

  screen.write(2, 3, colors.bold + `Tasks [${filterLabel}] - ${taskList.items.length} items` + codes.reset);

  if (state.searchMode) {
    screen.write(2, 4, colors.cyan + 'Search: ' + codes.reset + searchInput.render());
  }

  taskList.y = state.searchMode ? 6 : 5;
  taskList.height = state.searchMode ? screen.height - 9 : screen.height - 8;
  taskList.width = Math.min(60, screen.width - 4);
  taskList.render(screen);

  const selected = taskList.getSelected();
  if (selected && screen.width > 65) {
    const detailX = taskList.width + 5;
    screen.box(detailX, 5, 35, 12, box.rounded, 'Details', colors.bold + colors.cyan);

    screen.write(detailX + 2, 7, `${colors.bold}ID:${codes.reset} ${selected.id}`);
    screen.write(detailX + 2, 8, `${colors.bold}Name:${codes.reset} ${truncate(selected.name, 25)}`);
    screen.write(detailX + 2, 9, `${colors.bold}Status:${codes.reset} ${selected.status}`);
    screen.write(detailX + 2, 10, `${colors.bold}Priority:${codes.reset} ${selected.priority}`);

    screen.write(detailX + 2, 12, colors.dim + 'Enter to toggle status' + codes.reset);
    screen.write(detailX + 2, 13, colors.dim + 'P to cycle priority' + codes.reset);
    screen.write(detailX + 2, 14, colors.dim + 'D to delete task' + codes.reset);
    screen.write(detailX + 2, 15, colors.dim + 'N to add new task' + codes.reset);
  }
}

function drawLogs() {
  screen.write(2, 3, colors.bold + `System Logs - ${logs.length} entries` + codes.reset);

  logList.width = screen.width - 4;
  logList.height = screen.height - 8;
  logList.render(screen);
}

function drawSettings() {
  screen.write(2, 3, colors.bold + colors.cyan + 'Settings' + codes.reset);

  for (let i = 0; i < settings.length; i++) {
    const s = settings[i];
    const y = 5 + i;
    const isSelected = state.settingsIndex === i;
    const style = isSelected ? colors.bgGray + colors.white : '';
    const label = pad(s.label, 20);
    const value = s.format(s.value);
    const valueStyle = isSelected ? colors.cyan + colors.bold : colors.yellow;
    const line = ` ${label} ${valueStyle}${value}${codes.reset} `;
    const visible = visibleLength(line);
    const padding = Math.max(0, 50 - visible);

    if (isSelected) {
      screen.write(2, y, style + line + ' '.repeat(padding) + codes.reset);
    } else {
      screen.write(2, y, line);
    }
  }

  screen.write(2, 5 + settings.length + 1, colors.dim + '↑↓: Navigate  Enter/→: Change  ←: Change back' + codes.reset);
}

let _renderPending = false;

function render() {
  if (_renderPending) return;
  _renderPending = true;
  queueMicrotask(() => {
    _renderPending = false;
    screen.clear();
    drawHeader();

    switch (state.view) {
      case 'dashboard':
        drawDashboard();
        break;
      case 'tasks':
        drawTasks();
        break;
      case 'logs':
        drawLogs();
        break;
      case 'settings':
        drawSettings();
        break;
    }

    drawFooter();
    screen.render();
  });
}

function showHelp() {
  modal(screen, {
    id: 'help',
    width: 50,
    height: 18,
    title: 'Keyboard Shortcuts',
    titleStyle: colors.bold + colors.cyan,
    borderStyle: box.double,
    onKey: key => {
      if (key === keys.ESCAPE || key === keys.ENTER || key === '?') {
        screen.popModal('help');
        render();
      }
      return false;
    },
    render: (buf, w, _h, ox, oy) => {
      const shortcuts = [
        ['1-4', 'Switch between tabs'],
        ['↑/↓ or j/k', 'Navigate lists'],
        ['Enter', 'Select/Toggle item'],
        ['/', 'Search (in Tasks)'],
        ['Escape', 'Cancel/Close'],
        ['a', 'Show all tasks'],
        ['t', 'Filter: Todo only'],
        ['p', 'Filter: In Progress'],
        ['d', 'Filter: Done'],
        ['m', 'Show memory stats'],
        ['?', 'Show this help'],
        ['q', 'Quit application']
      ];

      for (let i = 0; i < shortcuts.length; i++) {
        const [key, desc] = shortcuts[i];
        buf.writeStyled(ox, oy + i + 1, `  ${colors.cyan}${pad(key, 12)}${codes.reset} ${desc}`);
      }

      buf.writeStyled(ox, oy + shortcuts.length + 2, colors.dim + padCenter('Press Escape to close', w) + codes.reset);
    }
  });
  screen.render();
}

function showMemoryModal() {
  const fmt = bytes => {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
    return (bytes / 1024 / 1024).toFixed(2) + ' MB';
  };

  modal(screen, {
    id: 'memory',
    width: 40,
    height: 24,
    title: 'Memory Usage',
    titleStyle: colors.bold + colors.yellow,
    borderStyle: box.rounded,
    onKey: key => {
      if (key === 'm' || key === keys.ESCAPE) {
        screen.popModal('memory');
        render();
      } else if (key === 'g') {
        Ant.gc();
        screen.popModal('memory');
        showMemoryModal();
      }
      return false;
    },
    render: (buf, _w, _h, ox, oy) => {
      const mem = Ant.stats();
      let y = 1;

      buf.writeStyled(ox, oy + y++, `${colors.cyan}Arena${codes.reset}`);
      buf.writeStyled(ox, oy + y++, `  Used:        ${colors.bold}${fmt(mem.arenaUsed)}${codes.reset}`);
      buf.writeStyled(ox, oy + y++, `  Size:        ${colors.bold}${fmt(mem.arenaSize)}${codes.reset}`);
      y++;

      if (mem.external) {
        buf.writeStyled(ox, oy + y++, `${colors.cyan}External${codes.reset}`);
        buf.writeStyled(ox, oy + y++, `  Buffers:     ${colors.bold}${fmt(mem.external.buffers)}${codes.reset}`);
        buf.writeStyled(ox, oy + y++, `  Code:        ${colors.bold}${fmt(mem.external.code)}${codes.reset}`);
        buf.writeStyled(ox, oy + y++, `  Collections: ${colors.bold}${fmt(mem.external.collections)}${codes.reset}`);
        buf.writeStyled(ox, oy + y++, `  Total:       ${colors.bold}${fmt(mem.external.total)}${codes.reset}`);
        y++;
      }

      if (mem.intern) {
        buf.writeStyled(ox, oy + y++, `${colors.cyan}Intern Table${codes.reset}`);
        buf.writeStyled(ox, oy + y++, `  Strings:     ${colors.bold}${mem.intern.count}${codes.reset}`);
        buf.writeStyled(ox, oy + y++, `  Bytes:       ${colors.bold}${fmt(mem.intern.bytes)}${codes.reset}`);
        y++;
      }

      buf.writeStyled(ox, oy + y++, `${colors.cyan}Process${codes.reset}`);
      buf.writeStyled(ox, oy + y++, `  RSS:         ${colors.bold}${fmt(mem.rss)}${codes.reset}`);
      if (mem.virtualSize) {
        buf.writeStyled(ox, oy + y++, `  Virtual:     ${colors.bold}${fmt(mem.virtualSize)}${codes.reset}`);
      }
      y++;

      buf.writeStyled(ox, oy + y++, `${colors.cyan}C Stack${codes.reset}`);
      buf.writeStyled(ox, oy + y++, `  Max:         ${colors.bold}${fmt(mem.cstack)}${codes.reset}`);
      y++;

      buf.writeStyled(ox, oy + y, colors.dim + '  g: Run GC  m/Esc: Close' + codes.reset);
    }
  });
  screen.render();
}

function handleKey(key) {
  if (screen.hasModal()) return;

  if (state.searchMode) {
    if (key === keys.ESCAPE) {
      state.searchMode = false;
      state.searchQuery = '';
      searchInput.clear();
      filterTasks();
    } else if (key === keys.ENTER) {
      state.searchMode = false;
      state.searchQuery = searchInput.value;
      filterTasks();
    } else {
      searchInput.handleKey(key);
      state.searchQuery = searchInput.value;
      filterTasks();
    }
    render();
    return;
  }

  switch (key) {
    case 'q':
    case keys.CTRL_C:
      if (getSetting('confirmQuit')) {
        confirm(screen, {
          title: 'Quit',
          message: 'Are you sure you want to quit?'
        }).then(confirmed => {
          if (confirmed) screen.exit(0);
          render();
        });
      } else {
        screen.exit(0);
      }
      return;

    case '1':
      state.view = 'dashboard';
      render();
      break;
    case '2':
      state.view = 'tasks';
      render();
      break;
    case '3':
      state.view = 'logs';
      render();
      break;
    case '4':
      state.view = 'settings';
      render();
      break;

    case '?':
      showHelp();
      break;
    case 'm':
      showMemoryModal();
      break;

    case keys.UP:
    case 'k':
      if (state.view === 'tasks') taskList.selectPrev();
      else if (state.view === 'logs') logList.selectPrev();
      else if (state.view === 'settings') state.settingsIndex = Math.max(0, state.settingsIndex - 1);
      render();
      break;

    case keys.DOWN:
    case 'j':
      if (state.view === 'tasks') taskList.selectNext();
      else if (state.view === 'logs') logList.selectNext();
      else if (state.view === 'settings') state.settingsIndex = Math.min(settings.length - 1, state.settingsIndex + 1);
      render();
      break;

    case keys.PAGE_UP:
      if (state.view === 'tasks') taskList.pageUp();
      else if (state.view === 'logs') logList.pageUp();
      render();
      break;

    case keys.PAGE_DOWN:
      if (state.view === 'tasks') taskList.pageDown();
      else if (state.view === 'logs') logList.pageDown();
      render();
      break;

    case keys.ENTER:
    case keys.RIGHT:
      if (state.view === 'tasks') {
        const task = taskList.getSelected();
        if (task) {
          task.status = task.status === 'done' ? 'todo' : task.status === 'todo' ? 'in_progress' : 'done';
          filterTasks();
        }
      } else if (state.view === 'settings') {
        cycleSetting(state.settingsIndex, 1);
      }
      render();
      break;

    case keys.LEFT:
      if (state.view === 'settings') {
        cycleSetting(state.settingsIndex, -1);
      }
      render();
      break;

    case '/':
      if (state.view === 'tasks') {
        state.searchMode = true;
        searchInput.clear();
      }
      render();
      break;

    case 'a':
      if (state.view === 'tasks') {
        state.taskFilter = 'all';
        filterTasks();
        render();
      }
      break;

    case 't':
      if (state.view === 'tasks') {
        state.taskFilter = 'todo';
        filterTasks();
        render();
      }
      break;

    case 'p':
      if (state.view === 'tasks') {
        state.taskFilter = 'in_progress';
        filterTasks();
        render();
      }
      break;

    case 'P':
      if (state.view === 'tasks') {
        const task = taskList.getSelected();
        if (task) {
          task.priority = task.priority === 'low' ? 'medium' : task.priority === 'medium' ? 'high' : 'low';
        }
      }
      render();
      break;

    case 'D':
      if (state.view === 'tasks') {
        const task = taskList.getSelected();
        if (task) {
          const idx = tasks.indexOf(task);
          if (idx >= 0) tasks.splice(idx, 1);
          filterTasks();
        }
      }
      render();
      break;

    case 'N':
      if (state.view === 'tasks') {
        const nameInput = new Input({ width: 30, placeholder: 'Task name...' });
        modal(screen, {
          id: 'new-task',
          width: 40,
          height: 7,
          title: 'New Task',
          titleStyle: colors.bold + colors.cyan,
          borderStyle: box.rounded,
          onKey: key => {
            if (key === keys.ENTER && nameInput.value) {
              const newTask = {
                id: tasks.length ? Math.max(...tasks.map(t => t.id)) + 1 : 1,
                name: nameInput.value,
                status: 'todo',
                priority: 'medium'
              };
              tasks.push(newTask);
              filterTasks();
              screen.popModal('new-task');
              render();
            } else if (key === keys.ESCAPE) {
              screen.popModal('new-task');
              render();
            } else {
              nameInput.handleKey(key);
              screen.render();
            }
            return false;
          },
          render: (buf, w, h, ox, oy) => {
            buf.writeStyled(ox, oy + 1, ' Name: ' + nameInput.render());
            buf.writeStyled(ox, oy + 3, colors.dim + ' Enter: Save  Esc: Cancel' + codes.reset);
          }
        });
        screen.render();
      }
      break;

    case 'd':
      if (state.view === 'tasks') {
        state.taskFilter = 'done';
        filterTasks();
        render();
      }
      break;
  }
}

screen.onKey(handleKey);

screen.onResize(() => {
  render();
});

const randomMessages = [
  'Request processed successfully',
  'User session expired',
  'Cache miss for key: user_prefs',
  'Rate limit exceeded for IP 192.168.1.42',
  'Garbage collection completed',
  'New connection from 10.0.0.15',
  'Query took 234ms to execute',
  'SSL certificate renewal scheduled',
  'Worker thread pool resized to 8',
  'Health check passed',
  'Disk usage above 80% threshold',
  'Backup completed successfully',
  'Failed to resolve hostname: api.example.com',
  'Retrying failed request (attempt 3/5)',
  'Memory pressure detected, evicting cache',
  'Configuration hot-reloaded',
  'Websocket connection dropped',
  'Slow query detected: SELECT * FROM events',
  'Plugin loaded: analytics-v2',
  'Cron job triggered: cleanup_temp_files'
];

const randomLevels = ['INFO', 'INFO', 'INFO', 'DEBUG', 'DEBUG', 'WARN', 'ERROR'];

function updateStats() {
  if (!getSetting('simulateStats')) return;
  state.cpuUsage = Math.max(5, Math.min(95, state.cpuUsage + (Math.random() - 0.5) * 10));
  state.memUsage = Math.max(20, Math.min(90, state.memUsage + (Math.random() - 0.5) * 5));
  state.networkIn = Math.floor(Math.random() * 500);
  state.networkOut = Math.floor(Math.random() * 200);

  if (state.view === 'dashboard' && !screen.hasModal()) {
    render();
  }
}

function addRandomLog() {
  const now = new Date();
  const time = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`;
  const level = randomLevels[Math.floor(Math.random() * randomLevels.length)];
  const message = randomMessages[Math.floor(Math.random() * randomMessages.length)];
  logs.push({ time, level, message });

  const max = getSetting('maxLogs');
  while (logs.length > max) logs.shift();

  const levels = ['DEBUG', 'INFO', 'WARN', 'ERROR'];
  const minIdx = levels.indexOf(getSetting('logLevel'));
  logList.setItems(logs.filter(l => levels.indexOf(l.level) >= minIdx));

  if (!screen.hasModal()) {
    render();
  }
}

let statsTimer = setInterval(updateStats, getSetting('refreshRate'));
let logTimer = setInterval(addRandomLog, getSetting('logInterval'));

screen.start();
render();
