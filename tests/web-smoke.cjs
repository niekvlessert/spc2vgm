const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { spc } = require('./fixture.cjs');
const root = path.resolve(__dirname, '..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'spc2vgm-web-'));
const create = require(path.join(root, 'web/dist/spc2vgm.js'));
async function main() {
  try {
    for (const [name, settings] of [['tone', {}], ['noise', { noise: true }], ['echo', { echo: true }]]) {
      const bytes = spc(settings), input = path.join(temporary, `${name}.spc`), output = path.join(temporary, `${name}.vgm`);
      fs.writeFileSync(input, bytes);
      const args = ['--auto-playback', '--prune-samples', '--jobs', '1', '--playback', '3'];
      const native = spawnSync(path.join(root, 'build-native/spc2vgm'), [input, '-o', output, ...args], { encoding: 'utf8' });
      assert.equal(native.status, 0, native.stdout + native.stderr);
      const logs = [];
      const mod = await create({ noInitialRun: true, print: line => logs.push(line), printErr: line => logs.push(line) });
      mod.FS.mkdir('/input'); mod.FS.mkdir('/output'); mod.FS.writeFile('/input/test.spc', bytes);
      assert.equal(mod.callMain(['/input/test.spc', '-o', '/output/test.vgm', ...args]), 0, logs.join('\n'));
      const actual = Buffer.from(mod.FS.readFile('/output/test.vgm'));
      assert.equal(actual.subarray(0, 4).toString(), 'Vgm ');
      assert.equal(actual.readUInt32LE(0x60) & 0x3fffffff, 33868800);
      assert.deepEqual(actual, fs.readFileSync(output), `${name}: native / WASM VGM bytes differ`);
      console.log(`${name}: native and WebAssembly output identical (${actual.length} bytes)`);
      if (name === 'tone') {
        fs.mkdirSync(path.join(root, 'build-web/test-fixtures'), { recursive: true });
        fs.writeFileSync(path.join(root, 'build-web/test-fixtures/tone.spc'), bytes);
      }
    }
    const createOptimizer = require(path.join(root, 'web/dist/vgm_cmp.js'));
    const optimizer = await createOptimizer({ noInitialRun: true, print() {}, printErr() {} });
    const source = fs.readFileSync(path.join(temporary, 'tone.vgm'));
    optimizer.FS.writeFile('/in.vgm', source);
    assert.equal(optimizer.callMain(['/in.vgm', '/out.vgm']), 0);
    const optimized = optimizer.FS.analyzePath('/out.vgm').exists ? optimizer.FS.readFile('/out.vgm') : source;
    assert.equal(Buffer.from(optimized).subarray(0, 4).toString(), 'Vgm ');
    assert.ok(optimized.length <= source.length);
    console.log(`vgm_cmp: ${source.length} -> ${optimized.length} bytes`);
    const bad = await create({ noInitialRun: true, print() {}, printErr() {} });
    bad.FS.writeFile('/bad.spc', new Uint8Array(20));
    assert.notEqual(bad.callMain(['/bad.spc', '-o', '/out/bad.vgm', '--jobs', '1']), 0);
    console.log('Invalid SPC rejected');
    process.exitCode = 0; // Emscripten mirrors the deliberately failing callMain to Node.
  } finally { fs.rmSync(temporary, { recursive: true, force: true }); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
