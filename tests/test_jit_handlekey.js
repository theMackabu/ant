// Reproduce "undeclared reg 0" MIR error: switch statement with many
// string-constant cases, method calls, and property access — matching
// the exact bytecode pattern of a TUI handleKey function.

const keys = {
  UP: '\x1b[A',
  DOWN: '\x1b[B',
  RIGHT: '\x1b[C',
  LEFT: '\x1b[D',
  PAGE_UP: '\x1b[5~',
  PAGE_DOWN: '\x1b[6~',
  ENTER: '\r',
  ESCAPE: '\x1b',
  TAB: '\t',
  BACKSPACE: '\x7f',
  HOME: '\x1b[H',
  END: '\x1b[F',
  CTRL_C: '\x03',
};

const list = {
  index: 0,
  items: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
  selectNext() { this.index = Math.min(this.items.length - 1, this.index + 1); },
  selectPrev() { this.index = Math.max(0, this.index - 1); },
  pageDown() { this.index = Math.min(this.items.length - 1, this.index + 5); },
  pageUp() { this.index = Math.max(0, this.index - 5); },
  getSelected() { return this.items[this.index]; },
};

const logList = {
  index: 0,
  items: ['a', 'b', 'c'],
  selectNext() { this.index = Math.min(this.items.length - 1, this.index + 1); },
  selectPrev() { this.index = Math.max(0, this.index - 1); },
  pageDown() { this.index = Math.min(this.items.length - 1, this.index + 5); },
  pageUp() { this.index = Math.max(0, this.index - 5); },
};

const state = {
  view: 'tasks',
  searchMode: false,
  searchQuery: '',
  taskFilter: 'all',
  settingsIndex: 0,
};

let renderCount = 0;
function render() { renderCount++; }

// Use switch(key) — this generates DUP+CONST+SEQ+JMP_FALSE chains where
// the discriminant stays on the vstack across branch targets.
function handleKey(key) {
  if (state.searchMode) {
    if (key === keys.ESCAPE) {
      state.searchMode = false;
      state.searchQuery = '';
      render();
      return;
    }
    if (key === keys.ENTER) {
      state.searchMode = false;
      render();
      return;
    }
    return;
  }

  switch (key) {
    case 'q':
    case keys.CTRL_C:
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
      render();
      break;
    case 'm':
      render();
      break;

    case keys.UP:
    case 'k':
      if (state.view === 'tasks') list.selectPrev();
      else if (state.view === 'logs') logList.selectPrev();
      else if (state.view === 'settings') state.settingsIndex = Math.max(0, state.settingsIndex - 1);
      render();
      break;

    case keys.DOWN:
    case 'j':
      if (state.view === 'tasks') list.selectNext();
      else if (state.view === 'logs') logList.selectNext();
      else if (state.view === 'settings') state.settingsIndex = Math.min(9, state.settingsIndex + 1);
      render();
      break;

    case keys.PAGE_UP:
      if (state.view === 'tasks') list.pageUp();
      else if (state.view === 'logs') logList.pageUp();
      render();
      break;

    case keys.PAGE_DOWN:
      if (state.view === 'tasks') list.pageDown();
      else if (state.view === 'logs') logList.pageDown();
      render();
      break;

    case keys.ENTER:
    case keys.RIGHT:
      if (state.view === 'tasks') {
        const item = list.getSelected();
        if (item) {
          const desc = 'item_' + String(item);
          if (desc.length > 0) render();
        }
      }
      render();
      break;

    case keys.LEFT:
      render();
      break;

    case keys.HOME:
      list.index = 0;
      render();
      break;

    case keys.END:
      list.index = list.items.length - 1;
      render();
      break;

    case keys.TAB:
      state.view = 'dashboard';
      render();
      break;

    case '/':
      if (state.view === 'tasks') {
        state.searchMode = true;
        state.searchQuery = '';
      }
      render();
      break;

    case 'a':
      if (state.view === 'tasks') { state.taskFilter = 'all'; render(); }
      break;
    case 't':
      if (state.view === 'tasks') { state.taskFilter = 'todo'; render(); }
      break;
    case 'p':
      if (state.view === 'tasks') { state.taskFilter = 'in_progress'; render(); }
      break;
    case 'd':
      if (state.view === 'tasks') { state.taskFilter = 'done'; render(); }
      break;
    case 'P':
      render();
      break;
    case 'D':
      render();
      break;
    case 'N':
      render();
      break;
  }
}

const scrollKeys = [
  keys.UP, keys.DOWN, keys.UP, keys.DOWN,
  keys.PAGE_UP, keys.PAGE_DOWN,
  keys.UP, keys.DOWN, 'k', 'j',
  keys.ENTER, keys.LEFT, keys.RIGHT,
  keys.HOME, keys.END, keys.TAB,
];

for (let i = 0; i < 500; i++) {
  handleKey(scrollKeys[i % scrollKeys.length]);
}

console.log('OK: rendered', renderCount, 'times, list index:', list.index);
