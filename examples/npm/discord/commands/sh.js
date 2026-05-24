import { SlashCommandBuilder, ApplicationIntegrationType, InteractionContextType } from 'discord.js';
import { spawn } from 'node:child_process';

const clip = (s, n) => (s.length > n ? s.slice(0, n) + '\n…' : s);

const run = cmd => new Promise(resolve => {
  const child = spawn(cmd, [], { shell: true });
  let stdout = '';
  let stderr = '';

  child.stdout.on('data', chunk => {
    stdout += chunk;
    process.stdout.write(chunk);
  });
  child.stderr.on('data', chunk => {
    stderr += chunk;
    process.stderr.write(chunk);
  });

  child.on('error', err => resolve({ stdout, stderr: stderr + (err?.stack ?? String(err)), code: -1 }));
  child.on('close', code => resolve({ stdout, stderr, code: code ?? -1 }));
});

export default {
  builder: new SlashCommandBuilder()
    .setName('sh')
    .setDescription('run a shell command (owner only)')
    .addStringOption(o => o.setName('cmd').setDescription('command to run').setRequired(true))
    .setIntegrationTypes(ApplicationIntegrationType.GuildInstall, ApplicationIntegrationType.UserInstall)
    .setContexts(InteractionContextType.Guild, InteractionContextType.BotDM, InteractionContextType.PrivateChannel),
  ownerOnly: true,
  run: async i => {
    const cmd = i.options.getString('cmd', true);
    console.log(`[sh] ${i.user.tag} (${i.user.id}): ${cmd}`);
    await i.deferReply();

    const { stdout, stderr, code } = await run(cmd);

    const sections = [`\`$ ${clip(cmd, 200)}\` · exit ${code}`];
    if (stdout.trim()) sections.push(`**stdout**\n\`\`\`ansi\n${clip(stdout.trimEnd(), 1500)}\n\`\`\``);
    if (stderr.trim()) sections.push(`**stderr**\n\`\`\`ansi\n${clip(stderr.trimEnd(), 1500)}\n\`\`\``);
    if (!stdout.trim() && !stderr.trim()) sections.push('_(no output)_');

    await i.editReply(sections.join('\n'));
  }
};
