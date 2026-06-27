// Node sanity test for decode.js against a real stego BMP.
// Generate one first:  python tools/test_data_roundtrip.py   (writes _roundtrip.bmp)
// Then:                node web/test_decode.js _roundtrip.bmp
import fs from 'node:fs';
import { decodeBeacon } from './decode.js';

const path = process.argv[2] || '../_roundtrip.bmp';
const data = new Uint8Array(fs.readFileSync(path));
const r = await decodeBeacon(data);          // BMP or PNG, auto-detected
console.log(`decoded ${r.count} network(s), CRC OK`);
for (const n of r.networks) console.log(' ', n);
