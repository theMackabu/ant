import os from 'node:os';

export function sysinfo(): string {
  const mem = (os.totalmem() / 1024 / 1024 / 1024).toFixed(1);
  return [
    `runtime   ${Ant.version} (${Ant.target})`,
    `host      ${os.hostname()}`,
    `platform  ${os.platform()} ${os.arch()}`,
    `cpus      ${os.cpus().length}`,
    `memory    ${mem} GB`,
    `binary    ${process.execPath}`
  ].join('\n');
}
