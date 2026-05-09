import { Client, GatewayIntentBits, Routes, MessageFlags } from 'discord.js';

import version from './commands/version.js';
import github from './commands/github.js';
import evalCmd from './commands/eval.js';
import runCmd from './commands/run.js';
import uptime from './commands/uptime.js';
import ping from './commands/ping.js';

const CLIENT_ID = process.env.DISCORD_CLIENT_ID;
const GUILD_ID = process.env.DISCORD_GUILD_ID;
const DISCORD_TOKEN = process.env.DISCORD_TOKEN;

const handlers = Object.fromEntries([version, github, evalCmd, runCmd, uptime, ping].map(h => [h.builder.name, h]));
const commands = Object.values(handlers).map(h => h.builder.toJSON());
const client = new Client({ intents: [GatewayIntentBits.Guilds] });

let ownerId = null;

client.once('clientReady', async () => {
  await client.rest.put(Routes.applicationGuildCommands(CLIENT_ID, GUILD_ID), { body: commands });
  const app = await client.application.fetch();
  ownerId = app.owner?.ownerId ?? app.owner?.id ?? app.owner?.members?.first()?.id ?? null;
  console.log(`logged in as ${client.user.tag}`);
});

client.on('interactionCreate', async interaction => {
  if (!interaction.isChatInputCommand()) return;
  const handler = handlers[interaction.commandName];
  if (!handler) return;
  if (handler.ownerOnly && interaction.user.id !== ownerId) {
    await interaction.reply({ content: 'this command is restricted to the application owner.', flags: MessageFlags.Ephemeral });
    return;
  }
  await handler.run(interaction);
});

client.login(DISCORD_TOKEN);
