import sys
dll = sys.argv[1] if len(sys.argv) > 1 else "e:/jinyiNew/Light/ChocoLight/build/bin/Release/Light.dll"
d = open(dll, 'rb').read()
t = b'goto continue'
pos = 0
count = 0
while True:
    idx = d.find(t, pos)
    if idx < 0:
        break
    count += 1
    ctx = d[max(0, idx-60):idx+60]
    print(f"#{count} at 0x{idx:X}:")
    print(f"  context: {ctx}")
    pos = idx + 1
print(f"\nTotal occurrences: {count}")
