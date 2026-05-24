import { SlashCommandBuilder, ApplicationIntegrationType, InteractionContextType } from 'discord.js';
import { spawn } from 'node:child_process';

const clip = (s, n) => (s.length > n ? s.slice(0, n) + '\n…' : s);
const MAX_OUTPUT = 64 * 1024;
const COMMAND_TIMEOUT_MS = 15_000;

const run = cmd => new Promise(resolve => {
  const child = spawn(cmd, [], { shell: true });
  let stdout = '';
  let stderr = '';
  let done = false;
  const finish = result => {
    if (done) return;
    done = true;
    clearTimeout(timer);
    resolve(result);
  };
  const append = (current, chunk) =>
    clip(current + chunk.toString(), MAX_OUTPUT);
  const timer = setTimeout(() => {
    child.kill('SIGTERM');
    finish({ stdout, stderr: stderr + '\ncommand timed out', code: -1 });
  }, COMMAND_TIMEOUT_MS);

  child.stdout.on('data', chunk => {
    stdout = append(stdout, chunk);
  });
  child.stderr.on('data', chunk => {
    stderr = append(stderr, chunk);
  });

  child.on('error', err => finish({ stdout, stderr: stderr + (err?.stack ?? String(err)), code: -1 }));
  child.on('close', code => finish({ stdout, stderr, code: code ?? -1 }));
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
    console.log(`[sh] ${i.user.tag} (${i.user.id})`);
    await i.deferReply();

    const { stdout, stderr, code } = await run(cmd);

    const sections = [`\`$ ${clip(cmd, 200)}\` · exit ${code}`];
    if (stdout.trim()) sections.push(`**stdout**\n\`\`\`ansi\n${clip(stdout.trimEnd(), 1500)}\n\`\`\``);
    if (stderr.trim()) sections.push(`**stderr**\n\`\`\`ansi\n${clip(stderr.trimEnd(), 1500)}\n\`\`\``);
    if (!stdout.trim() && !stderr.trim()) sections.push('_(no output)_');

    await i.editReply(sections.join('\n'));
  }
};
