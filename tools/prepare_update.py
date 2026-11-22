#!/usr/bin/env python3
"""Check your own Far Cry 3 (Xbox 360) default.xex and unpack patch.dat/patch.fat from your own Title Update 5.

  python prepare_update.py <path to default.xex> <path to tu00000004_00000000> <output folder> [--icons]

--icons (optional) also adds button icons for the Switch layout (A/B and X/Y swapped, white). They are made from
common.fat/common.dat next to your default.xex and merged into the unpacked patch files. Your originals are not changed.
Archive format notes: Gibbed.Dunia2 (Zlib licence).

Nothing is downloaded and no game data is included here. Standard library only."""
import hashlib, lzma, struct, sys
from pathlib import Path

XEX = 'f9c08098c7e435f89899955045608f727c0d1bea13e4de84e06b93cd729539df'
TU = '367a9caae9b3957aec1bb7258b62ecaaf0efbf3230fd4749bbee2ba156d80c09'
MEDIA, TITLE, BLK = '003B8D49', '5553088C', 0x1000


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def stfs_files(d):
    """Files of a LIVE/PIRS package in its read-only layout (one hash table per level)."""
    if d[:4] not in (b'LIVE', b'PIRS'):
        sys.exit('Not a LIVE/PIRS title update package')
    media, title = d[0x354:0x358].hex().upper(), d[0x360:0x364].hex().upper()
    if (media, title) != (MEDIA, TITLE):
        sys.exit(f'Wrong update: Media ID {media}, Title ID {title}; needed {MEDIA}, {TITLE}')
    start = (struct.unpack('>I', d[0x340:0x344])[0] + BLK - 1) & ~(BLK - 1)
    tables = 1 if d[0x37B] & 1 else 2          # tables per level
    table_blocks = struct.unpack('<H', d[0x37C:0x37E])[0]
    table_start = int.from_bytes(d[0x37E:0x381], 'little')

    def phys(n):
        q = n + (n // 0xAA + 1) * tables
        if n >= 0xAA:
            q += (n // 0x70E4 + 1) * tables
        return start + q * BLK

    def block(n):
        return d[phys(n):phys(n) + BLK]

    def next_block(n):                          # 24-byte entry: SHA-1, status, next block
        group = n - n % 0xAA
        table = phys(group) - tables * BLK
        e = table + (n % 0xAA) * 0x18
        return int.from_bytes(d[e + 0x15:e + 0x18], 'big')

    n, entries = table_start, b''
    for _ in range(table_blocks):
        entries += block(n)
        n = next_block(n)
    for o in range(0, len(entries), 0x40):
        e = entries[o:o + 0x40]
        name = e[:e[0x28] & 0x3F].decode('latin1')
        if not name or e[0x28] & 0x80:
            continue
        size, cur = struct.unpack('>I', e[0x34:0x38])[0], int.from_bytes(e[0x2F:0x32], 'little')
        out = bytearray()
        for i in range((size + BLK - 1) // BLK):
            out += block(cur + i if e[0x28] & 0x40 else cur)
            if not e[0x28] & 0x40:
                cur = next_block(cur)
        yield name, bytes(out[:size])


ICON_DIR = 'ui/common/textures/buttons/platform/'
PAIRS = [('tx_button1', 'tx_button2'), ('tx_button3', 'tx_button4')] + [
    (f'tx_qte_button{a}_{s}', f'tx_qte_button{b}_{s}') for a, b in ((1, 2), (3, 4)) for s in ('up', 'down')]
XBT_HEADER, GAIN = 0x58, 1.6


def crc64(name):
    h = 0
    for ch in name.lower().replace('/', chr(92)).encode('latin-1'):
        h ^= ch
        for _ in range(8):
            h = (h >> 1) ^ (0xD800000000000000 if h & 1 else 0)
    return h


def fat_read(path):
    d = Path(path).read_bytes()
    magic, ver, plat = struct.unpack_from('<III', d, 0)
    if magic != 0x46415432 or ver != 9 or plat & 0xFF != 2:
        sys.exit(f'{path}: not an Xbox 360 Far Cry 3 archive index')
    subtot, subcnt, n = struct.unpack_from('<iiI', d, 12)
    ents, off = {}, 24
    for _ in range(n):
        a, b, c, x, e = struct.unpack_from('>5I', d, off)
        off += 20
        ents[a << 32 | b] = [c, x << 2 | e >> 30, e & 0x3FFFFFFF]   # usize/scheme, offset, stored size
    return d[:12], subtot, subcnt, ents, d[off:]


def fat_write(path, fat):
    hdr, subtot, subcnt, ents, tail = fat
    b = bytearray(hdr) + struct.pack('<iiI', subtot, subcnt, len(ents))
    for h in sorted(ents):
        c, offset, size = ents[h]
        b += struct.pack('>5I', h >> 32, h & 0xFFFFFFFF, c, offset >> 2, (offset & 3) << 30 | size)
    Path(path).write_bytes(b + tail)


def unpack(d):
    if len(d) > 6 and d[1] == 0x5D:
        f = [{'id': lzma.FILTER_LZMA1, 'dict_size': int.from_bytes(d[2:6], 'little'), 'lc': 3, 'lp': 0, 'pb': 2}]
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=f).decompress(d[6:])
    return d


def grey565(c):
    y = 0.299 * ((c >> 11 & 31) * 255 / 31) + 0.587 * ((c >> 5 & 63) * 255 / 63) + 0.114 * ((c & 31) * 255 / 31)
    v = int(round(min(255.0, y * GAIN)))
    r5 = round(v * 31 / 255)
    g6 = min(63, max(0, 2 * r5 + (1 if (v * 63 / 255 - 2 * r5) >= 0.5 else 0)))
    return r5 << 11 | g6 << 5 | r5


def whiten(p):
    """DXT5 blocks, 8-in-16 swapped: replace only the two colour endpoints of every block."""
    n = len(p) & ~1
    s = bytearray(p)
    s[0:n:2], s[1:n:2] = p[1:n:2], p[0:n:2]
    for o in range(0, len(s) - 15, 16):
        for k in (8, 10):
            s[o + k:o + k + 2] = grey565(int.from_bytes(s[o + k:o + k + 2], 'little')).to_bytes(2, 'little')
    r = bytearray(s)
    r[0:n:2], r[1:n:2] = s[1:n:2], s[0:n:2]
    return bytes(r)


def add_icons(game, out):
    _, _, _, src, _ = fat_read(game / 'common.fat')
    tex = {}
    with open(game / 'common.dat', 'rb') as f:
        for name in (x for pair in PAIRS for x in pair):
            e = src.get(crc64(ICON_DIR + name + '.xbt'))
            if e is None:
                sys.exit('common.fat has no ' + name)
            f.seek(e[1])
            tex[name] = unpack(f.read(e[2]))
    fat = fat_read(out / 'patch.fat')
    dat = bytearray((out / 'patch.dat').read_bytes())
    for a, b in PAIRS:
        for name, other in ((a, b), (b, a)):
            data = tex[name][:XBT_HEADER] + whiten(tex[other][XBT_HEADER:])
            dat += bytes(-len(dat) % 16)
            fat[3][crc64(ICON_DIR + name + '.xbt')] = [0, len(dat), len(data)]
            dat += data
    (out / 'patch.dat').write_bytes(dat)
    fat_write(out / 'patch.fat', fat)
    print('icons added to patch.dat/patch.fat')


def main():
    args = [a for a in sys.argv[1:] if a != '--icons']
    if len(args) != 3:
        sys.exit(__doc__)
    xex, tu, out = map(Path, args)
    if sha(xex) != XEX:
        sys.exit(f'{xex}: this is not the supported default.xex (Media ID {MEDIA}, version 0.0.0.4)')
    print('default.xex OK')
    print('title update', 'OK' if sha(tu) == TU else 'WARNING: hash differs from the tested file')
    out.mkdir(parents=True, exist_ok=True)
    done = []
    for name, data in stfs_files(tu.read_bytes()):
        if name in ('patch.dat', 'patch.fat'):
            (out / name).write_bytes(data)
            done.append(name)
            print(name, len(data), hashlib.sha256(data).hexdigest())
    if sorted(done) != ['patch.dat', 'patch.fat']:
        sys.exit('patch.dat/patch.fat not found in the update')
    if '--icons' in sys.argv:
        add_icons(xex.parent, out)


if __name__ == '__main__':
    main()
