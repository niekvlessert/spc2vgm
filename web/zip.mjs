// ZIP32 writer: UTF-8 names, CRC-32, optional browser-native DEFLATE.
// Blob parts avoid concatenating the entire pack into another large buffer.
const encoder = new TextEncoder();
const table = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let k = 0; k < 8; k++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = table[(crc ^ byte) & 255] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}
function header(length) {
  const bytes = new Uint8Array(length);
  return { bytes, view: new DataView(bytes.buffer) };
}
export class Zip {
  parts = []; entries = []; offset = 0;
  async add(name, input) {
    const filename = encoder.encode(name);
    if (filename.length > 65535 || this.entries.length >= 65535) throw new Error('ZIP entry limit exceeded');
    const original = input instanceof Uint8Array ? input : new Uint8Array(input);
    let bytes = original, method = 0;
    if (typeof CompressionStream !== 'undefined') {
      try {
        const stream = new Blob([original]).stream().pipeThrough(new CompressionStream('deflate-raw'));
        const compressed = new Uint8Array(await new Response(stream).arrayBuffer());
        if (compressed.length < bytes.length) { bytes = compressed; method = 8; }
      } catch { /* Stored ZIP entries work in browsers without raw DEFLATE. */ }
    }
    if (this.offset + bytes.length + filename.length + 30 > 0xffffffff) throw new Error('Pack exceeds ZIP32 size limit');
    const crc = crc32(original), { bytes: local, view } = header(30);
    view.setUint32(0, 0x04034b50, true); view.setUint16(4, 20, true);
    view.setUint16(6, 0x800, true); view.setUint16(8, method, true);
    view.setUint16(12, 33, true); // 1980-01-01, reproducible output.
    view.setUint32(14, crc, true); view.setUint32(18, bytes.length, true);
    view.setUint32(22, original.length, true); view.setUint16(26, filename.length, true);
    this.parts.push(local, filename, bytes);
    this.entries.push({ filename, crc, size: original.length, packed: bytes.length, method, offset: this.offset });
    this.offset += local.length + filename.length + bytes.length;
  }
  blob() {
    const central = [];
    let length = 0;
    for (const entry of this.entries) {
      const { bytes, view } = header(46);
      view.setUint32(0, 0x02014b50, true); view.setUint16(4, 20, true); view.setUint16(6, 20, true);
      view.setUint16(8, 0x800, true); view.setUint16(10, entry.method, true); view.setUint16(14, 33, true);
      view.setUint32(16, entry.crc, true); view.setUint32(20, entry.packed, true); view.setUint32(24, entry.size, true);
      view.setUint16(28, entry.filename.length, true); view.setUint32(42, entry.offset, true);
      central.push(bytes, entry.filename); length += bytes.length + entry.filename.length;
    }
    const { bytes, view } = header(22);
    view.setUint32(0, 0x06054b50, true); view.setUint16(8, this.entries.length, true); view.setUint16(10, this.entries.length, true);
    view.setUint32(12, length, true); view.setUint32(16, this.offset, true);
    return new Blob([...this.parts, ...central, bytes], { type: 'application/zip' });
  }
}
