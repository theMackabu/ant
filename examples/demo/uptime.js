#!/usr/bin/env ant

import { $ } from 'ant:shell';
import { uptime as osUptime, loadavg } from 'ant:os';

const uptime = osUptime();
const users = $`who | wc -l`.text().trim();

const days = Math.floor(uptime / 86400);
const hours = Math.floor((uptime % 86400) / 3600);
const minutes = String(Math.floor((uptime % 3600) / 60)).padStart(2, '0');

const load = loadavg()
  .map(n => n.toFixed(2))
  .join(' ');

console.log(`up ${days} days, ${hours}:${minutes}, ${users} user${users == 1 ? '' : 's'}, load averages: ${load}`);
