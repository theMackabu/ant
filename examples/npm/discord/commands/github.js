import { SlashCommandBuilder } from 'discord.js';

export default {
  builder: new SlashCommandBuilder().setName('github').setDescription('link to ant github'),
  run: async i => i.reply('https://github.com/themackabu/ant')
};
