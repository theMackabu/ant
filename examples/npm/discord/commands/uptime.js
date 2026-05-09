import { SlashCommandBuilder } from 'discord.js';

const fmt = s => {
  const d = Math.floor(s / 86400);
  const h = Math.floor(s % 86400 / 3600);
  const m = Math.floor(s % 3600 / 60);
  const sec = Math.floor(s % 60);
  return [d && `${d}d`, h && `${h}h`, m && `${m}m`, `${sec}s`].filter(Boolean).join(' ');
};

export default {
  builder: new SlashCommandBuilder().setName('uptime').setDescription('show bot uptime'),
  run: async i => i.reply(`up for ${fmt(process.uptime())}`)
};
