import sys

path = sys.argv[1]
name = sys.argv[2]
data = open(path, 'rb').read()

print(f'static const unsigned char {name}[] = {{')
for i in range(0, len(data), 16):
    print('  ' + ''.join(f'0x{b:02x}, ' for b in data[i:i + 16]).rstrip())
print('};')
print(f'static const unsigned long {name}_len = {len(data)};')
