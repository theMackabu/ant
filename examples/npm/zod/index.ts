import * as z from 'zod';

const data = {
  username: 'themackabu',
  bio: ':3',
  xp: 67
};

const Player = z.object({
  username: z.string(),
  bio: z.string(),
  xp: z.number()
});

const CompiledPlayer = z.compile(Player);

console.log(Player.parse(data));
console.log(CompiledPlayer.parse(data));
