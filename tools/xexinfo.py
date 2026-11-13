"""Prints the XEX header fields the loader checks: python xexinfo.py <file.xex> ..."""
import struct
import sys

for path in sys.argv[1:]:
    x = open(path, 'rb').read()
    magic, flags, hsize, _, sec, n = struct.unpack_from('>4sIIIII', x, 0)
    print('%s\n  %s module flags %X, header size %X, security at %X' % (path, magic.decode(), flags, hsize, sec))
    keys = {}
    for i in range(n):
        k, v = struct.unpack_from('>II', x, 0x18 + i * 8)
        keys[k] = v
    print('  keys: ' + ' '.join('%08X=%08X' % kv for kv in sorted(keys.items())))
    sec_size, image_size = struct.unpack_from('>II', x, sec)
    info_size, image_flags, load = struct.unpack_from('>III', x, sec + 0x108)
    regions, media, pages = struct.unpack_from('>III', x, sec + 0x178)
    print('  image size %X, image flags %08X, load address %08X, regions %08X, media %08X, page descriptors %d'
          % (image_size, image_flags, load, regions, media, pages))
    first = [struct.unpack_from('>I', x, sec + 0x184 + i * 0x18)[0] for i in range(min(pages, 6))]
    print('  first descriptors (pages<<4|type): ' + ' '.join('%X' % d for d in first))
    if 0x3FF in keys:
        size, enc, comp = struct.unpack_from('>IHH', x, keys[0x3FF])
        print('  file format: encryption %d compression %d' % (enc, comp))
        if comp == 1 and enc == 0:
            # basic compression, not encrypted: the PE header is the start of the data
            pe = hsize + struct.unpack_from('<I', x, hsize + 0x3C)[0]
            machine, nsec, _, _, _, opt, chars = struct.unpack_from('<HHIIIHH', x, pe + 4)
            base = struct.unpack_from('<I', x, pe + 24 + 28)[0]
            sub = struct.unpack_from('<H', x, pe + 24 + 68)[0]
            salign, falign = struct.unpack_from('<II', x, pe + 24 + 32)
            print('  PE: machine %X sections %d characteristics %04X image base %08X subsystem %d align %X/%X'
                  % (machine, nsec, chars, base, sub, salign, falign))
