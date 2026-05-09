import { SlashCommandBuilder } from 'discord.js';

export default {
  builder: new SlashCommandBuilder().setName('version').setDescription('show ant version'),
  run: async i => i.reply(`ant ${Ant.version}`)
};
