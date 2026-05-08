import { Client, GatewayIntentBits, Routes, SlashCommandBuilder } from 'discord.js';

const CLIENT_ID = process.env.DISCORD_CLIENT_ID;
const GUILD_ID = process.env.DISCORD_GUILD_ID;
const DISCORD_TOKEN = process.env.DISCORD_TOKEN;

const commands = [
  new SlashCommandBuilder().setName('version').setDescription('show ant version'),
  new SlashCommandBuilder().setName('github').setDescription('link to ant github')
].map(c => c.toJSON());

const client = new Client({ intents: [GatewayIntentBits.Guilds] });

client.once('clientReady', async () => {
  await client.rest.put(Routes.applicationGuildCommands(CLIENT_ID, GUILD_ID), { body: commands });
  console.log(`logged in as ${client.user.tag}`);
});

client.on('interactionCreate', async interaction => {
  if (!interaction.isChatInputCommand()) return;
  if (interaction.commandName === 'version') await interaction.reply(`ant ${Ant.version}`);
  else if (interaction.commandName === 'github') await interaction.reply(`https://github.com/themackabu/ant`);
});

client.login(DISCORD_TOKEN);
