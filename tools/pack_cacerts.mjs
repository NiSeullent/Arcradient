#!/usr/bin/env bun
/**
 * Arcradient (MSHTML-Next) - tools/pack_cacerts.mjs
 *
 * Dev-only build utility (runs on Bun or Node.js; ZERO runtime dependency in shipped C binaries).
 * Packs PEM Root CA certificates into an indexed binary trust store (`arcr-cacerts.bin`)
 * for `arcr-net`.
 */
import { readFileSync, writeFileSync } from "node:fs";

const MAGIC = 0x41524341; // "ARCA"

export function packPemBundle(pemText) {
  const blocks = [];
  const regex = /-----BEGIN CERTIFICATE-----([\s\S]*?)-----END CERTIFICATE-----/g;
  let match;
  while ((match = regex.exec(pemText)) !== null) {
    const b64 = match[1].replace(/\s+/g, "");
    blocks.push(Buffer.from(b64, "base64"));
  }

  let totalBytes = 8 + blocks.length * 8;
  for (const der of blocks) totalBytes += der.length;

  const out = Buffer.alloc(totalBytes);
  out.writeUInt32LE(MAGIC, 0);
  out.writeUInt32LE(blocks.length, 4);

  let dataOffset = 8 + blocks.length * 8;
  blocks.forEach((der, idx) => {
    out.writeUInt32LE(dataOffset, 8 + idx * 8);
    out.writeUInt32LE(der.length, 12 + idx * 8);
    der.copy(out, dataOffset);
    dataOffset += der.length;
  });
  return { count: blocks.length, buffer: out };
}

if (process.argv[2] && process.argv[3]) {
  const pem = readFileSync(process.argv[2], "utf8");
  const { count, buffer } = packPemBundle(pem);
  writeFileSync(process.argv[3], buffer);
  console.log(`[arcr-tools] Packed ${count} Root CA certificates (${buffer.length} bytes) -> ${process.argv[3]}`);
}
