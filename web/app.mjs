import { Zip } from './zip.mjs';
const $ = id => document.getElementById(id);
const MAX_INPUT = 64 * 1024 * 1024, MAX_OUTPUT = 512 * 1024 * 1024;
let selected = [], busy = false, currentWorker = null, rejectWorker = null, run = 0, downloadURL = null;
const log = line => { $('log').textContent = ($('log').textContent + line + '\n').slice(-32000); };
function setBusy(value) {
  busy = value;
  for (const id of ['files', 'ram', 'duration', 'creator', 'optimize']) $(id).disabled = value;
  $('convert').disabled = value || !selected.length; $('cancel').hidden = !value;
}
function clearDownload() {
  if (downloadURL) URL.revokeObjectURL(downloadURL);
  downloadURL = null; $('download').hidden = true; $('download').removeAttribute('href');
}
function choose(files) {
  if (busy) return;
  clearDownload(); selected = []; $('tracks').replaceChildren(); $('progress').hidden = true;
  const input = Array.from(files);
  if (!input.length) { $('selection').textContent = 'No soundtrack selected'; setBusy(false); return; }
  const isSPC = file => /\.spc$/i.test(file.name);
  if (input.some(file => !/\.(rsn|rar|zip|7z|spc)$/i.test(file.name))) {
    $('status').textContent = 'Choose an RSN, RAR, ZIP, 7Z archive or SPC files.';
  } else if (input.some(file => !isSPC(file)) && input.length !== 1) {
    $('status').textContent = 'Choose one archive at a time, or select several individual SPC files.';
  } else if (input.reduce((size, file) => size + file.size, 0) > MAX_INPUT) {
    $('status').textContent = 'Please choose at most 64 MiB of input files.';
  } else {
    selected = input;
    $('status').textContent = 'Ready to convert. Files stay on your device.';
  }
  $('selection').textContent = selected.length ? (selected.length === 1 ? selected[0].name : `${selected.length} SPC tracks selected`) : 'No soundtrack selected';
  setBusy(false);
}
$('files').addEventListener('change', event => choose(event.target.files));
for (const type of ['dragenter', 'dragover']) $('drop').addEventListener(type, event => { event.preventDefault(); if (!busy) $('drop').classList.add('drag'); });
for (const type of ['dragleave', 'drop']) $('drop').addEventListener(type, event => { event.preventDefault(); $('drop').classList.remove('drag'); });
$('drop').addEventListener('drop', event => choose(event.dataTransfer.files));
function operation(payload, transfer = []) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('./worker.js', import.meta.url));
    currentWorker = worker; rejectWorker = reject;
    const finish = (error, data) => {
      worker.terminate(); currentWorker = null; rejectWorker = null;
      error ? reject(error) : resolve(data);
    };
    worker.onmessage = ({ data }) => {
      if (data.type === 'log') log(data.line);
      else if (data.type === 'error') finish(new Error(data.message));
      else finish(null, data);
    };
    worker.onerror = event => finish(new Error(event.message || 'Worker failed. Check that the WebAssembly build is deployed.'));
    worker.onmessageerror = () => finish(new Error('Unable to read worker result.'));
    worker.postMessage(payload, transfer);
  });
}
$('cancel').addEventListener('click', () => {
  ++run;
  currentWorker?.terminate(); currentWorker = null;
  rejectWorker?.(new DOMException('Cancelled', 'AbortError')); rejectWorker = null;
  $('status').textContent = 'Cancelled. Choose Convert soundtrack to start again.';
  $('progress').hidden = true; setBusy(false);
});
function safeName(name) {
  return name.split(/[\\/]/).pop().replace(/[\u0000-\u001f<>:"|?*]/g, '_').replace(/^\.+/, '_') || 'track';
}
$('convert').addEventListener('click', async () => {
  if (busy || !selected.length) return;
  const id = ++run, files = [...selected];
  const ram = Number($('ram').value), seconds = Number($('duration').value), creator = $('creator').value.trim(), optimize = $('optimize').checked;
  const active = () => { if (run !== id) throw new DOMException('Cancelled', 'AbortError'); };
  setBusy(true); clearDownload(); $('tracks').replaceChildren(); $('log').textContent = '';
  $('progress').hidden = false; $('progress').value = 0; $('progress').max = 1;
  const report = [`spc2vgm browser conversion`, `Source: ${files.map(file => file.name).join(', ')}`, `OPL4 RAM: ${ram} KiB`, `Playback: ${seconds || 'automatic'} seconds`, `Sample pruning: enabled`, `vgm_cmp: ${optimize ? 'enabled' : 'disabled'}`, ''];
  try {
    let tracks;
    if (files.length === 1 && !/\.spc$/i.test(files[0].name)) {
      $('status').textContent = 'Extracting SPC tracks…';
      const bytes = await files[0].arrayBuffer(); active();
      ({ files: tracks } = await operation({ type: 'extract', bytes }, [bytes])); active();
    } else {
      tracks = await Promise.all(files.map(async file => ({ name: file.name, bytes: await file.arrayBuffer() }))); active();
    }
    if (!tracks.length) throw new Error('No SPC tracks found.');
    if (tracks.length > 2000) throw new Error('Please use a pack with at most 2,000 tracks.');
    const zip = new Zip(), usedNames = new Set();
    let success = 0, failed = 0, outputSize = 0;
    $('progress').max = tracks.length;
    const rows = tracks.map(track => {
      const li = document.createElement('li'), name = document.createElement('span'), state = document.createElement('span');
      name.textContent = track.name; state.textContent = 'Waiting'; li.append(name, state); $('tracks').append(li); return state;
    });
    for (let index = 0; index < tracks.length; index++) {
      active(); const track = tracks[index]; rows[index].textContent = 'Converting…';
      $('status').textContent = `Converting ${index + 1} of ${tracks.length}: ${track.name}`; log(`\n--- ${track.name} ---`);
      let result;
      try {
        result = await operation({ type: 'convert', name: safeName(track.name), bytes: track.bytes, ram, seconds, creator, optimize }, [track.bytes]); active();
      } catch (error) {
        active(); failed++; rows[index].textContent = 'Failed'; report.push(`FAILED ${track.name}: ${error.message}`); log(error.message);
        $('progress').value = index + 1; continue;
      }
      outputSize += result.bytes.byteLength;
      if (outputSize > MAX_OUTPUT) throw new Error('Output exceeds the 512 MiB browser limit. Try fewer tracks or shorter playback.');
      const base = safeName(track.name).replace(/\.spc$/i, ''); let name = `${base}.vgm`, suffix = 2;
      while (usedNames.has(name.toLowerCase())) name = `${base} (${suffix++}).vgm`;
      usedNames.add(name.toLowerCase());
      await zip.add(name, result.bytes); active();
      success++; rows[index].textContent = `${(result.bytes.byteLength / 1024).toFixed(0)} KiB · Done`;
      report.push(`OK ${track.name} -> ${name} (${result.bytes.byteLength} bytes)`); $('progress').value = index + 1;
    }
    if (!success) throw new Error('All tracks failed. Open the conversion log for details.');
    $('status').textContent = 'Preparing your ZIP…';
    report.push('', `${success} converted, ${failed} failed.`, 'Output uses YMF278B/OPL4 and requires sufficient sample RAM.', 'SNES effects are approximated.');
    await zip.add('_conversion-report.txt', new TextEncoder().encode(report.join('\n'))); active();
    downloadURL = URL.createObjectURL(zip.blob());
    $('download').href = downloadURL; $('download').download = `${safeName(files[0].name).replace(/\.[^.]+$/, '')}-vgm.zip`; $('download').hidden = false;
    $('status').textContent = `${success} track${success === 1 ? '' : 's'} converted${failed ? `; ${failed} failed — see the report` : ''}. Your VGM ZIP is ready.`;
  } catch (error) {
    if (error.name !== 'AbortError' && run === id) { $('status').textContent = error.message; log(error.stack || String(error)); }
  } finally { if (run === id) setBusy(false); }
});
if (!('WebAssembly' in window) || !('Worker' in window)) {
  $('status').textContent = 'This converter needs a browser with WebAssembly and Web Worker support.';
  $('files').disabled = true;
}
