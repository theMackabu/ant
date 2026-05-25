import { SlashCommandBuilder, MessageFlags, ApplicationIntegrationType, InteractionContextType } from 'discord.js';
import { generateText } from 'ai';
import { openai } from '@ai-sdk/openai';

const clip = (s, n) => (s.length > n ? s.slice(0, n) + '\n…' : s);

export default {
  builder: new SlashCommandBuilder()
    .setName('ai')
    .setDescription('ask gpt-5.5 a question')
    .addStringOption(o => o.setName('prompt').setDescription('your prompt').setRequired(true))
    .setIntegrationTypes(ApplicationIntegrationType.GuildInstall, ApplicationIntegrationType.UserInstall)
    .setContexts(InteractionContextType.Guild, InteractionContextType.BotDM, InteractionContextType.PrivateChannel),
  ownerOnly: true,
  run: async i => {
    const prompt = i.options.getString('prompt', true);
    console.log(`[ai] ${i.user.tag} (${i.user.id})`);
    await i.deferReply({ flags: MessageFlags.Ephemeral });
    try {
      const { text } = await generateText({
        model: openai('gpt-5.5'),
        prompt
      });
      await i.editReply(clip(text || '_(no output)_', 1900));
    } catch (err) {
      await i.editReply({ content: `error: ${clip(err?.message ?? String(err), 1800)}` });
    }
  }
};
