const WIDTH = 150;
const HEIGHT = 40;

const cells = new Map();
const directions = [
  [-1, 1],
  [0, 1],
  [1, 1],
  [-1, 0],
  [1, 0],
  [-1, -1],
  [0, -1],
  [1, -1]
];

const key_for = (x, y) => `${x}-${y}`;

for (let y = 0; y < HEIGHT; y++) {
  for (let x = 0; x < WIDTH; x++) {
    cells.set(key_for(x, y), {
      x,
      y,
      alive: Math.random() <= 0.2,
      next_state: false,
      neighbours: []
    });
  }
}

for (const cell of cells.values()) {
  for (const [dx, dy] of directions) {
    const nx = cell.x + dx;
    const ny = cell.y + dy;
    if (nx < 0 || ny < 0 || nx >= WIDTH || ny >= HEIGHT) continue;
    const neighbour = cells.get(key_for(nx, ny));
    if (neighbour) cell.neighbours.push(neighbour);
  }
}

const dotick = () => {
  for (const cell of cells.values()) {
    let alive_neighbours = 0;
    for (const neighbour of cell.neighbours) {
      if (neighbour.alive) alive_neighbours++;
    }

    if (!cell.alive && alive_neighbours === 3) {
      cell.next_state = true;
    } else if (alive_neighbours < 2 || alive_neighbours > 3) {
      cell.next_state = false;
    } else {
      cell.next_state = cell.alive;
    }
  }

  for (const cell of cells.values()) {
    cell.alive = cell.next_state;
  }
};

const render = () => {
  let rendering = '';
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const cell = cells.get(key_for(x, y));
      rendering += cell && cell.alive ? 'o' : ' ';
    }
    rendering += '\n';
  }
  return rendering;
};

let ticks = 0;
const start = performance.now();
const duration = 30000;
const batch_size = 100;

const run = () => {
  for (let i = 0; i < batch_size && performance.now() - start < duration; i++) {
    dotick();
    render();
  }

  ticks += batch_size;
  if (performance.now() - start >= duration) {
    console.log('done', ticks);
    return;
  }

  setTimeout(run, 0);
};

setTimeout(run, 0);
