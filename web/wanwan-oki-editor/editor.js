'use strict';

const TABLE_COMMANDS = 128;
const DEFAULT_RATE = 8000;
const DEFAULT_PEAK = 1450;
const INDEX_SHIFT = [-1, -1, -1, -1, 2, 4, 6, 8];
const NBL2BIT = [
  [ 1,0,0,0], [ 1,0,0,1], [ 1,0,1,0], [ 1,0,1,1],
  [ 1,1,0,0], [ 1,1,0,1], [ 1,1,1,0], [ 1,1,1,1],
  [-1,0,0,0], [-1,0,0,1], [-1,0,1,0], [-1,0,1,1],
  [-1,1,0,0], [-1,1,0,1], [-1,1,1,0], [-1,1,1,1]
];
const DIFF_LOOKUP = Array.from({ length: 49 }, (_, step) => {
  const stepval = Math.floor(16.0 * Math.pow(11.0 / 10.0, step));
  return NBL2BIT.map(([sign, b1, b2, b3]) => sign * (stepval * b1 + Math.floor(stepval / 2) * b2 + Math.floor(stepval / 4) * b3 + Math.floor(stepval / 8)));
});

const SLOT_DEFS = [
  { cmd:0x01, known:false, assumption:'guess: dog bark / short dog vocalization' },
  { cmd:0x02, known:false, assumption:'guess: dog complain / whine' },
  { cmd:0x03, known:false, assumption:'guess: dog disappointed vocalization' },
  { cmd:0x04, known:false, assumption:'guess: dog grunt, originally heard with background music' },
  { cmd:0x05, known:false, assumption:'guess: bush / rustle effect' },
  { cmd:0x06, known:false, assumption:'guess: page flip effect' },
  { cmd:0x07, known:false, assumption:'guess: ship / machine / motor effect' },
  { cmd:0x08, known:false, assumption:'guess: explosion / impact effect' },
  { cmd:0x09, known:false, assumption:'guess: alternate dog bark' },
  { cmd:0x0A, known:false, assumption:'guess: alternate dog complain / whine' },
  { cmd:0x0B, known:false, assumption:'guess: alternate dog disappointed vocalization' },
  { cmd:0x0C, known:false, assumption:'guess: alternate dog grunt' },
  { cmd:0x0D, known:false, assumption:'guess: alternate bush / rustle effect' },
  { cmd:0x0E, known:false, assumption:'guess: alternate page flip effect' },
  { cmd:0x0F, known:false, assumption:'guess: alternate ship / machine / motor effect' },
  { cmd:0x10, known:false, assumption:'guess: bang / explosion / impact effect' },
  { cmd:0x11, known:false, assumption:'guess: yip / short bark' },
  { cmd:0x12, known:false, assumption:'guess: alternate complain / whine' },
  { cmd:0x13, known:true, name:'Dog bark 2', assumption:'trace-known: command 0x13 was observed from Wanwan .ls2' },
  { cmd:0x14, known:false, assumption:'guess: rustle / movement effect' },
  { cmd:0x15, known:false, assumption:'guess: alternate disappointed dog vocalization' },
  { cmd:0x16, known:true, name:'Dog bark', assumption:'trace-known: command 0x16 was observed from Wanwan .ls0' }
];

const slots = new Map();
let audioContext = null;
let lastRom = null;
let lastOrder = '';
let montageObjectUrl = null;

const els = {
  slots: document.getElementById('slots'),
  slotTemplate: document.getElementById('slotTemplate'),
  exportRom: document.getElementById('exportRom'),
  exportProject: document.getElementById('exportProject'),
  importProject: document.getElementById('importProject'),
  importRom: document.getElementById('importRom'),
  targetRate: document.getElementById('targetRate'),
  defaultPeak: document.getElementById('defaultPeak'),
  defaultMaxMs: document.getElementById('defaultMaxMs'),
  trimActive: document.getElementById('trimActive'),
  dcBlock: document.getElementById('dcBlock'),
  batchFiles: document.getElementById('batchFiles'),
  batchDrop: document.getElementById('batchDrop'),
  batchLog: document.getElementById('batchLog'),
  romSize: document.getElementById('romSize'),
  filledCount: document.getElementById('filledCount'),
  warningCount: document.getElementById('warningCount'),
  buildMontage: document.getElementById('buildMontage'),
  montageAudio: document.getElementById('montageAudio'),
  orderText: document.getElementById('orderText'),
  downloadOrder: document.getElementById('downloadOrder'),
  clearAll: document.getElementById('clearAll')
};

function hex(n, width = 2) { return '0x' + n.toString(16).toUpperCase().padStart(width, '0'); }
function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function getAudioContext() {
  if (!audioContext) audioContext = new (window.AudioContext || window.webkitAudioContext)();
  return audioContext;
}
function revokeUrl(url) { if (url) URL.revokeObjectURL(url); }

function renderSlots() {
  els.slots.textContent = '';
  for (const def of SLOT_DEFS) {
    const node = els.slotTemplate.content.firstElementChild.cloneNode(true);
    const cmd = node.querySelector('.cmd');
    const title = node.querySelector('h3');
    const confidence = node.querySelector('.confidence');
    const assumption = node.querySelector('.assumption');
    const audioInput = node.querySelector('.audioInput');
    const peakInput = node.querySelector('.peakInput');
    const pitchInput = node.querySelector('.pitchInput');
    const maxMsInput = node.querySelector('.maxMsInput');
    const previewBtn = node.querySelector('.previewBtn');
    const downloadWavBtn = node.querySelector('.downloadWavBtn');
    const clearBtn = node.querySelector('.clearBtn');

    cmd.textContent = `Command ${hex(def.cmd)}`;
    title.textContent = def.known ? def.name : 'Guess';
    confidence.textContent = def.known ? 'known' : 'guess';
    confidence.classList.add(def.known ? 'known' : 'guess');
    assumption.textContent = def.assumption;
    audioInput.addEventListener('change', () => {
      if (audioInput.files && audioInput.files[0]) handleAudioFile(def.cmd, audioInput.files[0]);
    });
    for (const input of [peakInput, pitchInput, maxMsInput]) {
      input.addEventListener('change', () => reprocessSlot(def.cmd));
    }
    previewBtn.addEventListener('click', () => previewSlot(def.cmd));
    downloadWavBtn.addEventListener('click', () => downloadDecodedWav(def.cmd));
    clearBtn.addEventListener('click', () => clearSlot(def.cmd));

    const state = {
      def, node,
      status: node.querySelector('.slot-status'),
      previewAudio: node.querySelector('.previewAudio'),
      audioInput, peakInput, pitchInput, maxMsInput,
      previewBtn, downloadWavBtn, clearBtn,
      sourceName: '', sourceBuffer: null, processedPcm: [], nibbles: [], phrase: new Uint8Array(), decodedPcm: [], previewUrl: null
    };
    slots.set(def.cmd, state);
    els.slots.appendChild(node);
  }
}

function readGlobalOptions() {
  return {
    targetRate: Number(els.targetRate.value) || DEFAULT_RATE,
    defaultPeak: clamp(Number(els.defaultPeak.value) || DEFAULT_PEAK, 200, 2047),
    defaultMaxMs: Math.max(0, Number(els.defaultMaxMs.value) || 0),
    trimActive: els.trimActive.checked,
    dcBlock: els.dcBlock.checked
  };
}

async function decodeAudioFile(file) {
  const ctx = getAudioContext();
  const bytes = await file.arrayBuffer();
  const audio = await ctx.decodeAudioData(bytes.slice(0));
  const len = audio.length;
  const channels = audio.numberOfChannels;
  const mono = new Float32Array(len);
  for (let ch = 0; ch < channels; ch++) {
    const data = audio.getChannelData(ch);
    for (let i = 0; i < len; i++) mono[i] += data[i] / channels;
  }
  return { sampleRate: audio.sampleRate, samples: mono };
}

function movingAverageAbs(data, radius) {
  const out = new Float32Array(data.length);
  let acc = 0;
  const queue = [];
  for (let i = 0; i < data.length; i++) {
    const v = Math.abs(data[i]);
    queue.push(v); acc += v;
    if (queue.length > radius) acc -= queue.shift();
    out[i] = acc / queue.length;
  }
  return out;
}

function trimActive(rate, data) {
  if (!data.length) return data;
  let mean = 0;
  for (const v of data) mean += v;
  mean /= data.length;
  const centered = new Float32Array(data.length);
  for (let i = 0; i < data.length; i++) centered[i] = data[i] - mean;
  const env = movingAverageAbs(centered, Math.max(1, Math.round(rate * 0.006)));
  let peak = 0;
  for (const v of env) if (v > peak) peak = v;
  const threshold = Math.max(0.0015, peak * 0.025);
  let start = -1, stop = -1;
  for (let i = 0; i < env.length; i++) if (env[i] > threshold) { start = i; break; }
  for (let i = env.length - 1; i >= 0; i--) if (env[i] > threshold) { stop = i + 1; break; }
  if (start < 0 || stop <= start) return centered;
  const pad = Math.round(rate * 0.012);
  start = Math.max(0, start - pad);
  stop = Math.min(centered.length, stop + pad);
  return centered.slice(start, stop);
}

function dcBlock(data) {
  const out = new Float32Array(data.length);
  let prevX = 0, prevY = 0;
  const r = 0.995;
  for (let i = 0; i < data.length; i++) {
    const x = data[i];
    const y = x - prevX + r * prevY;
    out[i] = y;
    prevX = x; prevY = y;
  }
  return out;
}

function resampleLinear(data, srcRate, dstRate) {
  if (!data.length || Math.abs(srcRate - dstRate) < 0.001) return new Float32Array(data);
  const outLen = Math.max(1, Math.round(data.length * dstRate / srcRate));
  const out = new Float32Array(outLen);
  const scale = srcRate / dstRate;
  const last = data.length - 1;
  for (let i = 0; i < outLen; i++) {
    const pos = i * scale;
    const j = Math.floor(pos);
    const frac = pos - j;
    out[i] = j >= last ? data[last] : data[j] * (1 - frac) + data[j + 1] * frac;
  }
  return out;
}

function pitchTime(data, pitch) {
  if (!data.length || Math.abs(pitch - 1) < 0.0001) return new Float32Array(data);
  const outLen = Math.max(8, Math.round(data.length / pitch));
  const out = new Float32Array(outLen);
  const last = data.length - 1;
  for (let i = 0; i < outLen; i++) {
    const pos = i * last / Math.max(1, outLen - 1);
    const j = Math.floor(pos);
    const frac = pos - j;
    out[i] = j >= last ? data[last] : data[j] * (1 - frac) + data[j + 1] * frac;
  }
  return out;
}

function normalizePeak(data, peakTarget) {
  const out = new Int16Array(data.length);
  if (!data.length) return out;
  let mean = 0;
  for (const v of data) mean += v;
  mean /= data.length;
  let peak = 0;
  for (const v of data) peak = Math.max(peak, Math.abs(v - mean));
  const scale = peak > 0 ? peakTarget / peak : 1;
  const fade = Math.min(Math.floor(data.length / 8), 80);
  for (let i = 0; i < data.length; i++) {
    let mul = 1;
    if (fade > 1 && i < fade) mul = i / fade;
    else if (fade > 1 && i >= data.length - fade) mul = (data.length - 1 - i) / fade;
    out[i] = clamp(Math.round((data[i] - mean) * scale * Math.max(0, Math.min(1, mul))), -2048, 2047);
  }
  return out;
}

function clock(signal, step, nibble) {
  signal += DIFF_LOOKUP[step][nibble & 15];
  signal = clamp(signal, -2048, 2047);
  step += INDEX_SHIFT[nibble & 7];
  step = clamp(step, 0, 48);
  return [signal, step];
}

function encodePcm(pcm) {
  let signal = -2, step = 0;
  const out = new Uint8Array(pcm.length);
  for (let i = 0; i < pcm.length; i++) {
    const target = pcm[i];
    let bestNibble = 0, bestError = 1e30, bestSignal = signal, bestStep = step;
    for (let nibble = 0; nibble < 16; nibble++) {
      const [predSignal, predStep] = clock(signal, step, nibble);
      const error = Math.abs(target - predSignal);
      if (error < bestError) {
        bestError = error;
        bestNibble = nibble;
        bestSignal = predSignal;
        bestStep = predStep;
      }
    }
    out[i] = bestNibble;
    signal = bestSignal; step = bestStep;
  }
  return out;
}

function decodeNibbles(nibbles) {
  let signal = -2, step = 0;
  const out = new Int16Array(nibbles.length);
  for (let i = 0; i < nibbles.length; i++) {
    [signal, step] = clock(signal, step, nibbles[i]);
    out[i] = signal;
  }
  return out;
}

function unpackPhrase(phrase) {
  const nibbles = [];
  let pos = 0;
  while (pos < phrase.length) {
    const count = phrase[pos++];
    if (count === 0) break;
    if (pos + count > phrase.length) throw new Error('truncated phrase block');
    for (let i = 0; i < count; i++) {
      const b = phrase[pos++];
      nibbles.push((b >> 4) & 15, b & 15);
    }
  }
  return new Uint8Array(nibbles);
}

function extractPhraseFromRom(rom, start) {
  if (start <= 0 || start >= rom.length) return null;
  const bytes = [];
  let pos = start;
  let blocks = 0;
  while (pos < rom.length) {
    const count = rom[pos++];
    bytes.push(count);
    if (count === 0) return new Uint8Array(bytes);
    if (pos + count > rom.length) throw new Error(`phrase at ${hex(start, 6)} runs past end of ROM`);
    for (let i = 0; i < count; i++) bytes.push(rom[pos++]);
    blocks++;
    if (blocks > 4096) throw new Error(`phrase at ${hex(start, 6)} has no terminator`);
  }
  throw new Error(`phrase at ${hex(start, 6)} has no terminator`);
}

function packPhrase(nibbles) {
  const bytes = [];
  let index = 0;
  while (index < nibbles.length) {
    let count = Math.min(254, nibbles.length - index);
    if (count & 1) count--;
    if (count <= 0) break;
    bytes.push(count >> 1);
    for (let offset = 0; offset < count; offset += 2) {
      bytes.push(((nibbles[index + offset] & 15) << 4) | (nibbles[index + offset + 1] & 15));
    }
    index += count;
  }
  bytes.push(0);
  return new Uint8Array(bytes);
}

function preprocessSource(slot) {
  if (!slot.sourceBuffer) return;
  const opts = readGlobalOptions();
  let data = slot.sourceBuffer.samples;
  let rate = slot.sourceBuffer.sampleRate;
  if (opts.trimActive) data = trimActive(rate, data);
  if (opts.dcBlock) { data = dcBlock(data); data = dcBlock(data); }
  data = resampleLinear(data, rate, opts.targetRate);
  data = pitchTime(data, clamp(Number(slot.pitchInput.value) || 1, 0.25, 4));
  const maxMs = Math.max(0, Number(slot.maxMsInput.value) || opts.defaultMaxMs);
  if (maxMs > 0) data = data.slice(0, Math.max(8, Math.round(opts.targetRate * maxMs / 1000)));
  const peak = clamp(Number(slot.peakInput.value) || opts.defaultPeak, 200, 2047);
  const pcm = normalizePeak(data, peak);
  const nibbles = encodePcm(pcm);
  const decoded = decodeNibbles(nibbles);
  const phrase = packPhrase(nibbles);
  slot.processedPcm = pcm;
  slot.nibbles = nibbles;
  slot.decodedPcm = decoded;
  slot.phrase = phrase;
  slot.status.textContent = `${slot.sourceName}\nsource ${slot.sourceBuffer.sampleRate.toFixed(0)} Hz -> ${opts.targetRate} Hz\npcm ${pcm.length} samples, phrase ${phrase.length} bytes\nduration ${(pcm.length / opts.targetRate).toFixed(3)} s`;
  slot.previewBtn.disabled = false;
  slot.downloadWavBtn.disabled = false;
  slot.clearBtn.disabled = false;
  revokeUrl(slot.previewUrl); slot.previewUrl = null; slot.previewAudio.removeAttribute('src');
}

async function handleAudioFile(cmd, file) {
  const slot = slots.get(cmd);
  if (!slot) return;
  try {
    slot.status.textContent = `Decoding ${file.name}...`;
    slot.sourceName = file.name;
    slot.sourceBuffer = await decodeAudioFile(file);
    preprocessSource(slot);
    updateSummary();
  } catch (err) {
    slot.status.textContent = `Decode failed: ${err && err.message ? err.message : String(err)}`;
  }
}

function reprocessSlot(cmd) {
  const slot = slots.get(cmd);
  if (!slot || !slot.sourceBuffer) return;
  preprocessSource(slot);
  updateSummary();
}

function pcmToWavBlob(pcm, sampleRate, scale = 14) {
  const n = pcm.length;
  const dataSize = n * 2;
  const buf = new ArrayBuffer(44 + dataSize);
  const dv = new DataView(buf);
  const writeStr = (off, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); };
  writeStr(0, 'RIFF'); dv.setUint32(4, 36 + dataSize, true); writeStr(8, 'WAVE');
  writeStr(12, 'fmt '); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true);
  dv.setUint32(24, sampleRate, true); dv.setUint32(28, sampleRate * 2, true); dv.setUint16(32, 2, true); dv.setUint16(34, 16, true);
  writeStr(36, 'data'); dv.setUint32(40, dataSize, true);
  let off = 44;
  for (let i = 0; i < n; i++, off += 2) dv.setInt16(off, clamp(Math.round(pcm[i] * scale), -32768, 32767), true);
  return new Blob([buf], { type: 'audio/wav' });
}

function previewSlot(cmd) {
  const slot = slots.get(cmd);
  if (!slot || !slot.decodedPcm.length) return;
  const opts = readGlobalOptions();
  revokeUrl(slot.previewUrl);
  slot.previewUrl = URL.createObjectURL(pcmToWavBlob(slot.decodedPcm, opts.targetRate));
  slot.previewAudio.src = slot.previewUrl;
  slot.previewAudio.play().catch(() => {});
}

function downloadDecodedWav(cmd) {
  const slot = slots.get(cmd);
  if (!slot || !slot.decodedPcm.length) return;
  const opts = readGlobalOptions();
  downloadBlob(pcmToWavBlob(slot.decodedPcm, opts.targetRate), `wanwan_cmd${hex(cmd).slice(2)}_decoded.wav`);
}

function clearSlot(cmd) {
  const slot = slots.get(cmd);
  if (!slot) return;
  slot.sourceName = '';
  slot.sourceBuffer = null;
  slot.processedPcm = [];
  slot.nibbles = [];
  slot.phrase = new Uint8Array();
  slot.decodedPcm = [];
  slot.audioInput.value = '';
  slot.status.textContent = '';
  slot.previewBtn.disabled = true;
  slot.downloadWavBtn.disabled = true;
  slot.clearBtn.disabled = true;
  revokeUrl(slot.previewUrl); slot.previewUrl = null; slot.previewAudio.removeAttribute('src');
  updateSummary();
}

function buildRom() {
  const rom = new Uint8Array(8 * 1024 * 1024);
  let pos = TABLE_COMMANDS * 4;
  const order = [];
  for (const def of SLOT_DEFS) {
    const slot = slots.get(def.cmd);
    if (!slot || !slot.phrase.length) {
      order.push(`${hex(def.cmd)}  ${def.known ? def.name : 'Guess'}  EMPTY`);
      continue;
    }
    while (pos & 0xF) pos++;
    const start = pos;
    if (start + slot.phrase.length >= rom.length) throw new Error('ROM exceeds 8 MiB safety limit');
    rom.set(slot.phrase, start);
    rom[def.cmd * 4 + 0] = (start >> 16) & 0xFF;
    rom[def.cmd * 4 + 1] = (start >> 8) & 0xFF;
    rom[def.cmd * 4 + 2] = start & 0xFF;
    rom[def.cmd * 4 + 3] = 0;
    pos += slot.phrase.length;
    order.push(`${hex(def.cmd)}  ${def.known ? def.name : 'Guess'}  ${slot.sourceName || '(project phrase)'}  start=${hex(start, 6)} bytes=${slot.phrase.length} ${def.known ? 'trace-known' : 'guess'}`);
  }
  lastRom = rom.slice(0, pos);
  lastOrder = [
    'Wanwan MSM6653A-457 replacement ADPCM ROM generated by browser editor',
    '',
    'Only trace-grounded command slots are named. Other slots are guesses based on original-sample assumptions.',
    'Command order:',
    ...order
  ].join('\n') + '\n';
  els.orderText.textContent = lastOrder;
  return lastRom;
}

function updateSummary() {
  let filled = 0, guesses = 0;
  for (const def of SLOT_DEFS) {
    const slot = slots.get(def.cmd);
    if (slot && slot.phrase.length) filled++;
    if (!def.known) guesses++;
  }
  try { els.romSize.textContent = buildRom().length; } catch { els.romSize.textContent = 'error'; }
  els.filledCount.textContent = filled;
  els.warningCount.textContent = guesses;
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function exportRom() {
  try {
    const rom = buildRom();
    downloadBlob(new Blob([rom], { type: 'application/octet-stream' }), 'wanwan_msm6653a_457_replacement.bin');
  } catch (err) {
    alert(err && err.message ? err.message : String(err));
  }
}

function buildMontage() {
  const opts = readGlobalOptions();
  const silence = new Int16Array(Math.round(opts.targetRate * 2.0));
  const pieces = [];
  let total = 0;
  for (const def of SLOT_DEFS) {
    const slot = slots.get(def.cmd);
    const pcm = slot && slot.decodedPcm.length ? slot.decodedPcm : new Int16Array(Math.round(opts.targetRate * 0.25));
    pieces.push(pcm, silence);
    total += pcm.length + silence.length;
  }
  const montage = new Int16Array(total);
  let off = 0;
  for (const p of pieces) { montage.set(p, off); off += p.length; }
  revokeUrl(montageObjectUrl);
  montageObjectUrl = URL.createObjectURL(pcmToWavBlob(montage, opts.targetRate));
  els.montageAudio.src = montageObjectUrl;
  els.montageAudio.play().catch(() => {});
}

function downloadOrderText() {
  buildRom();
  downloadBlob(new Blob([lastOrder], { type: 'text/plain' }), 'wanwan_oki_editor_order.txt');
}

function exportProject() {
  buildRom();
  const phrases = [];
  for (const def of SLOT_DEFS) {
    const slot = slots.get(def.cmd);
    if (!slot || !slot.phrase.length) continue;
    phrases.push({
      cmd: def.cmd,
      sourceName: slot.sourceName,
      phrase: arrayToBase64(slot.phrase),
      nibbles: arrayToBase64(slot.nibbles),
      decodedPcm: int16ToBase64(slot.decodedPcm),
      peak: Number(slot.peakInput.value),
      pitch: Number(slot.pitchInput.value),
      maxMs: Number(slot.maxMsInput.value)
    });
  }
  const project = {
    version: 1,
    generatedAt: new Date().toISOString(),
    options: readGlobalOptions(),
    phrases
  };
  downloadBlob(new Blob([JSON.stringify(project, null, 2)], { type: 'application/json' }), 'wanwan_oki_editor_project.json');
}

function arrayToBase64(arr) {
  let s = '';
  const u8 = arr instanceof Uint8Array ? arr : new Uint8Array(arr.buffer || arr);
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return btoa(s);
}
function int16ToBase64(arr) { return arrayToBase64(new Uint8Array(arr.buffer)); }
function base64ToU8(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}
function base64ToI16(s) {
  const u8 = base64ToU8(s);
  return new Int16Array(u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength));
}


async function importRomFile(file) {
  const rom = new Uint8Array(await file.arrayBuffer());
  if (rom.length < TABLE_COMMANDS * 4) throw new Error('ROM is too small to contain a 128-entry OKI phrase table');
  const imported = [];
  const failed = [];
  const seenStarts = new Map();
  for (const def of SLOT_DEFS) {
    const off = def.cmd * 4;
    const start = (rom[off] << 16) | (rom[off + 1] << 8) | rom[off + 2];
    const flags = rom[off + 3];
    const slot = slots.get(def.cmd);
    if (!slot) continue;
    if (!start) {
      clearSlot(def.cmd);
      continue;
    }
    try {
      const phrase = seenStarts.get(start) || extractPhraseFromRom(rom, start);
      if (!phrase) {
        clearSlot(def.cmd);
        continue;
      }
      seenStarts.set(start, phrase);
      const nibbles = unpackPhrase(phrase);
      const decoded = decodeNibbles(nibbles);
      slot.sourceName = `${file.name}: command ${hex(def.cmd)} @ ${hex(start, 6)}`;
      slot.sourceBuffer = null;
      slot.processedPcm = [];
      slot.nibbles = nibbles;
      slot.phrase = phrase;
      slot.decodedPcm = decoded;
      slot.audioInput.value = '';
      slot.status.textContent = `${slot.sourceName}\nimported phrase ${phrase.length} bytes, flags ${hex(flags)}\ndecoded ${decoded.length} samples\nduration ${(decoded.length / readGlobalOptions().targetRate).toFixed(3)} s`;
      slot.previewBtn.disabled = !decoded.length;
      slot.downloadWavBtn.disabled = !decoded.length;
      slot.clearBtn.disabled = false;
      revokeUrl(slot.previewUrl); slot.previewUrl = null; slot.previewAudio.removeAttribute('src');
      imported.push(`${hex(def.cmd)} <- ${hex(start, 6)} (${phrase.length} bytes)`);
    } catch (err) {
      failed.push(`${hex(def.cmd)} @ ${hex(start, 6)}: ${err && err.message ? err.message : String(err)}`);
    }
  }
  updateSummary();
  const message = [`Imported ${imported.length} Wanwan command slots from ${file.name}.`];
  if (failed.length) message.push('', 'Warnings:', ...failed.slice(0, 12), failed.length > 12 ? `...${failed.length - 12} more` : '');
  els.batchLog.textContent = message.join('\n');
}

async function importProjectFile(file) {
  const project = JSON.parse(await file.text());
  if (!project || project.version !== 1 || !Array.isArray(project.phrases)) throw new Error('Unsupported project file');
  if (project.options) {
    if (project.options.targetRate) els.targetRate.value = String(project.options.targetRate);
    if (project.options.defaultPeak) els.defaultPeak.value = String(project.options.defaultPeak);
    if (typeof project.options.defaultMaxMs === 'number') els.defaultMaxMs.value = String(project.options.defaultMaxMs);
    if (typeof project.options.trimActive === 'boolean') els.trimActive.checked = project.options.trimActive;
    if (typeof project.options.dcBlock === 'boolean') els.dcBlock.checked = project.options.dcBlock;
  }
  for (const p of project.phrases) {
    const slot = slots.get(p.cmd);
    if (!slot) continue;
    slot.sourceName = p.sourceName || 'imported project phrase';
    slot.sourceBuffer = null;
    slot.phrase = base64ToU8(p.phrase || '');
    slot.nibbles = base64ToU8(p.nibbles || '');
    slot.decodedPcm = base64ToI16(p.decodedPcm || '');
    slot.peakInput.value = String(p.peak || DEFAULT_PEAK);
    slot.pitchInput.value = String(p.pitch || 1);
    slot.maxMsInput.value = String(p.maxMs || 0);
    slot.status.textContent = `${slot.sourceName}\nimported phrase ${slot.phrase.length} bytes\ndecoded ${slot.decodedPcm.length} samples`;
    slot.previewBtn.disabled = !slot.decodedPcm.length;
    slot.downloadWavBtn.disabled = !slot.decodedPcm.length;
    slot.clearBtn.disabled = false;
  }
  updateSummary();
}

function parseCommandFromFilename(name) {
  const lower = name.toLowerCase();
  let m = lower.match(/cmd\s*([0-9a-f]{2})\b/i);
  if (m) return parseInt(m[1], 16);
  m = lower.match(/0x([0-9a-f]{1,2})\b/i);
  if (m) return parseInt(m[1], 16);
  m = lower.match(/(?:^|[^0-9])([1-9]|1[0-9]|2[0-2])(?:[^0-9]|$)/);
  if (m) return parseInt(m[1], 10);
  return 0;
}

async function handleBatchFiles(files) {
  const log = [];
  for (const file of files) {
    const cmd = parseCommandFromFilename(file.name);
    if (cmd >= 1 && cmd <= 0x16 && slots.has(cmd)) {
      await handleAudioFile(cmd, file);
      log.push(`${file.name} -> ${hex(cmd)}`);
    } else {
      log.push(`${file.name} -> not assigned; rename with cmdNN or 0xNN, or upload into a slot manually`);
    }
  }
  els.batchLog.textContent = log.join('\n');
}

for (const id of ['targetRate', 'defaultPeak', 'defaultMaxMs', 'trimActive', 'dcBlock']) {
  els[id].addEventListener('change', () => {
    for (const cmd of slots.keys()) reprocessSlot(cmd);
    updateSummary();
  });
}
els.exportRom.addEventListener('click', exportRom);
els.exportProject.addEventListener('click', exportProject);
els.importProject.addEventListener('change', () => {
  if (els.importProject.files && els.importProject.files[0]) importProjectFile(els.importProject.files[0]).catch(err => alert(err.message || String(err)));
});
els.importRom.addEventListener('change', () => {
  if (els.importRom.files && els.importRom.files[0]) importRomFile(els.importRom.files[0]).catch(err => alert(err.message || String(err)));
});
els.batchFiles.addEventListener('change', () => { if (els.batchFiles.files) handleBatchFiles([...els.batchFiles.files]); });
for (const eventName of ['dragenter', 'dragover']) {
  els.batchDrop.addEventListener(eventName, ev => { ev.preventDefault(); els.batchDrop.classList.add('drag'); });
}
for (const eventName of ['dragleave', 'drop']) {
  els.batchDrop.addEventListener(eventName, ev => { ev.preventDefault(); els.batchDrop.classList.remove('drag'); });
}
els.batchDrop.addEventListener('drop', ev => {
  const files = [...(ev.dataTransfer ? ev.dataTransfer.files : [])];
  if (files.length) handleBatchFiles(files);
});
els.buildMontage.addEventListener('click', buildMontage);
els.downloadOrder.addEventListener('click', downloadOrderText);
els.clearAll.addEventListener('click', () => { if (confirm('Clear all uploaded and imported audio?')) { for (const cmd of slots.keys()) clearSlot(cmd); } });

renderSlots();
updateSummary();
