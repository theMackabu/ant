import { SlashCommandBuilder, ApplicationIntegrationType, InteractionContextType } from 'discord.js';

export default {
  builder: new SlashCommandBuilder()
    .setName('github')
    .setDescription('link to ant github')
    .setIntegrationTypes(ApplicationIntegrationType.GuildInstall, ApplicationIntegrationType.UserInstall)
    .setContexts(InteractionContextType.Guild, InteractionContextType.BotDM, InteractionContextType.PrivateChannel),
  run: async i => i.reply('https://github.com/themackabu/ant')
};
