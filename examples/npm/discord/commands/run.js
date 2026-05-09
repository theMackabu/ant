import { SlashCommandBuilder, ModalBuilder, TextInputBuilder, TextInputStyle, ActionRowBuilder } from 'discord.js';

const clip = (s, n) => (s.length > n ? s.slice(0, n) + '\n…' : s);

export default {
  builder: new SlashCommandBuilder().setName('run').setDescription('evaluate javascript via modal (owner only)'),
  ownerOnly: true,
  run: async i => {
    const modal = new ModalBuilder()
      .setCustomId('run-modal')
      .setTitle('run')
      .addComponents(
        new ActionRowBuilder().addComponents(
          new TextInputBuilder()
            .setCustomId('code')
            .setLabel('code')
            .setStyle(TextInputStyle.Paragraph)
            .setRequired(true),
        ),
      );

    await i.showModal(modal);

    const submitted = await i.awaitModalSubmit({
      time: 300_000,
      filter: x => x.customId === 'run-modal' && x.user.id === i.user.id,
    }).catch(() => null);
    if (!submitted) return;

    const code = submitted.fields.getTextInputValue('code');

    let buf = '';
    const sink = { write(s) { buf += s; return true; } };
    const captured = new console.Console({ stdout: sink, stderr: sink });

    try {
      await eval(`(async (console) => { ${code} })`)(captured);
    } catch (err) {
      captured.error(err?.stack ?? String(err));
    }

    const content = buf ? '```ansi\n' + clip(buf, 1900) + '\n```' : '_(no output)_';
    await submitted.reply({ content });
  }
};
