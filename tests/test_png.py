import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n'
p = 8; idat = b''; w = h = 0
while p < len(d):
    n, t = struct.unpack('>I4s', d[p:p+8]); body = d[p+8:p+8+n]
    crc = struct.unpack('>I', d[p+8+n:p+12+n])[0]
    assert crc == zlib.crc32(t + body) & 0xffffffff, t
    if t == b'IHDR': w, h = struct.unpack('>II', body[:8]); assert body[8:10] == b'\x08\x02'
    if t == b'IDAT': idat += body
    p += 12 + n
raw = zlib.decompress(idat)
assert (w, h) == (300, 220) and len(raw) == h * (w * 3 + 1)
for y in (0, 100, 219):
    row = raw[y*(w*3+1):(y+1)*(w*3+1)]
    assert row[0] == 0
    for x in (0, 7, 299):
        r, g, b = row[1+x*3:4+x*3]
        assert (r, g, b) == ((x + y) & 255, y, x & 255), (x, y, r, g, b)
print('png decode ok')
