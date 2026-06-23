// decode.js — in-browser/Node port of tools/decode_beacon.py
//
// Reads the hidden WBCN WiFi payload out of a 24-bit BMP's pixel LSBs.
// Parses the BMP bytes directly (not via <canvas>) on purpose: canvas
// getImageData can apply alpha-premultiply / colour management and corrupt the
// low bits. Raw BMP parsing is byte-exact, matching the firmware + Python.

export function crc16ccittFalse(bytes, len) {
  let crc = 0xFFFF;
  for (let i = 0; i < len; i++) {
    crc ^= bytes[i] << 8;
    for (let b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
  }
  return crc;
}

const le16 = (d, o) => d[o] | (d[o + 1] << 8);
const le32 = (d, o) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
const i32  = (d, o) =>  d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24);

// 24-bit BMP -> channel-byte stream, top-down, row-major, R,G,B order
// (exactly the order the firmware embeds and decode_beacon.py reads).
export function bmpToChannels(data) {
  if (data.length < 54 || data[0] !== 0x42 || data[1] !== 0x4D)
    throw new Error('not a BMP file');
  const pixOff = le32(data, 10);
  const width  = le32(data, 18);
  const height = i32(data, 22);          // signed: negative = top-down rows
  const bpp    = le16(data, 28);
  if (bpp !== 24) throw new Error(`need a 24-bit BMP, got ${bpp}-bit`);

  const topDown = height < 0;
  const H = Math.abs(height), W = width;
  const stride = (W * 3 + 3) & ~3;       // rows padded to 4 bytes
  const ch = new Uint8Array(W * H * 3);
  let k = 0;
  for (let y = 0; y < H; y++) {
    const fileRow = topDown ? y : (H - 1 - y);
    let p = pixOff + fileRow * stride;
    for (let x = 0; x < W; x++) {
      const b = data[p], g = data[p + 1], r = data[p + 2];   // BMP is B,G,R
      ch[k++] = r; ch[k++] = g; ch[k++] = b;                 // emit R,G,B
      p += 3;
    }
  }
  return ch;
}

const MAGIC = [0x57, 0x42, 0x43, 0x4E]; // 'WBCN'

export function decodeChannels(ch) {
  const cap = Math.floor(ch.length / 8);
  const byteAt = (k) => { let v = 0; for (let b = 0; b < 8; b++) v = (v << 1) | (ch[k * 8 + b] & 1); return v; };

  const frame = [];
  const need = (n) => {
    while (frame.length < n) {
      if (frame.length >= cap) throw new Error('ran past end of payload');
      frame.push(byteAt(frame.length));
    }
  };

  need(6);
  for (let i = 0; i < 4; i++)
    if (frame[i] !== MAGIC[i])
      throw new Error('no WBCN marker — not a beacon image, or it was re-saved lossily (JPEG?)');
  const version = frame[4];
  if (version !== 1) throw new Error(`unsupported version ${version}`);
  const n = frame[5];

  let p = 6;
  const nets = [];
  const dec = new TextDecoder('utf-8');
  for (let i = 0; i < n; i++) {
    need(p + 1);
    const sl = frame[p]; p += 1;
    need(p + sl + 8);                                   // ssid + bssid(6)+rssi(1)+ch(1)
    const ssid = dec.decode(Uint8Array.from(frame.slice(p, p + sl))); p += sl;
    const bssid = frame.slice(p, p + 6)
      .map(x => x.toString(16).padStart(2, '0').toUpperCase()).join(':'); p += 6;
    let rssi = frame[p]; if (rssi > 127) rssi -= 256; p += 1;
    const channel = frame[p]; p += 1;
    nets.push({ ssid, bssid, rssi, channel });
  }

  need(p + 2);
  const stored = (frame[p] << 8) | frame[p + 1];
  const calc = crc16ccittFalse(frame, p);
  if (stored !== calc)
    throw new Error(`CRC mismatch (stored 0x${stored.toString(16).padStart(4, '0')}, ` +
                    `calc 0x${calc.toString(16).padStart(4, '0')}) — payload corrupted`);

  return { version, count: n, networks: nets, frame: frame.slice(0, p + 2) };
}

// ── PNG (lossless) -> same top-down R,G,B channel stream ──────────────────────
// Parsed properly (inflate + unfilter), NOT via <canvas>, because canvas can
// colour-manage / premultiply and corrupt the low bits we read.
const be32 = (d, o) => ((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0;

async function inflate(zbytes) {
  const ds = new DecompressionStream('deflate');           // 'deflate' = zlib-wrapped (PNG IDAT)
  const buf = await new Response(new Blob([zbytes]).stream().pipeThrough(ds)).arrayBuffer();
  return new Uint8Array(buf);
}

function paeth(a, b, c) {
  const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

export async function pngToChannels(data) {
  const SIG = [137, 80, 78, 71, 13, 10, 26, 10];
  for (let i = 0; i < 8; i++) if (data[i] !== SIG[i]) throw new Error('not a PNG file');

  let off = 8, width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat = [];
  while (off + 8 <= data.length) {
    const len = be32(data, off);
    const type = String.fromCharCode(data[off + 4], data[off + 5], data[off + 6], data[off + 7]);
    const start = off + 8;
    if (type === 'IHDR') {
      width = be32(data, start); height = be32(data, start + 4);
      bitDepth = data[start + 8]; colorType = data[start + 9];
      if (data[start + 12] !== 0) throw new Error('interlaced PNG unsupported');
    } else if (type === 'IDAT') {
      idat.push(data.subarray(start, start + len));
    } else if (type === 'IEND') break;
    off = start + len + 4;                                   // + 4 CRC
  }
  if (bitDepth !== 8) throw new Error(`PNG bit depth ${bitDepth} unsupported (need 8)`);
  const ch = colorType === 2 ? 3 : colorType === 6 ? 4 : 0; // RGB or RGBA only
  if (!ch) throw new Error(`PNG colour type ${colorType} unsupported (need RGB/RGBA)`);

  let total = 0; for (const c of idat) total += c.length;
  const z = new Uint8Array(total); let zp = 0; for (const c of idat) { z.set(c, zp); zp += c.length; }
  const raw = await inflate(z);                              // filtered scanlines

  const stride = width * ch;
  const out = new Uint8Array(width * height * 3);            // top-down R,G,B
  let prev = new Uint8Array(stride);
  const cur = new Uint8Array(stride);
  let ip = 0, op = 0;
  for (let y = 0; y < height; y++) {
    const f = raw[ip++];
    for (let x = 0; x < stride; x++) {
      let v = raw[ip++];
      const a = x >= ch ? cur[x - ch] : 0;                  // left
      const b = prev[x];                                    // up
      const c = x >= ch ? prev[x - ch] : 0;                 // up-left
      if (f === 1) v += a; else if (f === 2) v += b;
      else if (f === 3) v += (a + b) >> 1; else if (f === 4) v += paeth(a, b, c);
      cur[x] = v & 0xff;
    }
    for (let x = 0; x < width; x++) {                        // emit R,G,B (drop alpha)
      const s = x * ch; out[op++] = cur[s]; out[op++] = cur[s + 1]; out[op++] = cur[s + 2];
    }
    prev.set(cur);
  }
  return out;
}

// Route by magic: BMP ('BM') or PNG (\x89PNG). Async because PNG inflate is async.
export async function decodeBeacon(data) {
  let ch;
  if (data[0] === 0x42 && data[1] === 0x4D) ch = bmpToChannels(data);
  else if (data[0] === 0x89 && data[1] === 0x50) ch = await pngToChannels(data);
  else throw new Error('unsupported file — need a lossless BMP or PNG');
  return decodeChannels(ch);
}
