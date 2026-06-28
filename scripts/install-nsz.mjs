#!/usr/bin/env node
// Install a compressed NSZ to a sys-autopilot console by decompressing it on
// the host and streaming a reconstructed NSP to the /install endpoint.
//
// The sysmodule itself only understands plain NSP (uncompressed NCAs); doing
// the zstd decompression + AES-CTR re-encryption on-device would force a
// multi-MB zstd window to live permanently in an otherwise tiny sysmodule, so
// we do it here instead. The reconstructed NSP is streamed (never buffered to
// disk), and the title id's content hashes are still verified by the console
// as each NCA is written.
//
// NSZ structure: a PFS0 (same container as NSP) whose content files are ".ncz"
// (compressed NCA) instead of ".nca". Non-.ncz entries (plain .nca, .cnmt.nca,
// .tik, .cert) pass through unchanged. Each .ncz becomes a ".nca" of a size we
// can compute from its header WITHOUT decompressing, which lets us build the
// output PFS0 layout and Content-Length up front (the /install endpoint
// requires Content-Length and reads the body as a plain stream).
//
// Usage:
//   node scripts/install-nsz.mjs <file.nsz> <console-url> [options]
//
// Options:
//   --storage sd|nand   target storage (default: sd)
//   --user <user>       HTTP Basic auth username
//   --pass <pass>       HTTP Basic auth password
//   --netrc             read credentials from ~/.netrc for the host
//
// Example:
//   node scripts/install-nsz.mjs "Game [0100..].nsz" http://192.168.1.249:4150 --netrc

import { readFile } from 'node:fs/promises';
import { openSync, readSync, fstatSync, closeSync, createReadStream } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { Buffer } from 'node:buffer';
import { PassThrough, Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';

import { decode as decodePfs0 } from '@tootallnate/pfs0';
import { decompressNcz, isNcz, parseNczHeader } from '@tootallnate/ncz';
import { ZstdDecompressStream, decompressBytes } from '@tootallnate/zstd-wasm';

// --- args --------------------------------------------------------------------
function parseArgs(argv) {
	const pos = [];
	const opt = { storage: 'sd' };
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === '--storage') opt.storage = argv[++i];
		else if (a === '--user') opt.user = argv[++i];
		else if (a === '--pass') opt.pass = argv[++i];
		else if (a === '--netrc') opt.netrc = true;
		else if (a === '--dry-run') opt.dryRun = true;
		else if (a.startsWith('--')) die(`unknown option: ${a}`);
		else pos.push(a);
	}
	if (pos.length < 1 || (!opt.dryRun && pos.length < 2)) {
		die('usage: install-nsz.mjs <file.nsz> <console-url> [--storage sd|nand] [--user U --pass P | --netrc] [--dry-run]');
	}
	opt.file = pos[0];
	opt.url = pos[1];
	return opt;
}

function die(msg) {
	console.error(`error: ${msg}`);
	process.exit(1);
}

// Minimal ~/.netrc parser: returns { login, password } for the given host.
async function readNetrc(host) {
	let text;
	try {
		text = await readFile(join(homedir(), '.netrc'), 'utf8');
	} catch {
		return null;
	}
	const tokens = text.split(/\s+/);
	let cur = null;
	const machines = {};
	for (let i = 0; i < tokens.length; i++) {
		const t = tokens[i];
		if (t === 'machine') { cur = tokens[++i]; machines[cur] = {}; }
		else if (t === 'default') { cur = '__default__'; machines[cur] = {}; }
		else if (cur && (t === 'login' || t === 'password' || t === 'account')) {
			machines[cur][t] = tokens[++i];
		}
	}
	return machines[host] || machines['__default__'] || null;
}

// --- PFS0 output layout ------------------------------------------------------
function align(n, a) {
	const r = n % a;
	return r === 0 ? n : n + a - r;
}

// Build the output PFS0 header bytes and per-file plan from the ordered output
// entries [{ name, size(bigint), blob, ncz(bool) }].
function buildPfs0Header(entries) {
	const count = entries.length;
	const headerSize = 0x10;
	const tableSize = count * 0x18;
	let strtab = 0;
	const nameBufs = entries.map((e) => Buffer.from(e.name + '\0', 'utf8'));
	const nameOffsets = [];
	for (const nb of nameBufs) { nameOffsets.push(strtab); strtab += nb.length; }
	const strtabSize = align(strtab, 0x10);

	const buf = Buffer.alloc(headerSize + tableSize + strtabSize);
	buf.write('PFS0', 0, 'ascii');
	buf.writeUInt32LE(count, 4);
	buf.writeUInt32LE(strtabSize, 8);
	buf.writeUInt32LE(0, 12);

	let dataOff = 0n;
	for (let i = 0; i < count; i++) {
		const e = entries[i];
		const o = headerSize + i * 0x18;
		buf.writeBigUInt64LE(dataOff, o);
		buf.writeBigUInt64LE(e.size, o + 8);
		buf.writeUInt32LE(nameOffsets[i], o + 16);
		buf.writeUInt32LE(0, o + 20);
		dataOff += e.size;
	}
	let p = headerSize + tableSize;
	for (const nb of nameBufs) { nb.copy(buf, p); p += nb.length; }
	return { headerBuf: buf, bodySize: dataOff };
}

// --- main --------------------------------------------------------------------
async function main() {
	const opt = parseArgs(process.argv.slice(2));

	// Resolve auth.
	let auth = null;
	if (opt.user != null || opt.pass != null) {
		auth = { user: opt.user ?? '', pass: opt.pass ?? '' };
	} else if (opt.netrc) {
		const host = new URL(opt.url).hostname;
		const nc = await readNetrc(host);
		if (!nc?.login) die(`no ~/.netrc entry for host ${host}`);
		auth = { user: nc.login, pass: nc.password ?? '' };
	}

	// Read the NSZ as a Blob-like via a file handle wrapper.
	const fd = openSync(opt.file, 'r');
	const fileSize = fstatSync(fd).size;
	const nszBlob = makeFileBlob(fd, fileSize, opt.file);
	console.log(`nsz: ${opt.file} (${mb(fileSize)})`);

	// 1. Decode the PFS0 container.
	const pfs0 = await decodePfs0(nszBlob);

	// 2. Build the ordered output entry plan (.ncz -> .nca, others unchanged).
	const entries = [];
	for (const [name, blob] of pfs0) {
		if (await isNcz(blob)) {
			const outName = name.replace(/\.ncz$/i, '.nca');
			const { ncaSize } = await parseNczHeader(blob);
			entries.push({ name: outName, size: ncaSize, blob, ncz: true });
			console.log(`  ${name} -> ${outName}  ${mb(blob.size)} -> ${mb(ncaSize)} (decompress)`);
		} else {
			entries.push({ name, size: BigInt(blob.size), blob, ncz: false });
			console.log(`  ${name}  ${mb(blob.size)} (passthrough)`);
		}
	}

	// 3. Compute the output PFS0 header + total Content-Length.
	const { headerBuf, bodySize } = buildPfs0Header(entries);
	const contentLength = BigInt(headerBuf.length) + bodySize;
	console.log(`reconstructed nsp: ${mb(contentLength)} (Content-Length=${contentLength})`);

	const wasm = await readFile(new URL(import.meta.resolve('@tootallnate/zstd-wasm/zstd.wasm')));

	// Progress reporter (throttled to stderr).
	const total = Number(contentLength);
	let sent = 0;
	let lastPct = -1;
	const onProgress = (n) => {
		sent += n;
		const pct = Math.floor((sent / total) * 100);
		if (pct !== lastPct) {
			lastPct = pct;
			process.stderr.write(`\r  ${opt.dryRun ? 'decompressing' : 'uploading'} ${pct}% (${mb(sent)}/${mb(total)})   `);
		}
	};

	// Dry run: pump the whole reconstructed NSP into a counting sink and confirm
	// the produced byte count matches the predicted Content-Length. Exercises
	// the full zstd + AES-CTR re-encrypt pipeline without touching a console.
	if (opt.dryRun) {
		console.log('dry-run: decompressing + verifying sizes (no upload) ...');
		let produced = 0n;
		const counter = new PassThrough();
		counter.on('data', (c) => { produced += BigInt(c.length); });
		try {
			await pumpNsp(counter, headerBuf, entries, wasm, onProgress);
		} catch (err) {
			process.stderr.write('\n');
			console.error('dry-run error:', err?.stack || err);
			closeSync(fd);
			process.exitCode = 1;
			return;
		}
		closeSync(fd);
		const ok = produced === contentLength;
		process.stderr.write('\n');
		console.log(
			`dry-run: produced ${produced} bytes, expected ${contentLength} -> ${ok ? 'PASS' : 'MISMATCH'}`,
		);
		process.exitCode = ok ? 0 : 1;
		return;
	}

	// 4. Build the NSP body as a Node stream and PUT it. We pump into a
	//    PassThrough (real async backpressure) and hand its web-stream view to
	//    fetch as the request body.
	const pass = new PassThrough();
	const pumpDone = pumpNsp(pass, headerBuf, entries, wasm, onProgress).catch((err) => {
		pass.destroy(err);
	});

	const url = new URL('/install', opt.url);
	if (opt.storage && opt.storage !== 'sd') url.searchParams.set('storage', opt.storage);
	const headers = {
		'content-type': 'application/octet-stream',
		'content-length': contentLength.toString(),
	};
	if (auth) {
		headers.authorization =
			'Basic ' + Buffer.from(`${auth.user}:${auth.pass}`).toString('base64');
	}

	console.log(`installing to ${url} (storage=${opt.storage}) ...`);
	const res = await fetch(url, {
		method: 'PUT',
		headers,
		body: Readable.toWeb(pass),
		duplex: 'half',
	});
	const text = await res.text();
	await pumpDone;
	process.stderr.write('\n');
	console.log(`HTTP ${res.status}: ${text}`);
	closeSync(fd);
	process.exit(res.ok ? 0 : 1);
}

// Write the whole reconstructed NSP into the Node Writable `dest`, in order:
// the PFS0 header, then each entry's body. `.ncz` entries are decompressed +
// re-encrypted on the fly; other entries are copied verbatim from the source
// file. Backpressure is honored throughout (Node streams), so memory stays
// bounded even for multi-GB titles. Resolves when fully written (dest ended).
async function pumpNsp(dest, headerBuf, entries, wasm, onProgress) {
	// Helper: write a Buffer/Uint8Array respecting backpressure.
	const write = (chunk) =>
		new Promise((resolve, reject) => {
			const ok = dest.write(chunk, (err) => (err ? reject(err) : undefined));
			onProgress?.(chunk.length);
			if (ok) resolve();
			else dest.once('drain', resolve);
		});

	await write(headerBuf);

	for (const e of entries) {
		if (e.ncz) {
			// decompressNcz writes the reconstructed NCA into a WritableStream.
			// Bridge it to `dest`: each chunk is written with backpressure, and
			// we DON'T end `dest` (more entries may follow).
			await decompressNcz(
				e.blob,
				() =>
					new WritableStream({
						write(chunk) {
							onProgress?.(chunk.length);
							return new Promise((resolve, reject) => {
								const ok = dest.write(chunk, (err) =>
									err ? reject(err) : undefined,
								);
								if (ok) resolve();
								else dest.once('drain', resolve);
							});
						},
					}),
				{
					decompressStream: (input) =>
						input.pipeThrough(new ZstdDecompressStream(wasm)),
					decompressBytes: (buf) => decompressBytes(wasm, buf),
				},
			);
		} else {
			// Passthrough: stream the original bytes straight from the source
			// file at the entry's absolute offset. A counting PassThrough in the
			// middle reports progress without adding a second data consumer.
			const rs = createReadStream(e.blob._path, {
				start: e.blob._start,
				end: e.blob._end - 1, // inclusive
			});
			const counter = new PassThrough();
			counter.on('data', (c) => onProgress?.(c.length));
			await pipeline(rs, counter, dest, { end: false });
		}
	}

	// All entries written — end the destination.
	await new Promise((resolve, reject) => {
		dest.end((err) => (err ? reject(err) : resolve()));
	});
}

// --- helpers -----------------------------------------------------------------
// A minimal Blob-like over a file descriptor: supports .size, .slice(),
// .arrayBuffer() and .stream() — all that decodePfs0/isNcz/decompressNcz need.
// `_path`/`_start`/`_end` are exposed so passthrough entries can be streamed
// efficiently with fs.createReadStream (async IO, proper backpressure).
function makeFileBlob(fd, size, path, start = 0, end = size) {
	const len = end - start;
	return {
		size: len,
		_path: path,
		_start: start,
		_end: end,
		slice(s = 0, e = len) {
			const ns = start + s;
			const ne = start + Math.min(e, len);
			return makeFileBlob(fd, size, path, ns, ne);
		},
		async arrayBuffer() {
			const b = Buffer.alloc(len);
			if (len > 0) readSync(fd, b, 0, len, start);
			return b.buffer.slice(b.byteOffset, b.byteOffset + b.length);
		},
		stream() {
			let pos = start;
			const CHUNK = 1 << 20;
			return new ReadableStream({
				pull(controller) {
					if (pos >= end) { controller.close(); return; }
					const n = Math.min(CHUNK, end - pos);
					const b = Buffer.alloc(n);
					readSync(fd, b, 0, n, pos);
					pos += n;
					controller.enqueue(new Uint8Array(b));
				},
			});
		},
	};
}

function mb(n) {
	return (Number(n) / 1048576).toFixed(1) + ' MB';
}

main().catch((err) => {
	process.stderr.write('\n');
	die(err?.stack || String(err));
});
