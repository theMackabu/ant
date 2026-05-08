import { Client, GatewayIntentBits, Routes, SlashCommandBuilder } from 'discord.js';

const DISCORD_TOKEN = process.env.DISCORD_TOKEN;
const CLIENT_ID = process.env.DISCORD_CLIENT_ID;

const commands = [
  new SlashCommandBuilder().setName('ping').setDescription('Replies with pong!'),
  new SlashCommandBuilder().setName('hello').setDescription('Says hello')
].map(c => c.toJSON());

const client = new Client({ intents: [GatewayIntentBits.Guilds] });

client.once('ready', async () => {
  await client.rest.put(Routes.applicationCommands(CLIENT_ID), { body: commands });
  console.log(`Logged in as ${client.user.tag}`);
});

client.on('interactionCreate', async interaction => {
  if (!interaction.isChatInputCommand()) return;
  if (interaction.commandName === 'ping') await interaction.reply('pong!');
  else if (interaction.commandName === 'hello') await interaction.reply(`hi ${interaction.user.username}!`);
});

client.login(DISCORD_TOKEN);
