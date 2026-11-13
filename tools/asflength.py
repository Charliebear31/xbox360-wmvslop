"""Checks the plugin's way of reading a .wmv file's playing time (video_length_ms in src/wmvslop.c).

python asflength.py <ffprobe.exe> <file.wmv> ...

Same steps as the C: find the ASF File Properties object's ID in the first 8 KB; play duration (100 ns units)
is 64 bytes after the ID's start, preroll (ms) 80 bytes after; length = play duration - preroll. Prints that
beside ffprobe's duration."""
import struct
import subprocess
import sys

FILE_PROPERTIES = bytes([0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65])


def length_ms(path):
    head = open(path, 'rb').read(8192)
    at = head.find(FILE_PROPERTIES)
    if at < 0 or at + 104 > len(head):
        return 0
    play = struct.unpack_from('<Q', head, at + 64)[0] // 10000
    preroll = struct.unpack_from('<Q', head, at + 80)[0]
    if play <= preroll or play - preroll < 2000 or play - preroll > 86400000:
        return 0
    return play - preroll


ffprobe = sys.argv[1]
for path in sys.argv[2:]:
    probe = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path],
                           capture_output=True, text=True).stdout.strip()
    print('%8d ms from the header, ffprobe %s s  %s' % (length_ms(path), probe, path))
