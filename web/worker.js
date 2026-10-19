/* A fresh worker per track bounds the lifetime of the native heap. */
self.onmessage = async ({ data }) => {
  const logs = [];
  const print = line => { logs.push(String(line)); self.postMessage({ type: 'log', line: String(line) }); };
  const options = { noInitialRun: true, print, printErr: print, locateFile: name => new URL(`./dist/${name}`, self.location.href).href };
  try {
    if (data.type === 'extract') {
      importScripts('./dist/unarr_extract.js');
      const mod = await create_unarr_extract(options);
      mod.FS.mkdir('/input'); mod.FS.mkdir('/tracks');
      mod.FS.writeFile('/input/archive.rsn', new Uint8Array(data.bytes));
      const code = mod.callMain(['/input/archive.rsn', '/tracks']);
      if (code !== 0) throw new Error(logs.slice(-4).join('\n') || `Extraction failed (${code})`);
      const files = mod.FS.readdir('/tracks').filter(name => /\.spc$/i.test(name)).sort().map(name => ({ name, bytes: mod.FS.readFile(`/tracks/${name}`).buffer }));
      self.postMessage({ type: 'extracted', files }, files.map(file => file.bytes));
    } else if (data.type === 'convert') {
      importScripts('./dist/spc2vgm.js');
      const mod = await create_spc2vgm(options);
      mod.FS.mkdir('/input'); mod.FS.mkdir('/output');
      const inputPath = '/input/' + (data.name || 'track.spc').split(/[\\/]/).pop();
      mod.FS.writeFile(inputPath, new Uint8Array(data.bytes));
      const args = [inputPath, '-o', '/output/track.vgm', '--auto-playback', '--prune-samples', '--jobs', '1', '--opl4-ram-kib', String(data.ram), '--creator', data.creator || 'spc2vgm'];
      if (data.seconds) args.push('--playback', String(data.seconds));
      const code = mod.callMain(args);
      if (code !== 0) throw new Error(logs.slice(-4).join('\n') || `Conversion failed (${code})`);
      let bytes = mod.FS.readFile('/output/track.vgm');
      // Release the converter's runtime when this worker terminates.
      if (data.optimize) {
        importScripts('./dist/vgm_cmp.js');
        const optimizer = await create_vgm_cmp(options);
        optimizer.FS.writeFile('/track.vgm', bytes);
        const code = optimizer.callMain(['/track.vgm', '/optimized.vgm']);
        if (code !== 0) throw new Error(`VGM optimization failed (${code})`);
        if (optimizer.FS.analyzePath('/optimized.vgm').exists) bytes = optimizer.FS.readFile('/optimized.vgm');
      }
      self.postMessage({ type: 'converted', bytes: bytes.buffer }, [bytes.buffer]);
    } else throw new Error('Unknown worker operation');
  } catch (error) {
    self.postMessage({ type: 'error', message: error.message || String(error) });
  }
};
