from pathlib import Path
from zipfile import ZipFile
root = Path('build-web/test-fixtures')
with ZipFile(root / 'output.zip') as z:
    assert z.testzip() is None
    assert z.read('音楽.vgm') == b'Vgm ' + b'abc' * 2000
    assert z.read('_conversion-report.txt') == b'2 tracks\n'
with ZipFile(root / 'stored.zip') as z:
    assert z.testzip() is None
    assert z.read('音楽.vgm') == b'Vgm '
    assert z.read('empty.vgm') == b''
print('Python zipfile independently verified both ZIPs')
