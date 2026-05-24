import { SlashCommandBuilder, ApplicationIntegrationType, InteractionContextType } from 'discord.js';

export default {
  builder: new SlashCommandBuilder()
    .setName('ping')
    .setDescription('show gateway latency')
    .setIntegrationTypes(ApplicationIntegrationType.GuildInstall, ApplicationIntegrationType.UserInstall)
    .setContexts(InteractionContextType.Guild, InteractionContextType.BotDM, InteractionContextType.PrivateChannel),
  run: async i => {
    const sent = await i.reply({ content: 'pinging…', withResponse: true });
    const rtt = sent.resource.message.createdTimestamp - i.createdTimestamp;
    await i.editReply(`pong — rtt ${rtt}ms · ws ${Math.round(i.client.ws.ping)}ms`);
  }
};
