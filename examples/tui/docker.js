import { Screen, List, Input, colors, box, keys, codes, confirm, alert, pad, padCenter, truncate } from './tuey.js';
import { $ } from 'ant:shell';

const screen = new Screen({ fullscreen: true, hideCursor: true });

let containers = [];

const state = {
  searchMode: false,
  searchQuery: '',
  lastError: ''
};

const searchInput = new Input({ width: 30, placeholder: 'Search containers...' });

const containerList = new List({
  items: [],
  x: 2,
  y: 6,
  width: 60,
  height: 10,
  selectedStyle: colors.bgBlue + colors.bold + colors.white,
  renderItem: container => formatContainer(container)
});

function runDocker(command) {
  const result = $(command);
  if (result.exitCode !== 0) {
    const output = result.text().trim();
    state.lastError = output || `Command failed: ${command}`;
    return null;
  }
  return result.text();
}

function isRunning(container) {
  return container.status && container.status.startsWith('Up');
}

function formatStatus(container) {
  if (!container.status) {
    return colors.dim + 'UNKNOWN' + codes.reset;
  }
  if (isRunning(container)) {
    return colors.green + 'UP' + codes.reset;
  }
  if (container.status.startsWith('Exited')) {
    return colors.red + 'EXIT' + codes.reset;
  }
  return colors.yellow + truncate(container.status.split(' ')[0].toUpperCase(), 6) + codes.reset;
}

function formatContainer(container) {
  const width = containerList.width;
  const nameWidth = 20;
  const imageWidth = 20;
  const statusWidth = 8;
  const portsWidth = Math.max(10, width - nameWidth - imageWidth - statusWidth - 6);

  const name = pad(truncate(container.name || '', nameWidth), nameWidth);
  const image = pad(truncate(container.image || '', imageWidth), imageWidth);
  const ports = pad(truncate(container.ports || '', portsWidth), portsWidth);

  return `${name} ${image} ${formatStatus(container)} ${ports}`;
}

function parseContainers(output) {
  const lines = output.trim() ? output.trim().split('\n') : [];
  return lines.map(line => {
    const [id, name, image, status, ports] = line.split('\t');
    return {
      id: id || '',
      name: name || '',
      image: image || '',
      status: status || '',
      ports: ports || ''
    };
  });
}

function applyFilter() {
  let filtered = containers;
  if (state.searchQuery) {
    const q = state.searchQuery.toLowerCase();
    filtered = containers.filter(container => {
      return container.name.toLowerCase().includes(q) || container.image.toLowerCase().includes(q) || container.id.toLowerCase().includes(q);
    });
  }
  containerList.setItems(filtered);
}

function refreshContainers() {
  state.lastError = '';
  const output = runDocker("docker ps -a --format '{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.Status}}\t{{.Ports}}'");
  if (output === null) {
    containers = [];
    containerList.setItems([]);
    return;
  }
  containers = parseContainers(output);
  applyFilter();
}

function drawHeader() {
  const title = ' Docker TUI - Containers ';
  const w = screen.width;
  screen.write(0, 0, colors.bgBlue + colors.bold + colors.white + padCenter(title, w) + codes.reset);

  const statusLine = state.lastError
    ? colors.bgRed + colors.white + pad(` Error: ${truncate(state.lastError, w - 8)} `, w) + codes.reset
    : colors.bgGray + colors.white + pad(' Connected to docker CLI ', w) + codes.reset;
  screen.write(0, 1, statusLine);
}

function drawFooter() {
  const w = screen.width;
  const help = ' Up/Down:Navigate  Enter:Start/Stop  s:Start  t:Stop  R:Restart  r:Refresh  /:Search  q:Quit ';
  screen.write(0, screen.height - 1, colors.bgGray + colors.white + pad(help, w) + codes.reset);
}

function drawList() {
  const listWidth = screen.width > 95 ? 62 : Math.max(40, screen.width - 4);
  containerList.width = listWidth;
  containerList.height = Math.max(5, screen.height - 8);
  containerList.x = 2;
  containerList.y = state.searchMode ? 7 : 6;

  const heading = pad('Name', 20) + ' ' + pad('Image', 20) + ' ' + pad('State', 8) + ' ' + pad('Ports', listWidth - 20 - 20 - 8 - 3);
  screen.write(2, 5, colors.bold + heading + codes.reset);

  if (state.searchMode) {
    screen.write(2, 3, colors.cyan + 'Search: ' + codes.reset + searchInput.render());
  } else {
    screen.write(2, 3, colors.bold + `Containers - ${containerList.items.length} total` + codes.reset);
  }

  containerList.render(screen);

  if (screen.width > 95) {
    drawDetailsPanel(listWidth + 4);
  }
}

function drawDetailsPanel(x) {
  const selected = containerList.getSelected();
  const width = screen.width - x - 2;
  if (!selected || width < 20) return;

  const panelHeight = 10;
  screen.box(x, 5, width, panelHeight, box.rounded, 'Details', colors.bold + colors.cyan);
  screen.write(x + 2, 7, `${colors.bold}Name:${codes.reset} ${truncate(selected.name, width - 12)}`);
  screen.write(x + 2, 8, `${colors.bold}ID:${codes.reset} ${truncate(selected.id, width - 10)}`);
  screen.write(x + 2, 9, `${colors.bold}Image:${codes.reset} ${truncate(selected.image, width - 12)}`);
  screen.write(x + 2, 10, `${colors.bold}Status:${codes.reset} ${truncate(selected.status, width - 13)}`);
  screen.write(x + 2, 11, `${colors.bold}Ports:${codes.reset} ${truncate(selected.ports || '-', width - 12)}`);
}

function render() {
  screen.clear();
  drawHeader();
  drawList();
  drawFooter();
  screen.render();
}

function runAction(action, container) {
  const verb = action.charAt(0).toUpperCase() + action.slice(1);
  confirm(screen, {
    title: `${verb} Container`,
    message: `Run: docker ${action} ${container.name}?`
  }).then(confirmed => {
    if (!confirmed) {
      render();
      return;
    }

    const output = runDocker(`docker ${action} ${container.id}`);
    if (output === null) {
      alert(screen, {
        title: 'Docker Error',
        message: state.lastError || 'Docker command failed.'
      }).then(() => {
        refreshContainers();
        render();
      });
      return;
    }

    refreshContainers();
    render();
  });
}

function toggleStartStop(container) {
  if (isRunning(container)) {
    runAction('stop', container);
  } else {
    runAction('start', container);
  }
}

function handleKey(key) {
  if (screen.hasModal()) return;

  if (state.searchMode) {
    if (key === keys.ESCAPE) {
      state.searchMode = false;
      state.searchQuery = '';
      searchInput.clear();
      applyFilter();
    } else if (key === keys.ENTER) {
      state.searchMode = false;
      state.searchQuery = searchInput.value;
      applyFilter();
    } else {
      searchInput.handleKey(key);
      state.searchQuery = searchInput.value;
      applyFilter();
    }
    render();
    return;
  }

  switch (key) {
    case 'q':
    case keys.CTRL_C:
      confirm(screen, {
        title: 'Quit',
        message: 'Exit Docker TUI?'
      }).then(confirmed => {
        if (confirmed) {
          screen.exit(0);
        }
        render();
      });
      return;
    case '/':
      state.searchMode = true;
      searchInput.clear();
      render();
      return;
    case 'r':
      refreshContainers();
      render();
      return;
    case keys.ENTER: {
      const selected = containerList.getSelected();
      if (selected) {
        toggleStartStop(selected);
      }
      return;
    }
    case 's': {
      const selected = containerList.getSelected();
      if (selected) {
        runAction('start', selected);
      }
      return;
    }
    case 't': {
      const selected = containerList.getSelected();
      if (selected) {
        runAction('stop', selected);
      }
      return;
    }
    case 'R': {
      const selected = containerList.getSelected();
      if (selected) {
        runAction('restart', selected);
      }
      return;
    }
  }

  if (containerList.handleKey(key)) {
    render();
  }
}

screen.onKey(handleKey);
screen.onResize(() => render());

screen.start();
refreshContainers();
render();

setInterval(() => {
  if (!screen.hasModal()) {
    refreshContainers();
    render();
  }
}, 4000);
