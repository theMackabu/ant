import { SlashCommandBuilder, ApplicationIntegrationType, InteractionContextType } from 'discord.js';

const clip = (s, n) => (s.length > n ? s.slice(0, n) + '\n…' : s);

export default {
  builder: new SlashCommandBuilder()
    .setName('eval')
    .setDescription('evaluate javascript (owner only)')
    .addStringOption(o => o.setName('code').setDescription('code to evaluate').setRequired(true))
    .setIntegrationTypes(ApplicationIntegrationType.GuildInstall, ApplicationIntegrationType.UserInstall)
    .setContexts(InteractionContextType.Guild, InteractionContextType.BotDM, InteractionContextType.PrivateChannel),
  ownerOnly: true,
  run: async i => {
    const code = i.options.getString('code', true);
    console.log(`[eval] ${i.user.tag} (${i.user.id}): ${code}`);

    let buf = '';
    const sink = { write(s) { buf += s; return true; } };
    const captured = new console.Console({ stdout: sink, stderr: sink });

    try {
      await eval(`(async (console) => { ${code} })`)(captured);
    } catch (err) {
      captured.error(err?.stack ?? String(err));
    }

    const content = buf ? '```ansi\n' + clip(buf, 1900) + '\n```' : '_(no output)_';
    await i.reply({ content });
  }
};
