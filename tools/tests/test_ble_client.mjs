// Unit tests for the protocol codec inside tools/ble_client.html.
// Run from the repository root:  node --test tools/tests/test_ble_client.mjs
// The codec section (between "==== BEGIN CODEC ====" and "==== END CODEC ====") is evaluated
// as-is. Vectors come from hand-built byte arrays (fw/include/*.h) and, when
// .venv/bin/python exists, from tools/ecgcard/codec.py and record.py.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const html = readFileSync(join(root, "tools", "ble_client.html"), "utf8");
const a = html.indexOf("// ==== BEGIN CODEC ====");
const b = html.indexOf("// ==== END CODEC ====");
assert.ok(a > 0 && b > a, "codec markers not found");
const C = new Function(html.slice(a, b) + "\nreturn Codec;")();

const bytes = (...v) => Uint8Array.from(v.flat(Infinity));
const dv = (u8) => new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
const fromHex = (h) => Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16)));
const le16 = (v) => [v & 0xff, (v >> 8) & 0xff];
const le32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const close = (x, y, eps = 1e-9) => assert.ok(Math.abs(x - y) <= eps * Math.max(1, Math.abs(y)), `${x} != ${y}`);

test("UUIDs match link_fmt.h", () => {
  assert.equal(C.SERVICE_UUID, "d5780001-8a28-493a-ad8d-6b741ba09ea7");
  assert.equal(C.CHAR_UUID.ECG, "d5780002-8a28-493a-ad8d-6b741ba09ea7");
  assert.equal(C.CHAR_UUID.CONTROL, "d5780006-8a28-493a-ad8d-6b741ba09ea7");
  // LINK_UUID128 little-endian bytes reversed give the string form
  const le = [0xa7, 0x9e, 0xa0, 0x1b, 0x74, 0x6b, 0x8d, 0xad, 0x3a, 0x49, 0x28, 0x8a, 0x03, 0x00, 0x78, 0xd5];
  const s = le.slice().reverse().map((x) => x.toString(16).padStart(2, "0")).join("");
  assert.equal(`${s.slice(0, 8)}-${s.slice(8, 12)}-${s.slice(12, 16)}-${s.slice(16, 20)}-${s.slice(20)}`, C.CHAR_UUID.PPG);
});

test("ECG block: layout and uV scaling at both gains", () => {
  const s = [0, 1, -1, 32767, -32768, 1000, -1000, 8, -8, 12345];
  for (const [flags, gain] of [[0x00, 558], [0x01, 1098], [0x07, 1098], [0x06, 558]]) {
    const raw = bytes(le32(0x12345678), flags, s.map(le16));
    assert.equal(raw.length, 25);
    const out = C.newEcgBlock();
    assert.equal(C.parseEcg(dv(raw), out), null);
    assert.equal(out.start, 0x12345678);
    assert.equal(out.flags, flags);
    assert.deepEqual(Array.from(out.s), s);
    assert.equal(C.ecgGain(flags), gain);
    close(1000 * C.ecgUvPerUnit(flags), 1000 * 805.7 / (8 * gain));
  }
  // 1 mV at gain 1098: 1000 uV * 8 * 1098 / 805.7 stream units
  close(Math.round(1000 * 8 * 1098 / 805.7) * C.ecgUvPerUnit(1), 1000, 1e-3);
  const start = C.newEcgBlock();
  assert.equal(C.parseEcg(dv(bytes(le32(0xffffffff), 0, new Array(20).fill(0))), start), null);
  assert.equal(start.start, 0xffffffff);
});

test("length validation rejects every wrong size", () => {
  const e = C.newEcgBlock(), p = C.newPpgBlock(), ev = C.newEvent();
  for (const n of [0, 1, 24, 26, 244]) assert.match(C.parseEcg(dv(new Uint8Array(n)), e), /ECG payload/);
  for (const n of [0, 14, 16]) assert.match(C.parsePpg(dv(new Uint8Array(n)), p), /PPG payload/);
  assert.equal(C.parseEvent(dv(new Uint8Array(0)), ev), "empty event");
  for (const [kind, len] of Object.entries(C.EV_LEN)) {
    for (const n of [len - 1, len + 1]) {
      const u = new Uint8Array(n); u[0] = +kind;
      assert.match(C.parseEvent(dv(u), ev), /expected/);
    }
  }
  for (const n of [0, 19, 21]) assert.ok(C.parseStatus(dv(new Uint8Array(n))).err);
  assert.ok(C.parseBattery(dv(new Uint8Array(2))).err);
  assert.ok(C.parseHrm(dv(new Uint8Array(1))).err);
});

test("PPG block: layout, signed coarse, wrapped fine", () => {
  const raw = bytes(le32(4242), le16(600), 0x05, le16(-12 & 0xffff), le16(0x8000), le16(3000), le16(0x7fff));
  const out = C.newPpgBlock();
  assert.equal(C.parsePpg(dv(raw), out), null);
  assert.equal(out.start, 4242); assert.equal(out.led, 600); assert.equal(out.flags, 5);
  assert.deepEqual(Array.from(out.coarse), [-12, 3000]);
  assert.deepEqual(Array.from(out.fine), [-32768, 32767]);
});

test("Unwrap16 follows the smallest step across +-32768", () => {
  const u = new C.Unwrap16();
  // true series crossing the int16 range both ways
  const truth = [32000, 32700, 32767, 32768, 33000, 40000, 30000, 10000, -12000, -32000, -32768, -32769, -40000, -20000, -1000, 0];
  const got = truth.map((v) => u.push(C.wrapI16(v)));
  // differences between neighbours are < 32768 in this series, so the unwrap is exact
  assert.deepEqual(got, truth);
  u.reset();
  assert.equal(u.push(-5), -5);
  assert.equal(C.diffI16(-32768, 32767), 1);
  assert.equal(C.diffI16(32767, -32768), -1);
});

test("Continuity: OK, gap, duplicate, late, new run, u32 wrap", () => {
  const K = C.CONT, c = new C.Continuity(1000);
  assert.equal(c.push(100, 10), K.FIRST); assert.equal(c.uStart, 100);
  assert.equal(c.push(110, 10), K.OK); assert.equal(c.uStart, 110);
  assert.equal(c.push(140, 10), K.GAP); assert.equal(c.missing, 20); assert.equal(c.uStart, 140);
  assert.equal(c.push(140, 10), K.BACK); assert.equal(c.behind, 10); assert.equal(c.uStart, 140);
  assert.equal(c.push(120, 10), K.BACK); assert.equal(c.behind, 30); assert.equal(c.uStart, 120);
  assert.equal(c.push(150, 10), K.OK);                       // expectation unchanged by BACK
  assert.equal(c.push(0, 10), K.NEWRUN); assert.equal(c.run, 1); assert.equal(c.uStart, 0);
  assert.equal(c.push(10, 10), K.OK);
  // far backwards without 0 is also a restart
  const d = new C.Continuity(1000);
  d.push(50000, 10); assert.equal(d.push(20, 10), K.NEWRUN);
  // u32 wrap: contiguous across 0xFFFFFFFF -> 0, unwrapped index keeps counting
  const w = new C.Continuity(1000);
  assert.equal(w.push(0xfffffff6, 10), K.FIRST);
  assert.equal(w.push(0, 10), K.OK); assert.equal(w.uStart, 0x100000000);
  assert.equal(w.push(30, 10), K.GAP); assert.equal(w.missing, 20); assert.equal(w.uStart, 0x100000000 + 30);
  const w2 = new C.Continuity(1000);
  w2.push(0xfffffffe, 2); assert.equal(w2.push(4, 2), K.GAP); assert.equal(w2.missing, 4); // 0,1,2,3 missing
  // PPG step of 2 frames
  const p = new C.Continuity(200);
  p.push(0, 2); p.push(2, 2); assert.equal(p.push(6, 2), K.GAP); assert.equal(p.missing, 2);
});

test("events: every kind from hand-built bytes", () => {
  const e = C.newEvent(), P = (u) => { assert.equal(C.parseEvent(dv(u), e), null); return C.eventFields(e); };
  assert.deepEqual(P(bytes(1, le32(123456), le16(72))), { sample: 123456, hr_bpm: 72 });
  assert.deepEqual(P(bytes(2, le32(0x89abcdef), le16(71), le16(212), le16(13))), { foot_sample: 0x89abcdef, pulse_bpm: 71, pat_ms: 212, pi_permille: 13 });
  assert.deepEqual(P(bytes(3, 1)), { contact: 1 });
  assert.deepEqual(P(bytes(4, 3, 60, le16(2048), 77, 3)), { mode: 3, notch_hz: 60, mid_code: 2048, batt_pct: 77, flags: 3 });
  assert.deepEqual(P(bytes(5, 2, 1)), { button: 2, kind: 1 });
  assert.deepEqual(P(bytes(6, 3, 2)), { cmd: 3, result: 2 });
  assert.match(C.describeEvent(e), /SET: BAD_ARG/);
  assert.equal(C.parseEvent(dv(bytes(0x7e, 1, 2, 3)), e), null);
  assert.equal(e.known, false); assert.equal(e.kind, 0x7e);
  for (const r of [0, 1, 2, 3, 4, 5, 6]) assert.ok(C.RES_NAME[r]);
});

test("Status: all 20 bytes", () => {
  const raw = bytes(1, 3, 2, 87, 0x41, 2, 1, 3, 1, 2, 50, 255, le16(3915), 0, 4, 12, 0, le16(1234));
  assert.equal(raw.length, 20);
  const s = C.parseStatus(dv(raw));
  assert.equal(s.err, null);
  assert.deepEqual([s.proto, s.mode, s.meas, s.battPct, s.flags, s.gain, s.notch, s.rld, s.ledLimit, s.sweep, s.notchHz, s.hrBpm, s.battMv, s.fw, s.uptimeMin],
    [1, 3, 2, 87, 0x41, 2, 1, 3, 1, 2, 50, 255, 3915, "0.4.12", 1234]);
  assert.equal(C.statusFlagsText(0x41), "USB, time valid");
  assert.equal(C.MEAS_NAME[s.meas], "running");
  assert.deepEqual(Array.from(C.encodeStatus(s)), Array.from(raw));
});

test("Heart Rate Measurement and Battery Level", () => {
  assert.equal(C.parseHrm(dv(bytes(0x00, 72))).bpm, 72);
  const h = C.parseHrm(dv(bytes(0x1f, le16(300), le16(5), le16(1024), le16(512))));
  assert.equal(h.bpm, 300); assert.equal(h.energy, 5); assert.deepEqual(h.rr, [1, 0.5]); assert.equal(h.contact, 1);
  assert.equal(C.parseBattery(dv(bytes(64))).pct, 64);
});

test("command encoders (LINK_CMD payloads)", () => {
  assert.deepEqual(Array.from(C.cmdStart(3)), [1, 3]);
  assert.deepEqual(Array.from(C.cmdStop()), [2]);
  assert.deepEqual(Array.from(C.cmdSet(4, 2)), [3, 4, 2]);
  assert.deepEqual(Array.from(C.cmdGetStatus()), [5]);
  assert.deepEqual(Array.from(C.cmdSetTime(0x6543210f, -420)), [4, 0x0f, 0x21, 0x43, 0x65, ...le16(-420 & 0xffff)]);
  assert.deepEqual(Array.from(C.cmdSetTime(1, 330)), [4, 1, 0, 0, 0, 330 & 0xff, 330 >> 8]);
  const [sec, off] = C.nowSetTimeArgs(new Date(Date.UTC(2026, 10, 12, 23, 0, 5)));
  assert.equal(sec, Date.UTC(2026, 10, 12, 23, 0, 5) / 1000);
  assert.equal(off, -new Date(Date.UTC(2026, 10, 12, 23, 0, 5)).getTimezoneOffset()); // local offset, minutes east of UTC
  assert.equal(C.describeCommand(C.cmdSet(1, 2)), "SET notch = 60 Hz");
  assert.equal(C.describeCommand(C.cmdStart(2)), "START PPG");
  for (const b of [C.cmdStart(1), C.cmdStop(), C.cmdSet(0, 0), C.cmdSetTime(0, 0), C.cmdGetStatus()]) assert.ok(b.length >= 1 && b.length <= C.CMD_MAX_LEN);
  // settings.h enum sizes, item numbers and the items refused (BUSY) during a session
  assert.deepEqual(C.SETTINGS.map((s) => s.options.length), [3, 3, 4, 3, 3, 2]);
  assert.deepEqual(C.SETTINGS.map((s) => s.item), [0, 1, 2, 3, 4, 5]);
  assert.deepEqual(C.SETTINGS.filter((s) => s.lockedInSession).map((s) => s.key), ["gain", "notch", "rld", "led_limit"]);
  assert.equal(C.SET_ITEM_BLE, 5);
  assert.deepEqual(Array.from(C.cmdSet(C.SET_ITEM_BLE, C.BLE_OFF)), [3, 5, 0]);
  assert.equal(C.describeCommand(C.cmdSet(5, 1)), "SET ble = on");
});

test("SET_TIME limits and device-name digits (link_fmt.h clarifications)", () => {
  assert.equal(C.TIME_MIN_UNIX, Date.UTC(2024, 0, 1) / 1000);
  assert.equal(C.setTimeProblem(C.TIME_MIN_UNIX, 0), null);
  assert.match(C.setTimeProblem(C.TIME_MIN_UNIX - 1, 0), /2024/);
  assert.equal(C.setTimeProblem(1.8e9, -720), null);
  assert.equal(C.setTimeProblem(1.8e9, 840), null);
  assert.match(C.setTimeProblem(1.8e9, -721), /offset/);
  assert.match(C.setTimeProblem(1.8e9, 841), /offset/);
  assert.equal(C.nameSuffixOfAddress("A1:B2:C3:D4:E5:F6"), "E5F6");
  assert.equal(C.nameSuffixOfAddress("a1b2c3d4e5f6"), "E5F6");
  assert.equal(C.nameSuffixOfAddress("nope"), null);
  assert.equal(C.NOTIFY_MAX_LEN, 32);
  assert.ok(C.ECG_LEN <= 32 && C.PPG_LEN <= 32 && C.STATUS_LEN <= 32 && Math.max(...Object.values(C.EV_LEN)) <= 32);
});

test("timeline matches record.py", () => {
  close(C.ecgTime(0), -0.010); close(C.ecgTime(500), 0.990);
  close(C.ppgTime(0), 0.00995); close(C.ppgTime(100), 1.00995);
  close(C.ecgIndexAt(C.ecgTime(1234)), 1234); close(C.ppgIndexAt(C.ppgTime(77)), 77);
});

test("encoders round-trip through the parsers", () => {
  const e = C.newEcgBlock(), p = C.newPpgBlock();
  assert.equal(C.parseEcg(dv(C.encodeEcg(10, 1, [1, 2, 3, 4, 5, 6, 7, 8, 9, 40000])), e), null);
  assert.equal(e.s[9], 32767);                               // clamped
  assert.equal(C.parsePpg(dv(C.encodePpg(8, 5, 2, [1, -2], [-33000, 40000])), p), null);
  assert.deepEqual(Array.from(p.fine), [C.wrapI16(-33000), C.wrapI16(40000)]);
});

test("capture file round trip", () => {
  const hdr = C.encodeCaptureHeader({ format: "ecgcard-ble-capture", version: 1, t0_unix_ms: 5 });
  const buf = new Uint8Array(200);
  let o = C.writeCaptureRecord(buf, 0, 0x0002, 12.5, bytes(1, 2, 3));
  o = C.writeCaptureRecord(buf, o, 0xffff, 1e6, new TextEncoder().encode("hi"));
  const all = new Uint8Array(hdr.length + o); all.set(hdr); all.set(buf.subarray(0, o), hdr.length);
  assert.equal(new TextDecoder().decode(all.subarray(0, 8)), "ECGCAP01");
  const r = C.parseCapture(all);
  assert.equal(r.err, null); assert.equal(r.header.t0_unix_ms, 5);
  assert.deepEqual(r.records.map((x) => [x.ch, x.tMs, Array.from(x.payload)]), [[2, 12.5, [1, 2, 3]], [0xffff, 1e6, [104, 105]]]);
  assert.equal(C.parseCapture(all.subarray(0, all.length - 1)).err, "record truncated");
});

test("display filters: high-pass removes DC, notch removes mains", () => {
  const hp = new C.HighPass1(0.5, 500);
  let y = 0; for (let i = 0; i < 5000; i++) y = hp.step(1000);
  assert.ok(Math.abs(y) < 1e-3);
  const f = new C.EcgDisplayFilter("bpn", 50);
  let peak = 0;
  for (let i = 0; i < 5000; i++) { const v = f.step(100 * Math.sin(2 * Math.PI * 50 * i / 500)); if (i > 4000) peak = Math.max(peak, Math.abs(v)); }
  assert.ok(peak < 2, `notch residue ${peak}`);
});

// ---- device log (Log characteristic 0x0007) ----
const enc = (s) => new TextEncoder().encode(s);
/** Splits bytes into notification-sized chunks (<= 32 B), like the radio. */
const chunks = (u8, n = 32) => { const out = []; for (let i = 0; i < u8.length; i += n) out.push(u8.slice(i, i + n)); return out; };
const ECG_LINE = "[52310] ecg: wait rail=68% min=0 max=4086 avg=1495 fr=3 beats=0 hr=0 mains=4917 lost=4 rs=0";
const PPG_LINE = "[52311] ppg: on led=1344 dac=1290 dk=304 on=1225 hi=1944/1869-2046 c=921 agc=0 dcc=1 p=21 pr=73 lost=0";

test("Log characteristic UUID and name (link_fmt.h LINK_UUID_LOG 0x0007)", () => {
  assert.equal(C.CHAR.LOG, 0x0007);
  assert.equal(C.CHAR_UUID.LOG, "d5780007-8a28-493a-ad8d-6b741ba09ea7");
  assert.equal(C.uuid(C.CHAR.LOG), C.CHAR_UUID.LOG);
  assert.equal(C.CHAR_NAME[C.CHAR.LOG], "Log");
});

test("line assembler: lines split across notifications, several per chunk", () => {
  const a = C.newLineAssembler();
  const stream = enc(ECG_LINE + "\n" + PPG_LINE + "\nradio: ble: sent 12\n");
  const got = [];
  const parts = chunks(stream);
  assert.ok(parts.length >= 7 && parts.every((p) => p.length <= 32));
  for (const p of parts) got.push(...a.push(p));
  assert.deepEqual(got, [ECG_LINE, PPG_LINE, "radio: ble: sent 12"]);
  assert.equal(a.pending, "");
  // a single chunk ending two lines and starting a third; a DataView and an ArrayBuffer work too
  const b = C.newLineAssembler();
  assert.deepEqual(b.push(enc("ab")), []);
  assert.deepEqual(b.push(dv(enc("c\nd\ne"))), ["abc", "d"]);
  assert.equal(b.pending, "e");
  assert.deepEqual(b.push(enc("f\n").buffer), ["ef"]);
  // strings are accepted (demo / tests)
  assert.deepEqual(b.push("x\ny\n"), ["x", "y"]);
});

test("line assembler: CRLF, empty and blank lines, flush", () => {
  const a = C.newLineAssembler();
  assert.deepEqual(a.push(enc("one\r\n\n\r\n   \ntwo\r")), ["one"]);
  assert.deepEqual(a.push(enc("\nthree")), ["two"]);
  assert.deepEqual(a.flush(), ["three"]);
  assert.deepEqual(a.flush(), []);
  // a cut-short line (best-effort log) is still a line
  assert.deepEqual(a.push(enc("[100] ecg: run rail=0% mi\n[101] ppg: on\n")), ["[100] ecg: run rail=0% mi", "[101] ppg: on"]);
  a.push(enc("partial")); a.reset();
  assert.deepEqual(a.push(enc("new\n")), ["new"]);
});

test("line assembler: a UTF-8 character split across notifications", () => {
  const s = "radio: temp 23°C µV € \u{1f600} ok";
  const u8 = enc(s + "\n");
  for (let cut = 1; cut < u8.length - 1; cut++) {
    const a = C.newLineAssembler();
    const got = [...a.push(u8.slice(0, cut)), ...a.push(u8.slice(cut))];
    assert.deepEqual(got, [s], `cut at ${cut}`);
  }
  // byte by byte
  const a = C.newLineAssembler(), got = [];
  for (const byte of u8) got.push(...a.push(Uint8Array.of(byte)));
  assert.deepEqual(got, [s]);
});

test("line assembler: runaway partial line is flushed at the cap", () => {
  const a = C.newLineAssembler(16), got = [];
  for (const p of chunks(enc("x".repeat(40)), 7)) got.push(...a.push(p));
  assert.deepEqual(got, ["x".repeat(16), "x".repeat(16)]);
  assert.equal(a.pending, "x".repeat(8));
  assert.deepEqual(a.push(enc("\n")), ["x".repeat(8)]);
  // a complete line longer than the cap comes out in pieces; exactly the cap is one line
  assert.deepEqual(C.newLineAssembler(4).push("abcdefghij\nabcd\n"), ["abcd", "efgh", "ij", "abcd"]);
  // default cap
  assert.equal(C.LOG_LINE_MAX, 512);
  const d = C.newLineAssembler(), big = [];
  for (const p of chunks(enc("y".repeat(1300)))) big.push(...d.push(p));
  assert.deepEqual(big.map((l) => l.length), [512, 512]);
  assert.equal(d.pending.length, 1300 - 1024);
});

test("parseLogLine: ecg evidence line", () => {
  const r = C.parseLogLine(ECG_LINE);
  assert.equal(r.t_ms, 52310); assert.equal(r.src, "mcu"); assert.equal(r.tag, "ecg"); assert.equal(r.kind, "ecg");
  assert.equal(r.state, "wait"); assert.equal(r.evidence, true); assert.equal(r.transition, false); assert.equal(r.nodata, false);
  assert.deepEqual(r.fields, { rail: 68, min: 0, max: 4086, avg: 1495, fr: 3, beats: 0, hr: 0, mains: 4917, lost: 4, rs: 0 });
  assert.equal(r.text, "wait rail=68% min=0 max=4086 avg=1495 fr=3 beats=0 hr=0 mains=4917 lost=4 rs=0");
});

test("parseLogLine: ppg evidence line with hi=mean/min-max and negative values", () => {
  const r = C.parseLogLine(PPG_LINE);
  assert.equal(r.t_ms, 52311); assert.equal(r.kind, "ppg"); assert.equal(r.state, "on"); assert.equal(r.evidence, true);
  assert.deepEqual(r.fields, { led: 1344, dac: 1290, dk: 304, on: 1225, hi: 1944, hi_min: 1869, hi_max: 2046, c: 921, agc: 0, dcc: 1, p: 21, pr: 73, lost: 0 });
  const n = C.parseLogLine("[7] ppg: set led=10 dac=0 dk=900 on=880 hi=-5/-30-12 c=-20 agc=2 dcc=0 p=0 pr=0 lost=0");
  assert.equal(n.fields.c, -20);
  assert.deepEqual([n.fields.hi, n.fields.hi_min, n.fields.hi_max], [-5, -30, 12]);
  assert.equal(n.state, "set");
});

test("parseLogLine: no samples / no frames, unknown and reordered fields", () => {
  const e = C.parseLogLine("[60000] ecg: settle no samples fr=0 beats=0 hr=0 mains=0 lost=0 rs=1");
  assert.equal(e.state, "settle"); assert.equal(e.nodata, true); assert.equal(e.evidence, true);
  assert.equal(e.fields.rail, undefined); assert.equal(e.fields.min, undefined); assert.equal(e.fields.rs, 1);
  const p = C.parseLogLine("[60001] ppg: off led=0 dac=0 no frames agc=0 dcc=0 p=0 pr=0 lost=7");
  assert.equal(p.state, "off"); assert.equal(p.nodata, true); assert.equal(p.fields.lost, 7); assert.equal(p.fields.dk, undefined);
  const u = C.parseLogLine("[5] ecg: run mains=12 future=3.5 rail=2% mode=fast beats=9");
  assert.deepEqual(u.fields, { mains: 12, future: 3.5, rail: 2, mode: "fast", beats: 9 });
  assert.equal(u.state, "run"); assert.equal(u.evidence, true);
  // a missing state word: fields still parse, state stays null
  const m = C.parseLogLine("[5] ecg: rail=1% min=2");
  assert.equal(m.state, null); assert.deepEqual(m.fields, { rail: 1, min: 2 });
});

test("parseLogLine: state transitions", () => {
  const a = C.parseLogLine("[5000] ecg: wait -> settle");
  assert.equal(a.transition, true); assert.equal(a.evidence, false);
  assert.deepEqual([a.from, a.state, a.why, a.t_ms], ["wait", "settle", null, 5000]);
  const b = C.parseLogLine("[9000] ecg: settle -> run (settled, beat)");
  assert.deepEqual([b.from, b.state, b.why], ["settle", "run", "settled, beat"]);
  assert.deepEqual(b.fields, {});
  const c = C.parseLogLine("[9100] ppg: wait->run");
  assert.deepEqual([c.kind, c.from, c.state], ["ppg", "wait", "run"]);
});

test("parseLogLine: radio lines and other text", () => {
  const r = C.parseLogLine("radio: ble: sent 2513 notif, dropped 0");
  assert.equal(r.src, "radio"); assert.equal(r.t_ms, null); assert.equal(r.tag, "ble"); assert.equal(r.kind, "ble");
  assert.equal(r.text, "sent 2513 notif, dropped 0"); assert.equal(r.evidence, false); assert.equal(r.state, null);
  // a radio line that looks like an ecg line is not front-end evidence
  const f = C.parseLogLine("radio: ecg: run rail=1%");
  assert.equal(f.src, "radio"); assert.equal(f.evidence, false);
  const plain = C.parseLogLine("radio: hello");
  assert.equal(plain.src, "radio"); assert.equal(plain.tag, null); assert.equal(plain.text, "hello");
  const other = C.parseLogLine("[123] batt: mv=3930 pct=82");
  assert.deepEqual([other.kind, other.state, other.evidence, other.fields.mv, other.fields.pct], ["batt", null, false, 3930, 82]);
  const junk = C.parseLogLine("garbage without a prefix");
  assert.deepEqual([junk.src, junk.t_ms, junk.tag, junk.evidence], ["mcu", null, null, false]);
});

// ---- spectrum ----
test("FFT matches a direct DFT", () => {
  const n = 64, re = new Float64Array(n), im = new Float64Array(n), x = [];
  for (let i = 0; i < n; i++) { x.push(Math.sin(i * 0.37) + 0.3 * Math.cos(i * 1.9) + (i % 5) * 0.1); re[i] = x[i]; }
  C.fft(re, im);
  for (const k of [0, 1, 5, 17, 32, 63]) {
    let sr = 0, si = 0;
    for (let i = 0; i < n; i++) { sr += x[i] * Math.cos(2 * Math.PI * k * i / n); si -= x[i] * Math.sin(2 * Math.PI * k * i / n); }
    close(re[k], sr, 1e-9); close(im[k], si, 1e-9);
  }
  assert.throws(() => C.fft(new Float64Array(12), new Float64Array(12)), /power of two/);
});

test("amplitude spectrum: 60 Hz (1.0) + 10 Hz (0.25) at fs = 500, N = 2048", () => {
  const fs = 500, N = 2048, x = new Float64Array(N);
  for (let i = 0; i < N; i++) x[i] = 3 + Math.sin(2 * Math.PI * 60 * i / fs) + 0.25 * Math.sin(2 * Math.PI * 10 * i / fs + 1);
  const s = C.amplitudeSpectrum(x, fs, N);
  assert.equal(s.amp.length, N / 2 + 1); close(s.df, fs / N); assert.equal(s.n, N);
  const peak = (lo, hi) => { let k = lo; for (let i = lo; i <= hi; i++) if (s.amp[i] > s.amp[k]) k = i; return k; };
  const k60 = peak(1, s.amp.length - 1), k10 = peak(1, Math.round(30 / s.df));
  assert.ok(Math.abs(k60 - 60 / s.df) <= 1, `60 Hz peak at bin ${k60}`);
  assert.ok(Math.abs(k10 - 10 / s.df) <= 1, `10 Hz peak at bin ${k10}`);
  // Hann scalloping is at most 1.42 dB (15 %); both tones are near bin centres here
  assert.ok(Math.abs(s.amp[k60] - 1) < 0.1, `60 Hz amplitude ${s.amp[k60]}`);
  assert.ok(Math.abs(s.amp[k10] - 0.25) < 0.025, `10 Hz amplitude ${s.amp[k10]}`);
  assert.ok(Math.abs(s.amp[k10] / s.amp[k60] - 0.25) < 0.03);
  assert.ok(s.amp[0] < 1e-3, "mean removed");
  assert.ok(s.amp[Math.round(150 / s.df)] < 1e-3, "no leakage far from the tones");
  // exact-frequency readouts
  close(C.toneAmplitude(x, fs, 60), 1, 1e-3);
  close(C.toneAmplitude(x, fs, 10), 0.25, 1e-3);
  const p = C.peakToneAmplitude(x, fs, 60.2);
  close(p.amp, 1, 1e-3); assert.ok(Math.abs(p.f - 60) < 0.03);
  // fewer samples than N: windowed over the samples, zero padded, same amplitude scale
  const s2 = C.amplitudeSpectrum(x.subarray(0, 1500), fs, N);
  assert.equal(s2.n, 1500);
  assert.ok(Math.abs(Math.max(...s2.amp.subarray(1)) - 1) < 0.1);
});

const py = join(root, ".venv", "bin", "python");
test("vectors from tools/ecgcard/codec.py and record.py", { skip: !existsSync(py) && "no .venv/bin/python" }, () => {
  const script = `
import json, sys
sys.path.insert(0, ${JSON.stringify(join(root, "tools"))})
import numpy as np
from ecgcard import codec as c, record as r
out = {}
def h(m): return c.message_payload(m)[1].hex()
out["ecg"] = [
  {"hex": h(c.EcgBlock(0, 0, np.array([0,1,-1,32767,-32768,100,-100,5000,-5000,7]))), "gain": 558},
  {"hex": h(c.EcgBlock(4294967290, c.ECG_FLAG_GAIN_HI | c.ECG_FLAG_LEAD_OFF, np.arange(-5, 5) * 1111)), "gain": 1098},
]
for e in out["ecg"]:
    m = c.parse_payload(c.STREAM_ECG_BLOCK, 0, bytes.fromhex(e["hex"]))
    gain = np.where(m.flags & 1, r.GAIN_HI, r.GAIN_LO)
    e["uv"] = (m.samples.astype(float) * r.UV_PER_CODE / (8.0 * gain)).tolist()
    e["start"] = m.start; e["flags"] = m.flags
out["ppg"] = [{"hex": h(c.PpgBlock(77, 1023, 5, (-5, 4000), (-32768, 32767)))},
              {"hex": h(c.PpgBlock(78, 1023, 2, (1, 2), (-33000, 40000)))}]
out["events"] = [h(c.RPeakEvent(123456, 72)), h(c.PulseEvent(2**32 - 1, 66, 215, 11)), h(c.LeadEvent(0)),
                 h(c.StateEvent(3, 50, 2047, 91, 3)), h(c.ButtonEvent(1, 1))]
raw = np.array([32000, 32767, -32768, -30000, 20000, -100, 32000, -32000], dtype=np.int16)
valid = np.array([True, True, True, True, False, True, True, True])
out["unwrap"] = {"raw": raw.tolist(), "valid": valid.tolist(), "out": [None if np.isnan(v) else float(v) for v in r.unwrap_int16(raw, valid)]}
out["t"] = {"ecg": [float(r.ecg_time(i)) for i in (0, 1, 777)], "ppg": [float(r.ppg_time(i)) for i in (0, 1, 777)]}
print(json.dumps(out))
`;
  const V = JSON.parse(execFileSync(py, ["-c", script], { encoding: "utf8" }));
  const e = C.newEcgBlock();
  for (const v of V.ecg) {
    assert.equal(C.parseEcg(dv(fromHex(v.hex)), e), null);
    assert.equal(e.start, v.start); assert.equal(e.flags, v.flags);
    Array.from(e.s).forEach((s, i) => close(s * C.ecgUvPerUnit(e.flags), v.uv[i], 1e-12));
    assert.equal(C.hex(C.encodeEcg(e.start, e.flags, e.s)).replace(/ /g, ""), v.hex);
  }
  const p = C.newPpgBlock();
  assert.equal(C.parsePpg(dv(fromHex(V.ppg[0].hex)), p), null);
  assert.deepEqual([p.start, p.led, p.flags, ...p.coarse, ...p.fine], [77, 1023, 5, -5, 4000, -32768, 32767]);
  assert.equal(C.parsePpg(dv(fromHex(V.ppg[1].hex)), p), null);
  assert.deepEqual(Array.from(p.fine), [C.wrapI16(-33000), C.wrapI16(40000)]);
  const ev = C.newEvent();
  const want = [{ sample: 123456, hr_bpm: 72 }, { foot_sample: 2 ** 32 - 1, pulse_bpm: 66, pat_ms: 215, pi_permille: 11 }, { contact: 0 },
    { mode: 3, notch_hz: 50, mid_code: 2047, batt_pct: 91, flags: 3 }, { button: 1, kind: 1 }];
  V.events.forEach((hx, i) => {
    assert.equal(C.parseEvent(dv(fromHex(hx)), ev), null);
    assert.deepEqual(C.eventFields(ev), want[i]);
    assert.equal(C.hex(C.encodeEvent(ev.kind, ev)).replace(/ /g, ""), hx);
  });
  // unwrap over valid samples equals record.unwrap_int16
  const u = new C.Unwrap16();
  V.unwrap.raw.forEach((raw, i) => { if (V.unwrap.valid[i]) assert.equal(u.push(raw), V.unwrap.out[i]); });
  V.t.ecg.forEach((t, i) => close(C.ecgTime([0, 1, 777][i]), t));
  V.t.ppg.forEach((t, i) => close(C.ppgTime([0, 1, 777][i]), t));
});
