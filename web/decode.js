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

export function decodeBeacon(data) {
  return decodeChannels(bmpToChannels(data));
}
