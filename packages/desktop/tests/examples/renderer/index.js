import { randomUUID } from 'node:crypto';

const version = document.querySelector('#version');
const details = document.querySelector('#details');

function applyTheme(theme) {
  for (const [name, value] of Object.entries(theme)) {
    document.documentElement.style.setProperty(`--${name}`, value);
  }
}

async function toggleTheme() {
  applyTheme(await Ant.ipc.invoke('app:toggle-theme'));
}

document.querySelector('#toggle-theme').addEventListener('click', toggleTheme);

const info = await desktop.runtimeInfo();
version.textContent = `Ant ${Ant.versions.ant} | Desktop ${Ant.versions.desktop}`;
details.textContent = `Chrome ${Ant.versions.chrome} | ${desktop.platform} | ${randomUUID().slice(0, 8)}`;

if (!preloadReady || !info.rendererIsWindow) {
  throw new Error('preload and renderer integration failed');
}

Ant.ipc.send('page:ready', { title: document.title });
