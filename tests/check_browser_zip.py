from zipfile import ZipFile
with ZipFile('build-web/test-fixtures/browser-output.zip') as archive:
    assert archive.testzip() is None
    assert set(archive.namelist()) == {'Tone.vgm', '_conversion-report.txt'}
    vgm = archive.read('Tone.vgm')
    assert vgm[:4] == b'Vgm '
    assert int.from_bytes(vgm[0x18:0x1c], 'little') == 441000
    assert int.from_bytes(vgm[0x60:0x64], 'little') == 33868800
    assert '1 converted, 0 failed.' in archive.read('_conversion-report.txt').decode()
print('Browser download: valid ZIP, 10-second OPL4 VGM, conversion report')
