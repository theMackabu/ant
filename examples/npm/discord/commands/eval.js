import { SlashCommandBuilder } from 'discord.js';

const clip = (s, n) => (s.length > n ? s.slice(0, n) + '\n…' : s);

export default {
  builder: new SlashCommandBuilder()
    .setName('eval')
    .setDescription('evaluate javascript (owner only)')
    .addStringOption(o => o.setName('code').setDescription('code to evaluate').setRequired(true)),
  ownerOnly: true,
  run: async i => {
    const code = i.options.getString('code', true);

    let buf = '';
    const sink = { write(s) { buf += s; return true; } };
    const captured = new console.Console({ stdout: sink, stderr: sink });

    const realConsole = globalThis.console;
    globalThis.console = new Proxy(captured, {
      get: (target, prop) => prop in target ? target[prop] : realConsole[prop],
    });

    let body;
    try {
      const result = await eval(code);
      captured.log(result);
      body = '```ansi\n' + clip(buf, 1900) + '\n```';
    } catch (err) {
      captured.error(err?.stack ?? String(err));
      body = '```ansi\n' + clip(buf, 1900) + '\n```';
    } finally {
      globalThis.console = realConsole;
    }

    await i.reply({ content: body });
  }
};
