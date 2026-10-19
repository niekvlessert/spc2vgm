"""Generate ZIP and RAR4/RSN fixtures from our original synthetic SPC."""
import struct
import zlib
from pathlib import Path
from zipfile import ZipFile
root = Path('build-web/test-fixtures')
data = (root / 'tone.spc').read_bytes()
with ZipFile(root / 'soundtrack.zip', 'w') as archive:
    archive.writestr('folder/Tone.SPC', data)
    archive.writestr('notes.txt', 'Non-SPC files should be ignored')
with ZipFile(root / 'duplicates.zip', 'w') as archive:
    archive.writestr('a/tone.spc', data)
    archive.writestr('b/tone.spc', data)
with ZipFile(root / 'empty.zip', 'w') as archive:
    archive.writestr('notes.txt', 'No music')
def header(kind, flags, body):
    raw = struct.pack('<BHH', kind, flags, 7 + len(body)) + body
    return struct.pack('<H', zlib.crc32(raw) & 0xffff) + raw
name = b'Tone.spc'
body = struct.pack('<IIBIIBBHI', len(data), len(data), 3, zlib.crc32(data), 0, 20, 0x30, len(name), 0x20) + name
rar = b'Rar!\x1a\x07\x00' + header(0x73, 0, bytes(6)) + header(0x74, 0x8000, body) + data + header(0x7b, 0, b'')
(root / 'soundtrack.rsn').write_bytes(rar)
(root / 'invalid.rsn').write_bytes(b'not an archive')
(root / 'invalid.spc').write_bytes(b'not an SPC')
print('Generated original ZIP and RAR4/RSN fixtures')
