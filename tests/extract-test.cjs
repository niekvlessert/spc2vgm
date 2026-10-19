const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const create = require(path.join(root, 'web/dist/unarr_extract.js'));
(async () => {
  for (const [name, expected] of [['soundtrack.rsn', 0], ['soundtrack.zip', 0], ['duplicates.zip', 1], ['empty.zip', 1], ['invalid.rsn', 1]]) {
    const logs = [];
    const mod = await create({ noInitialRun: true, print: line => logs.push(line), printErr: line => logs.push(line) });
    mod.FS.mkdir('/tracks');
    mod.FS.writeFile('/archive', fs.readFileSync(path.join(root, 'build-web/test-fixtures', name)));
    const code = mod.callMain(['/archive', '/tracks']);
    assert.equal(code, expected, `${name}: ${logs.join('\n')}`);
    if (!code) {
      const files = mod.FS.readdir('/tracks').filter(file => /\.spc$/i.test(file));
      assert.equal(files.length, 1);
      assert.deepEqual(Buffer.from(mod.FS.readFile(`/tracks/${files[0]}`)), fs.readFileSync(path.join(root, 'build-web/test-fixtures/tone.spc')));
    }
    console.log(`${name}: ${code === 0 ? 'extracted byte-identical SPC' : 'correctly rejected'}`);
  }
  process.exitCode = 0; // Expected archive failures set Node's exit code.
})().catch(error => { console.error(error); process.exitCode = 1; });
