const STORAGE_CONFIG = 'loopy.wasm.config.v1';
const STORAGE_MANIFEST = 'loopy.wasm.manifest.v1';
const DB_NAME = 'loopy-wasm-storage';
const DB_VERSION = 1;
const STORE_FILES = 'files';
const EXPECTED_BIOS_CRC = 0x8c57ff9f;
const EXPECTED_SOUND_CRC = 0x8f51fa17;
const STATUS_TEXT = { 0:'waiting for ROMs', 1:'ROMs ready', 2:'cart ready', 3:'running', 4:'paused', 5:'error' };
const BUTTONS = [
  ['Start', 0x0002], ['A', 0x0010], ['B', 0x0080], ['C', 0x0040], ['D', 0x0020],
  ['L1', 0x0004], ['R1', 0x0008], ['Up', 0x0100], ['Down', 0x0200], ['Left', 0x0400], ['Right', 0x0800]
];
const DEFAULT_KEYS = { Start:'Enter', A:'KeyZ', B:'KeyX', C:'KeyC', D:'KeyV', L1:'KeyA', R1:'KeyS', Up:'ArrowUp', Down:'ArrowDown', Left:'ArrowLeft', Right:'ArrowRight' };
const DEFAULT_CONFIG = { keys:{...DEFAULT_KEYS}, video:{ aspect:'native', smoothUpscale:false, scanlines:false, showFps:true }, touchGamepad:{ opacity:0.72 }, wanwanReplacementPcm:true, stateSlot:0, portDevice:'gamepad' };
const els = {
  canvas: document.getElementById('video'), screenFrame: document.getElementById('screenFrame'), fpsCounter: document.getElementById('fpsCounter'),
  touchGamepad: document.getElementById('touchGamepad'), touchStick: document.getElementById('touchStick'), touchKnob: document.getElementById('touchKnob'), touchOpacity: document.getElementById('touchOpacity'),
  menuToggle: document.getElementById('menuToggle'), closeMenu: document.getElementById('closeMenu'), sideMenu: document.getElementById('sideMenu'), fullscreenToggle: document.getElementById('fullscreenToggle'),
  startupModal: document.getElementById('startupModal'), startupError: document.getElementById('startupError'), openStartup: document.getElementById('openStartup'), softResetTop: document.getElementById('softResetTop'), startButton: document.getElementById('startButton'), closeStartup: document.getElementById('closeStartup'), closeStartupAlt: document.getElementById('closeStartupAlt'),
  biosFile: document.getElementById('biosFile'), soundFile: document.getElementById('soundFile'), okiFile: document.getElementById('okiFile'), wanwanReplacementPcm: document.getElementById('wanwanReplacementPcm'), biosStatus: document.getElementById('biosStatus'), soundStatus: document.getElementById('soundStatus'), okiStatus: document.getElementById('okiStatus'), biosCard: document.getElementById('biosCard'), soundCard: document.getElementById('soundCard'), okiCard: document.getElementById('okiCard'),
  cartFile: document.getElementById('cartFile'), startupCartFile: document.getElementById('startupCartFile'), cartName: document.getElementById('cartName'), startupCartName: document.getElementById('startupCartName'),
  runtimeStatus: document.getElementById('runtimeStatus'), runtimeResolution: document.getElementById('runtimeResolution'), runtimeFrame: document.getElementById('runtimeFrame'), runtimeCart: document.getElementById('runtimeCart'),
  aspectMode: document.getElementById('aspectMode'), smoothUpscale: document.getElementById('smoothUpscale'), scanlines: document.getElementById('scanlines'), showFps: document.getElementById('showFps'),
  controlMap: document.getElementById('controlMap'), portDevice: document.getElementById('portDevice'), captureMouse: document.getElementById('captureMouse'), mouseCaptureStatus: document.getElementById('mouseCaptureStatus'), pauseToggle: document.getElementById('pauseToggle'), resetButton: document.getElementById('resetButton'), stateSlot: document.getElementById('stateSlot'), saveState: document.getElementById('saveState'), loadState: document.getElementById('loadState'), clearStorage: document.getElementById('clearStorage'), storageStatus: document.getElementById('storageStatus'),
  printModal: document.getElementById('printModal'), printCanvas: document.getElementById('printCanvas'), downloadPrint: document.getElementById('downloadPrint'), closePrint: document.getElementById('closePrint')
};
let wasm, memory, db;
let ctx = els.canvas.getContext('2d', { alpha:false });
let imageData = null;
let config = loadConfig();
let manifest = loadManifest();
let activeCartBytes = null;
let activeCartName = '';
let pressed = new Set();
let virtualPressed = new Set();
let activeTouchStickId = null;
let activeTouchStickRect = null;
let remapTarget = null;
let paused = false;
let running = false;
let autoPausedForHiddenTab = false;
let frameRequest = 0;
let fpsLastTime = 0, fpsLastFrame = 0, fpsValue = 0;
let limiterLastTime = 0, limiterAccumulator = 0;
let mouseLastX = 0, mouseLastY = 0, mouseLastValid = false, mouseFracX = 0, mouseFracY = 0, mouseDX = 0, mouseDY = 0, mouseButtons = 0;
let pendingPrintUrl = '', pendingPrintFilename = 'loopy_print.png';
let audioCtx = null, audioNode = null, audioWorkletReady = false, audioWorkletPromise = null;
let audioQueue = [], audioQueueOffset = 0, audioQueueFrames = 0;
let audioGeneration = 0;
const AUDIO_MAX_POST_FRAMES = 4096;
const AUDIO_PROCESSOR = `loopy-pcm-${Math.random().toString(36).slice(2)}`;
const FRAME_RATE = 598261 / 10000;
const MAX_EMU_FRAMES_PER_RAF = 3;
const touchControlsMedia = typeof window !== 'undefined' && window.matchMedia ? window.matchMedia('(hover: none) and (pointer: coarse), (any-pointer: coarse)') : null;

function loadConfig() {
  try {
    const raw = localStorage.getItem(STORAGE_CONFIG);
    if (raw) {
      const saved = JSON.parse(raw);
      return { ...DEFAULT_CONFIG, ...saved, keys:{...DEFAULT_KEYS, ...(saved.keys || {})}, video:{...DEFAULT_CONFIG.video, ...(saved.video || {})}, touchGamepad:{...DEFAULT_CONFIG.touchGamepad, ...(saved.touchGamepad || {})}, wanwanReplacementPcm: saved.wanwanReplacementPcm !== false, portDevice: saved.portDevice === 'mouse' ? 'mouse' : 'gamepad', stateSlot: Number.isInteger(saved.stateSlot) ? saved.stateSlot : 0 };
    }
  } catch (_) {}
  return structuredClone(DEFAULT_CONFIG);
}
function saveConfig(){ localStorage.setItem(STORAGE_CONFIG, JSON.stringify(config)); }
function loadManifest(){ try { return JSON.parse(localStorage.getItem(STORAGE_MANIFEST)) || {roms:{}, cart:null}; } catch (_) { return {roms:{}, cart:null}; } }
function saveManifest(){ localStorage.setItem(STORAGE_MANIFEST, JSON.stringify(manifest)); }
function updateStorageStatus(text, tone='muted') { els.storageStatus.textContent = text; els.storageStatus.className = tone === 'ok' ? 'status-ok' : tone === 'bad' ? 'status-bad' : tone === 'warn' ? 'status-warn' : 'muted'; }
function openDb(){ return new Promise((resolve,reject)=>{ const req=indexedDB.open(DB_NAME,DB_VERSION); req.onupgradeneeded=()=>req.result.createObjectStore(STORE_FILES); req.onsuccess=()=>resolve(req.result); req.onerror=()=>reject(req.error || new Error('IndexedDB open failed')); }); }
function dbPut(key,value){ return new Promise((resolve,reject)=>{ const tx=db.transaction(STORE_FILES,'readwrite'); tx.objectStore(STORE_FILES).put(value,key); tx.oncomplete=()=>resolve(); tx.onerror=()=>reject(tx.error || new Error(`IndexedDB put failed: ${key}`)); }); }
function dbGet(key){ return new Promise((resolve,reject)=>{ const tx=db.transaction(STORE_FILES,'readonly'); const req=tx.objectStore(STORE_FILES).get(key); req.onsuccess=()=>resolve(req.result || null); req.onerror=()=>reject(req.error || new Error(`IndexedDB get failed: ${key}`)); }); }
function dbClear(){ return new Promise((resolve,reject)=>{ const tx=db.transaction(STORE_FILES,'readwrite'); tx.objectStore(STORE_FILES).clear(); tx.oncomplete=()=>resolve(); tx.onerror=()=>reject(tx.error || new Error('IndexedDB clear failed')); }); }
function wasmImports(){ return { loopy:{ read_host_file(){ return 0; } } }; }
async function instantiateBackend(){ let result; try { result = await WebAssembly.instantiateStreaming(fetch('loopy_wasm_core.wasm'), wasmImports()); } catch (_) { const bytes = await (await fetch('loopy_wasm_core.wasm')).arrayBuffer(); result = await WebAssembly.instantiate(bytes, wasmImports()); } wasm = result.instance.exports; memory = wasm.memory; wasm.loopy_wasm_init(); clearCanvasToBlack(); }
function wasmU8(){ return new Uint8Array(memory.buffer); }
function copyBytesToWasm(bytes){ const ptr = wasm.loopy_wasm_malloc(bytes.byteLength || 1); if (!ptr) throw new Error('wasm allocation failed'); wasmU8().set(bytes, ptr); return ptr; }
async function fileToBytes(file){ if (!file) throw new Error('no file selected'); return new Uint8Array(await file.arrayBuffer()); }
function fmtCrc(crc){ return `CRC32 ${((crc>>>0).toString(16).toUpperCase()).padStart(8,'0')}`; }
function crc32(bytes){ let c = 0xffffffff; for (const b of bytes) { c ^= b; for (let k=0;k<8;k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1); } return (~c) >>> 0; }
function expectedCrcFor(kind){ return kind === 'bios' ? EXPECTED_BIOS_CRC : kind === 'sound' ? EXPECTED_SOUND_CRC : null; }
function labelForKind(kind){ return kind === 'bios' ? 'internal ROM' : kind === 'sound' ? 'sound ROM' : 'Wanwan OKI sample ROM'; }
function romTone(crc, expected){ return expected == null || (crc>>>0) === (expected>>>0) ? 'ok' : 'warn'; }
function romStatus(entry, expected, missingText='missing'){ if (!entry) return missingText; const ok = expected == null || (entry.crc>>>0) === (expected>>>0); return `${entry.name} (${entry.size.toLocaleString()} bytes, ${fmtCrc(entry.crc)}${ok || expected == null ? '' : `; expected ${fmtCrc(expected)}`})${ok ? '' : ' — warning: ROM may be invalid'}`; }
function updateRomStatus(){ const bios=manifest.roms?.bios, sound=manifest.roms?.sound, oki=manifest.roms?.oki; els.biosStatus.textContent = romStatus(bios, EXPECTED_BIOS_CRC); els.soundStatus.textContent = romStatus(sound, EXPECTED_SOUND_CRC, 'missing; internal music/synth will be silent'); const okiMissingText = config.wanwanReplacementPcm === false ? 'missing; Wanwan replacement PCM disabled' : 'missing; freely licensed replacement ROM will be used'; els.okiStatus.textContent = romStatus(oki, null, okiMissingText); els.biosStatus.className = bios ? (romTone(bios.crc, EXPECTED_BIOS_CRC)==='ok'?'status-ok':'status-warn') : 'status-bad'; els.soundStatus.className = sound ? (romTone(sound.crc, EXPECTED_SOUND_CRC)==='ok'?'status-ok':'status-warn') : 'status-warn'; els.okiStatus.className = oki ? 'status-ok' : 'status-warn'; els.biosCard.classList.toggle('ready', !!bios && bios.crc === EXPECTED_BIOS_CRC); els.soundCard.classList.toggle('ready', !!sound && sound.crc === EXPECTED_SOUND_CRC); els.okiCard?.classList.toggle('ready', !!oki); els.biosCard.classList.toggle('warn', !!bios && bios.crc !== EXPECTED_BIOS_CRC); els.soundCard.classList.toggle('warn', !sound || sound.crc !== EXPECTED_SOUND_CRC); els.okiCard?.classList.toggle('warn', !oki); }
async function storeRom(kind, file){ if (!file) return; const bytes = await fileToBytes(file); const crc = crc32(bytes); const expected = expectedCrcFor(kind); const key = `rom:${kind}`; await dbPut(key, bytes); manifest.roms[kind] = { key, name:file.name, size:file.size, crc, storedAt:new Date().toISOString() }; saveManifest(); updateRomStatus(); const ok = expected == null || crc === expected; updateStorageStatus(`${labelForKind(kind)} stored: ${fmtCrc(crc)}${ok ? '' : ' (warning: CRC mismatch)'}`, ok ? 'ok' : 'warn'); }
async function loadStoredRom(kind, opts={}){ const entry = manifest.roms?.[kind]; if (!entry) { if (opts.optional) return null; throw new Error(`${labelForKind(kind)} is missing`); } const raw = await dbGet(entry.key); if (!raw) { if (opts.optional) return null; throw new Error(`${entry.name} metadata exists but IndexedDB payload is missing`); } return raw instanceof Uint8Array ? raw : new Uint8Array(raw); }
function asciiContains(bytes, needle, maxLen=0x10000){ if(!bytes || !needle) return false; const n = new TextEncoder().encode(needle.toLowerCase()); const lim = Math.min(bytes.byteLength || bytes.length, maxLen); if(lim < n.length) return false; for(let i=0;i<=lim-n.length;i++){ let ok=true; for(let j=0;j<n.length;j++){ let c=bytes[i+j]; if(c>=65&&c<=90)c+=32; if(c!==n[j]){ok=false;break;} } if(ok)return true; } return false; }
function activeCartIsWanwan(){ return asciiContains(activeCartBytes, 'WAN WAN STORY') || asciiContains(activeCartBytes, 'WANWAN STORY') || asciiContains(activeCartBytes, 'XK-501') || asciiContains(activeCartBytes, 'WANWAN AIJOU'); }
function cartPrefersMouse(){ const n=(activeCartName||'').toLowerCase(); const crc=(manifest.cart?.crc>>>0)||0; return crc===0x550616f0 || n.includes('little romance') || n.includes('littleromance') || n.includes('xk-503'); }
function chooseDefaultPortForCart(){ config.portDevice = cartPrefersMouse() ? 'mouse' : 'gamepad'; els.portDevice.value = config.portDevice; saveConfig(); applyPortDevice({status:false}); }
async function setCartFromFile(file){ if (!file) return; activeCartBytes = await fileToBytes(file); activeCartName = file.name || 'cartridge.bin'; manifest.cart = { name: activeCartName, size: activeCartBytes.byteLength, crc: crc32(activeCartBytes) }; saveManifest(); chooseDefaultPortForCart(); els.cartName.textContent = `${activeCartName} (${activeCartBytes.byteLength.toLocaleString()} bytes, ${fmtCrc(manifest.cart.crc)})`; els.startupCartName.textContent = els.cartName.textContent; updateStorageStatus(`cartridge loaded: ${activeCartName}; default input ${config.portDevice}`, 'ok'); await startOrRestart(); }
async function startOrRestart(){ if (!activeCartBytes) throw new Error('Load a cartridge first'); unlockAudioFromGesture(); wasm.loopy_wasm_reset_heap(); wasm.loopy_wasm_set_wanwan_replacement_pcm?.(config.wanwanReplacementPcm === false ? 0 : 1); const bios = await loadStoredRom('bios'); const sound = await loadStoredRom('sound', {optional:true}); const oki = await loadStoredRom('oki', {optional:true}); let ptr = copyBytesToWasm(bios); const biosCrc = wasm.loopy_wasm_load_bios(ptr, bios.byteLength) >>> 0; let soundCrc = null; if (sound) { ptr = copyBytesToWasm(sound); soundCrc = wasm.loopy_wasm_load_sound_rom(ptr, sound.byteLength) >>> 0; } let okiCrc = null; if (oki && wasm.loopy_wasm_load_oki_rom) { ptr = copyBytesToWasm(oki); okiCrc = wasm.loopy_wasm_load_oki_rom(ptr, oki.byteLength) >>> 0; } ptr = copyBytesToWasm(activeCartBytes); wasm.loopy_wasm_load_cart(ptr, activeCartBytes.byteLength); const sram = await dbGet(sramKey()); if (sram) { const u8 = sram instanceof Uint8Array ? sram : new Uint8Array(sram); ptr = copyBytesToWasm(u8); wasm.loopy_wasm_load_sram(ptr, u8.byteLength); }
  const ok = wasm.loopy_wasm_start(config.portDevice === 'mouse' ? 1 : 0); if (!ok) throw new Error(`backend rejected startup, error 0x${(wasm.loopy_wasm_get_error()>>>0).toString(16)}`); autoPausedForHiddenTab = false; paused = false; running = true; applyPortDevice({status:false}); clearAudioQueue(); resetFrameLimiter(); els.pauseToggle.textContent = 'Pause'; els.startupModal.classList.remove('open'); const soundText = soundCrc == null ? 'sound ROM omitted' : `sound ${fmtCrc(soundCrc)}`; const needsOki = activeCartIsWanwan(); const okiText = needsOki ? (okiCrc == null ? (config.wanwanReplacementPcm === false ? 'Wanwan OKI/PCM disabled' : 'Wanwan OKI replacement bank') : `Wanwan OKI user ROM ${fmtCrc(okiCrc)}`) : 'no Wanwan OKI ROM needed'; updateStorageStatus(`running; BIOS ${fmtCrc(biosCrc)}, ${soundText}, ${okiText}, input ${actualPortDeviceName()}`, (biosCrc===EXPECTED_BIOS_CRC && (soundCrc == null || soundCrc===EXPECTED_SOUND_CRC)) ? 'ok' : 'warn'); }
async function softReset(){ if (!running) { updateStorageStatus('no running machine to reset', 'warn'); return; } const ok = wasm?.loopy_wasm_soft_reset?.(); if (!ok) { updateStorageStatus('soft reset failed', 'bad'); return; } pressed.clear(); resetMouseTracking(); clearAudioQueue(); resetFrameLimiter(); paused = false; autoPausedForHiddenTab = false; els.pauseToggle.textContent = 'Pause'; applyPortDevice({status:false}); await persistSram().catch(()=>{}); updateStorageStatus(`soft reset; input ${actualPortDeviceName()}`, 'ok'); renderOnce(); }
function clearCanvasToBlack(){ ctx.fillStyle = '#000'; ctx.fillRect(0,0,els.canvas.width,els.canvas.height); }
function resizeVideoPresentation(){ const w = wasm?.loopy_wasm_get_width?.() || els.canvas.width || 256; const h = wasm?.loopy_wasm_get_height?.() || els.canvas.height || 240; els.screenFrame.dataset.aspect = config.video.aspect; if (config.video.aspect === 'stretch') { els.canvas.style.width = '100%'; els.canvas.style.height = '100%'; return; } const rect = els.screenFrame.getBoundingClientRect(); const maxW = Math.max(1, rect.width); const maxH = Math.max(1, rect.height); const aspect = config.video.aspect === '4:3' ? (4/3) : (w / h); let cssW = maxW; let cssH = cssW / aspect; if (cssH > maxH) { cssH = maxH; cssW = cssH * aspect; } els.canvas.style.width = `${Math.max(1, Math.floor(cssW))}px`; els.canvas.style.height = `${Math.max(1, Math.floor(cssH))}px`; }
function renderOnce(){ if (!wasm) { resizeVideoPresentation(); return; } const w = wasm.loopy_wasm_get_width(); const h = wasm.loopy_wasm_get_height(); if (els.canvas.width !== w || els.canvas.height !== h) { els.canvas.width = w; els.canvas.height = h; imageData = null; } const ptr = wasm.loopy_wasm_get_framebuffer_rgba(); if (ptr) { const src = wasmU8().subarray(ptr, ptr + w*h*4); if (!imageData || imageData.width !== w || imageData.height !== h) imageData = new ImageData(w,h); imageData.data.set(src); ctx.putImageData(imageData,0,0); } els.runtimeStatus.textContent = STATUS_TEXT[wasm.loopy_wasm_get_status?.() ?? 0] || 'unknown'; const ah = wasm.loopy_wasm_get_active_height?.() || h; els.runtimeResolution.textContent = ah === h ? `${w} × ${h}` : `${w} × ${h} (${ah} active)`; els.runtimeFrame.textContent = String(wasm.loopy_wasm_get_frame_count?.() ?? 0); els.runtimeCart.textContent = activeCartName || 'none'; els.canvas.classList.toggle('smooth', !!config.video.smoothUpscale); els.canvas.classList.toggle('nearest', !config.video.smoothUpscale); els.screenFrame.classList.toggle('scanlines', !!config.video.scanlines); els.fpsCounter.classList.toggle('hidden', !config.video.showFps); els.fpsCounter.textContent = `${fpsValue.toFixed(1)} FPS`; resizeVideoPresentation(); }
function buttonsMask(){ let mask = 0; for (const [name, bit] of BUTTONS) if (pressed.has(config.keys[name]) || virtualPressed.has(name)) mask |= bit; return mask >>> 0; }
function applyTouchOpacity(){ const value = Math.max(0.2, Math.min(1.0, Number(config.touchGamepad?.opacity ?? 0.72))); els.touchGamepad?.style.setProperty('--touch-opacity', String(value)); }
function updateTouchControlsAvailability(){ const hasTouchInput = Number(navigator.maxTouchPoints || 0) > 0 || Number(navigator.msMaxTouchPoints || 0) > 0; const ua = navigator.userAgent || ''; const mobileUserAgent = /Android|iPhone|iPad|iPod|Mobile|Tablet|Kindle|Silk|Windows Phone/i.test(ua); const compactViewport = Math.min(window.innerWidth || 0, window.innerHeight || 0) <= 620 || Math.max(window.innerWidth || 0, window.innerHeight || 0) <= 980; const coarsePointer = !!touchControlsMedia?.matches; const shouldShow = config.portDevice !== 'mouse' && (hasTouchInput || coarsePointer || mobileUserAgent || compactViewport); document.body.classList.toggle('touch-controls-active', shouldShow); }
function releaseVirtualControls(){ virtualPressed.clear(); activeTouchStickId = null; activeTouchStickRect = null; els.touchKnob?.style.setProperty('--stick-x', '0px'); els.touchKnob?.style.setProperty('--stick-y', '0px'); els.touchStick?.classList.remove('active'); document.querySelectorAll('.touch-btn.active').forEach(btn=>btn.classList.remove('active')); }
function setSideMenuOpen(open){ const isOpen = !!open; els.sideMenu.classList.toggle('open', isOpen); document.body.classList.toggle('menu-open', isOpen); if (isOpen) releaseVirtualControls(); }
function resetFrameLimiter(){ limiterLastTime = performance.now(); limiterAccumulator = 0; }
function tick(now){ if (running && !paused) { const elapsed = Math.max(0, Math.min(100, now - limiterLastTime)); limiterLastTime = now; limiterAccumulator = Math.min(limiterAccumulator + elapsed, 1000 / FRAME_RATE * 4); const frameInterval = 1000 / FRAME_RATE; let ran = 0; while (limiterAccumulator >= frameInterval && ran < MAX_EMU_FRAMES_PER_RAF) { wasm.loopy_wasm_frame(buttonsMask(), mouseDX|0, mouseDY|0, mouseButtons>>>0); mouseDX = 0; mouseDY = 0; pullAudio(); maybePersistSram(); checkPrinter(); limiterAccumulator -= frameInterval; ran++; } const fc = wasm.loopy_wasm_get_frame_count(); if (!fpsLastTime) { fpsLastTime = now; fpsLastFrame = fc; } else if (now - fpsLastTime >= 500) { fpsValue = (fc - fpsLastFrame) * 1000 / (now - fpsLastTime); fpsLastTime = now; fpsLastFrame = fc; } } else { limiterLastTime = now; limiterAccumulator = 0; } renderOnce(); frameRequest = requestAnimationFrame(tick); }
function stateKey(){ return `state:${activeCartName || 'cart'}:${config.stateSlot}`; }
function sramKey(){ return `sram:${activeCartName || 'cart'}`; }
async function saveState(){ if (!running) return updateStorageStatus('no running machine to save', 'warn'); const ok = wasm.loopy_wasm_save_state(); if (!ok) return updateStorageStatus('save state failed', 'bad'); const ptr = wasm.loopy_wasm_get_save_ptr(); const size = wasm.loopy_wasm_get_save_size(); const bytes = wasmU8().slice(ptr, ptr + size); await dbPut(stateKey(), bytes); updateStorageStatus(`state slot ${config.stateSlot} saved (${size.toLocaleString()} bytes)`, 'ok'); }
async function loadState(){ const bytes = await dbGet(stateKey()); if (!bytes) return updateStorageStatus(`no state in slot ${config.stateSlot}`, 'warn'); const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes); const wasPaused = paused; clearAudioQueue(); wasm?.loopy_wasm_set_paused?.(1); let ok = 0; if (wasm.loopy_wasm_prepare_load_state && wasm.loopy_wasm_load_prepared_state) { const ptr = wasm.loopy_wasm_prepare_load_state(u8.byteLength); if (ptr) { wasmU8().set(u8, ptr); ok = wasm.loopy_wasm_load_prepared_state(u8.byteLength); } } else { const ptr = copyBytesToWasm(u8); ok = wasm.loopy_wasm_load_state(ptr, u8.byteLength); } clearAudioQueue(); wasm?.loopy_wasm_set_paused?.(wasPaused ? 1 : 0); paused = wasPaused; els.pauseToggle.textContent = paused ? 'Resume' : 'Pause'; resetFrameLimiter(); updateStorageStatus(ok ? `state slot ${config.stateSlot} loaded` : 'state rejected', ok ? 'ok' : 'bad'); renderOnce(); }
let sramPersistCounter = 0;
async function persistSram(){ if (!running || !activeCartName) return; if (!wasm.loopy_wasm_save_sram()) return; const ptr = wasm.loopy_wasm_get_sram_ptr(); const size = wasm.loopy_wasm_get_sram_size(); if (!ptr || !size) return; await dbPut(sramKey(), wasmU8().slice(ptr, ptr + size)); }
function maybePersistSram(){ if (++sramPersistCounter >= 300) { sramPersistCounter = 0; persistSram().catch(()=>{}); } }
async function clearAllStorage(){ for (const k of Object.keys(localStorage)) if (k.startsWith('loopy.wasm.')) localStorage.removeItem(k); if (db) await dbClear(); config = structuredClone(DEFAULT_CONFIG); manifest = {roms:{}, cart:null}; activeCartBytes = null; activeCartName = ''; saveConfig(); saveManifest(); updateRomStatus(); applyConfigToControls(); rebuildControlMap(); clearAudioQueue(); releaseVirtualControls(); wasm?.loopy_wasm_init(); clearCanvasToBlack(); running = false; paused = false; autoPausedForHiddenTab = false; updateStorageStatus('browser storage cleared', 'warn'); els.startupModal.classList.add('open'); renderOnce(); }
function clearAudioQueue(){ audioGeneration = (audioGeneration + 1) >>> 0; audioQueue = []; audioQueueOffset = 0; audioQueueFrames = 0; audioNode?.port?.postMessage({ type:'clear', generation:audioGeneration }); }
function browserHasUserActivation(ev){ if (ev && ev.isTrusted === false) return false; const ua = navigator.userActivation; return !(ua && ua.isActive === false); }
function unlockAudioFromGesture(ev){ if (!browserHasUserActivation(ev)) return; if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint:'interactive', sampleRate:48000 }); if (audioCtx.state !== 'running') audioCtx.resume().catch(()=>{}); ensureAudioOutputInstalled(); }
async function installAudioWorklet(){ if (!audioCtx || !audioCtx.audioWorklet) return false; if (audioWorkletReady && audioNode) return true; if (audioWorkletPromise) return audioWorkletPromise; audioWorkletPromise = (async()=>{ const source = `
const MAX_QUEUE_FRAMES = 4096;
class LoopyPCMProcessor extends AudioWorkletProcessor {
  constructor(){ super(); this.queue=[]; this.offset=0; this.frames=0; this.generation=0; this.port.onmessage=e=>{ const m=e.data||{}; if(m.type==='clear'){ this.generation=m.generation>>>0; this.queue=[]; this.offset=0; this.frames=0; return; } if(m.type==='audio'&&m.buffer){ if((m.generation>>>0)!==this.generation) return; const c=new Int16Array(m.buffer); this.queue.push(c); this.frames += c.length>>1; this.trim(); } }; }
  dropFrames(n){ while(n>0 && this.queue.length){ const front=this.queue[0]; const left=(front.length>>1)-this.offset; if(n>=left){ n-=left; this.frames-=left; this.queue.shift(); this.offset=0; } else { this.offset+=n; this.frames-=n; n=0; } } if(this.frames<0) this.frames=0; }
  trim(){ if(this.frames>MAX_QUEUE_FRAMES) this.dropFrames(this.frames-MAX_QUEUE_FRAMES); }
  process(inputs, outputs){ const out=outputs[0], l=out[0], r=out[1]||out[0]; for(let i=0;i<l.length;i++){ if(!this.queue.length){ l[i]=0; r[i]=0; continue; } const front=this.queue[0]; const si=this.offset*2; l[i]=front[si]/32768; r[i]=front[si+1]/32768; this.offset++; this.frames--; if(this.offset >= (front.length>>1)){ this.queue.shift(); this.offset=0; } } return true; }
}
registerProcessor('${AUDIO_PROCESSOR}', LoopyPCMProcessor);`; const url=URL.createObjectURL(new Blob([source],{type:'text/javascript'})); try { await audioCtx.audioWorklet.addModule(url); if (audioNode && !audioWorkletReady) { try { audioNode.disconnect(); } catch (_) {} audioQueue=[]; audioQueueOffset=0; audioQueueFrames=0; } audioNode = new AudioWorkletNode(audioCtx, AUDIO_PROCESSOR, { numberOfInputs:0, numberOfOutputs:1, outputChannelCount:[2] }); audioNode.connect(audioCtx.destination); audioWorkletReady = true; audioNode.port.postMessage({type:'clear', generation:audioGeneration}); return true; } finally { URL.revokeObjectURL(url); } })(); return audioWorkletPromise.catch(e=>{ audioWorkletPromise=null; throw e; }); }
function dropFallbackAudioFrames(n){ while(n>0 && audioQueue.length){ const front=audioQueue[0]; const left=(front.length>>1)-audioQueueOffset; if(n>=left){ n-=left; audioQueueFrames-=left; audioQueue.shift(); audioQueueOffset=0; } else { audioQueueOffset+=n; audioQueueFrames-=n; n=0; } } if(audioQueueFrames<0) audioQueueFrames=0; }
function installScriptProcessorFallback(){ if (!audioCtx || audioNode) return; audioNode = audioCtx.createScriptProcessor(1024,0,2); audioNode.onaudioprocess = ev => { const l=ev.outputBuffer.getChannelData(0), r=ev.outputBuffer.getChannelData(1); for(let i=0;i<l.length;i++){ if(!audioQueue.length){ l[i]=0; r[i]=0; continue; } const front=audioQueue[0]; const si=audioQueueOffset*2; l[i]=front[si]/32768; r[i]=front[si+1]/32768; audioQueueOffset++; audioQueueFrames--; if(audioQueueOffset >= (front.length>>1)){ audioQueue.shift(); audioQueueOffset=0; } } }; audioNode.connect(audioCtx.destination); }
function ensureAudioOutputInstalled(){ installAudioWorklet().then(ok=>{ if(!ok) installScriptProcessorFallback(); }).catch(()=>installScriptProcessorFallback()); }
function pullAudio(){ if (!wasm) return; let frames = wasm.loopy_wasm_get_audio_frames(); if (!frames) return; if (paused) { wasm.loopy_wasm_audio_consume(frames); return; } if (frames > AUDIO_MAX_POST_FRAMES) { wasm.loopy_wasm_audio_consume(frames - AUDIO_MAX_POST_FRAMES); frames = AUDIO_MAX_POST_FRAMES; } const ptr = wasm.loopy_wasm_get_audio_ptr(); const samples = wasmU8().slice(ptr, ptr + frames*4); wasm.loopy_wasm_audio_consume(frames); if (audioNode?.port && audioWorkletReady) audioNode.port.postMessage({type:'audio', generation:audioGeneration, buffer:samples.buffer}, [samples.buffer]); else { audioQueue.push(new Int16Array(samples.buffer)); audioQueueFrames += frames; if (audioQueueFrames > AUDIO_MAX_POST_FRAMES) dropFallbackAudioFrames(audioQueueFrames - AUDIO_MAX_POST_FRAMES); } }
function rebuildControlMap(){ els.controlMap.innerHTML = ''; for (const [name] of BUTTONS) { const row=document.createElement('div'); row.className='map-row'; row.dataset.name=name; const label=document.createElement('span'); label.textContent=name; const btn=document.createElement('button'); btn.textContent=config.keys[name]; btn.addEventListener('click',()=>{ remapTarget=name; document.querySelectorAll('.map-row').forEach(r=>r.classList.toggle('pending', r.dataset.name===name)); btn.textContent='press key...'; els.screenFrame.focus(); }); row.append(label,btn); els.controlMap.append(row); } }
function populateStateSlots(){ els.stateSlot.innerHTML=''; for(let i=0;i<10;i++){ const opt=document.createElement('option'); opt.value=String(i); opt.textContent=`Slot ${i}`; els.stateSlot.append(opt); } }
function applyConfigToControls(){ els.aspectMode.value = config.video.aspect; els.smoothUpscale.checked = !!config.video.smoothUpscale; els.scanlines.checked = !!config.video.scanlines; els.showFps.checked = config.video.showFps !== false; if (els.wanwanReplacementPcm) els.wanwanReplacementPcm.checked = config.wanwanReplacementPcm !== false; els.stateSlot.value = String(config.stateSlot); els.portDevice.value = config.portDevice; if (els.touchOpacity) els.touchOpacity.value = String(Math.round((config.touchGamepad?.opacity ?? 0.72) * 100)); applyTouchOpacity(); updateTouchControlsAvailability(); renderOnce(); }
function resetMouseTracking(){ mouseLastX = 0; mouseLastY = 0; mouseLastValid = false; mouseFracX = 0; mouseFracY = 0; mouseDX = 0; mouseDY = 0; mouseButtons = 0; }
function actualPortDeviceName(){ return wasm?.loopy_wasm_get_port_device?.() ? 'mouse' : 'gamepad'; }
function syncMouseCaptureUI(){ const locked = document.pointerLockElement === els.canvas || document.pointerLockElement === els.screenFrame; if(els.captureMouse) els.captureMouse.textContent = locked ? 'Release mouse' : 'Capture mouse'; if(els.mouseCaptureStatus) els.mouseCaptureStatus.textContent = config.portDevice === 'mouse' ? (locked ? 'Mouse captured; press Esc to release.' : 'Mouse mode active; click the screen or Capture mouse to lock pointer movement.') : 'Gamepad mode active; mouse capture disabled.'; }
function releaseMouseCapture(){ if(document.pointerLockElement === els.canvas || document.pointerLockElement === els.screenFrame) document.exitPointerLock?.(); resetMouseTracking(); syncMouseCaptureUI(); }
function requestMouseCapture(){ if(config.portDevice !== 'mouse') { config.portDevice='mouse'; els.portDevice.value='mouse'; saveConfig(); applyPortDevice({status:true}); } els.screenFrame.focus(); const target = els.screenFrame.requestPointerLock ? els.screenFrame : els.canvas; try { target.requestPointerLock?.(); } catch (_) {} syncMouseCaptureUI(); }
function applyPortDevice(opts={}){ const wantMouse = config.portDevice === 'mouse'; wasm?.loopy_wasm_set_port_device?.(wantMouse ? 1 : 0); resetMouseTracking(); pressed.clear(); releaseVirtualControls(); if(!wantMouse) releaseMouseCapture(); syncMouseCaptureUI(); updateTouchControlsAvailability(); if(opts.status) updateStorageStatus(`input device set to ${actualPortDeviceName()}`, 'ok'); }
function applyRuntimeOptions(){ config.video.aspect = els.aspectMode.value; config.video.smoothUpscale = els.smoothUpscale.checked; config.video.scanlines = els.scanlines.checked; config.video.showFps = els.showFps.checked; if (els.wanwanReplacementPcm) config.wanwanReplacementPcm = !!els.wanwanReplacementPcm.checked; config.stateSlot = parseInt(els.stateSlot.value,10)||0; if (els.touchOpacity) config.touchGamepad = { opacity: Math.max(0.2, Math.min(1.0, (parseInt(els.touchOpacity.value || '72', 10) || 72) / 100)) }; applyTouchOpacity(); updateRomStatus(); const selectedDevice = els.portDevice.value === 'mouse' ? 'mouse' : 'gamepad'; const oldDevice = config.portDevice; config.portDevice = selectedDevice; saveConfig(); applyPortDevice({status: oldDevice !== selectedDevice}); updateTouchControlsAvailability(); renderOnce(); }
function setPaused(p, opts={}){
  paused = !!p;
  if (!opts.keepHiddenAutoFlag) autoPausedForHiddenTab = false;
  wasm?.loopy_wasm_set_paused(paused ? 1 : 0);
  if (paused) clearAudioQueue();
  else {
    clearAudioQueue();
    resetFrameLimiter();
    if (audioCtx && audioCtx.state !== 'running') audioCtx.resume().catch(()=>{});
  }
  els.pauseToggle.textContent = paused ? 'Resume' : 'Pause';
  renderOnce();
}
function autoResumeBlocked(){
  return els.printModal.classList.contains('open') || els.startupModal.classList.contains('open');
}
function resumeAfterHiddenIfPossible(){
  if (!autoPausedForHiddenTab || document.hidden) return;
  if (running && paused && !autoResumeBlocked()) {
    autoPausedForHiddenTab = false;
    setPaused(false, { keepHiddenAutoFlag:true });
    updateStorageStatus('resumed after tab became visible', 'ok');
  } else {
    els.pauseToggle.textContent = paused ? 'Resume' : 'Pause';
    resetFrameLimiter();
  }
}
function closeRomManager(){
  els.startupModal.classList.remove('open');
  resumeAfterHiddenIfPossible();
}
function handleVisibilityChange(){
  if (document.hidden) {
    persistSram().catch(()=>{});
    if (running && !paused) {
      autoPausedForHiddenTab = true;
      setPaused(true, { keepHiddenAutoFlag:true });
    }
    pressed.clear();
    releaseVirtualControls();
    resetMouseTracking();
    return;
  }
  resumeAfterHiddenIfPossible();
}
function handleWindowFocus(){
  resumeAfterHiddenIfPossible();
}
function setFullscreen(full){ if(full && !document.fullscreenElement) els.screenFrame.requestFullscreen?.(); else if(!full && document.fullscreenElement) document.exitFullscreen?.(); }
function updateFullscreenUI(){ const full=!!document.fullscreenElement; els.screenFrame.classList.toggle('fullscreen-active', full); document.body.classList.toggle('fullscreen-emulation', full); requestAnimationFrame(()=>renderOnce()); }
function checkPrinter(){ if (els.printModal.classList.contains('open')) return; if (!wasm?.loopy_wasm_print_pending?.()) return; const w=wasm.loopy_wasm_print_width(), h=wasm.loopy_wasm_print_height(); const ptr=wasm.loopy_wasm_print_rgba(); if(!ptr||!w||!h) return; releaseMouseCapture(); mouseButtons = 0; pressed.clear(); resetMouseTracking(); els.printCanvas.width=w; els.printCanvas.height=h; const pctx=els.printCanvas.getContext('2d'); const img=new ImageData(w,h); img.data.set(wasmU8().subarray(ptr, ptr+w*h*4)); pctx.putImageData(img,0,0); if (pendingPrintUrl) URL.revokeObjectURL(pendingPrintUrl); els.printCanvas.toBlob(blob=>{ if(!blob) { pendingPrintUrl=''; updateStorageStatus('printer output ready, but PNG encoding failed', 'warn'); return; } pendingPrintUrl = URL.createObjectURL(blob); pendingPrintFilename = `loopy_print_${String(Date.now()).slice(-6)}.png`; }, 'image/png'); els.printModal.classList.add('open'); updateStorageStatus('printer output ready; emulation continues', 'ok'); }
function wireEvents(){
  document.addEventListener('click', ev=>unlockAudioFromGesture(ev), {capture:true, passive:true}); document.addEventListener('keydown', ev=>unlockAudioFromGesture(ev), {capture:true, passive:true});
  els.openStartup.addEventListener('click',()=>els.startupModal.classList.add('open')); els.closeStartup?.addEventListener('click', closeRomManager); els.closeStartupAlt?.addEventListener('click', closeRomManager); els.menuToggle.addEventListener('click',()=>setSideMenuOpen(!els.sideMenu.classList.contains('open'))); els.closeMenu.addEventListener('click',()=>setSideMenuOpen(false));
  els.fullscreenToggle.addEventListener('click',()=>setFullscreen(!document.fullscreenElement)); document.addEventListener('fullscreenchange', updateFullscreenUI); window.addEventListener('resize',()=>requestAnimationFrame(()=>{ updateTouchControlsAvailability(); renderOnce(); })); if (touchControlsMedia?.addEventListener) touchControlsMedia.addEventListener('change', updateTouchControlsAvailability); else touchControlsMedia?.addListener?.(updateTouchControlsAvailability);
  els.biosFile.addEventListener('change',()=>storeRom('bios', els.biosFile.files[0]).catch(e=>updateStorageStatus(e.message,'bad'))); els.soundFile.addEventListener('change',()=>storeRom('sound', els.soundFile.files[0]).catch(e=>updateStorageStatus(e.message,'bad'))); els.okiFile?.addEventListener('change',()=>storeRom('oki', els.okiFile.files[0]).catch(e=>updateStorageStatus(e.message,'bad')));
  els.cartFile.addEventListener('change',()=>setCartFromFile(els.cartFile.files[0]).catch(e=>{ els.cartName.textContent=e.message; updateStorageStatus(e.message,'bad'); })); els.startupCartFile.addEventListener('change',()=>setCartFromFile(els.startupCartFile.files[0]).catch(e=>{ els.startupCartName.textContent=e.message; updateStorageStatus(e.message,'bad'); }));
  els.startButton.addEventListener('click',()=>startOrRestart().catch(e=>{ els.startupError.textContent=e.message; updateStorageStatus(e.message,'bad'); }));
  els.softResetTop?.addEventListener('click',()=>softReset().catch(e=>updateStorageStatus(e.message,'bad')));
  els.pauseToggle.addEventListener('click',()=>setPaused(!paused)); els.resetButton.addEventListener('click',()=>softReset().catch(e=>updateStorageStatus(e.message,'bad')));
  els.saveState.addEventListener('click',()=>saveState().catch(e=>updateStorageStatus(e.message,'bad'))); els.loadState.addEventListener('click',()=>loadState().catch(e=>updateStorageStatus(e.message,'bad'))); els.clearStorage.addEventListener('click',()=>clearAllStorage().catch(e=>updateStorageStatus(e.message,'bad')));
  for (const el of [els.aspectMode, els.smoothUpscale, els.scanlines, els.showFps, els.wanwanReplacementPcm, els.stateSlot, els.portDevice, els.touchOpacity].filter(Boolean)) el.addEventListener('change', applyRuntimeOptions);
  els.downloadPrint.addEventListener('click',()=>{ if(!pendingPrintUrl){ updateStorageStatus('PNG is still being prepared; try again in a moment', 'warn'); return; } const a=document.createElement('a'); a.href=pendingPrintUrl; a.download=pendingPrintFilename || 'loopy_print.png'; document.body.appendChild(a); a.click(); a.remove(); });
  els.closePrint.addEventListener('click',()=>{ els.printModal.classList.remove('open'); wasm?.loopy_wasm_print_clear?.(); if (pendingPrintUrl) { URL.revokeObjectURL(pendingPrintUrl); pendingPrintUrl=''; } resumeAfterHiddenIfPossible(); });
  window.addEventListener('keydown', e=>{ if(remapTarget){ config.keys[remapTarget]=e.code; remapTarget=null; saveConfig(); rebuildControlMap(); e.preventDefault(); return; } if(e.code==='KeyM' && (e.ctrlKey || e.metaKey)){ if(document.pointerLockElement === els.canvas || document.pointerLockElement === els.screenFrame) releaseMouseCapture(); else requestMouseCapture(); e.preventDefault(); return; } if(e.code==='F11'){ setFullscreen(!document.fullscreenElement); e.preventDefault(); return; } if(e.code==='F1'){ setSideMenuOpen(!els.sideMenu.classList.contains('open')); e.preventDefault(); return; } if(e.code==='F5'){ saveState(); e.preventDefault(); return; } if(e.code==='F7'){ loadState(); e.preventDefault(); return; } pressed.add(e.code); if(Object.values(config.keys).includes(e.code)) e.preventDefault(); });
  window.addEventListener('keyup', e=>pressed.delete(e.code)); window.addEventListener('blur',()=>{ pressed.clear(); releaseVirtualControls(); resetMouseTracking(); }); window.addEventListener('focus', handleWindowFocus); document.addEventListener('visibilitychange', handleVisibilityChange); window.addEventListener('pagehide',()=>persistSram().catch(()=>{}));
  els.screenFrame.addEventListener('click', e=>{ if(!e.target.closest('button,a,input,select,label')) els.screenFrame.focus(); });
  els.screenFrame.addEventListener('contextmenu', e=>e.preventDefault());
  const buttonMaskFromButtons = buttons => ((buttons & 1) ? 1 : 0) | ((buttons & 2) ? 2 : 0);
  const buttonMaskFromButton = button => button === 0 ? 1 : button === 2 ? 2 : 0;
  const accumulateMouseDelta = (dx, dy) => { mouseFracX += dx; mouseFracY += dy; const ix = mouseFracX >= 0 ? Math.floor(mouseFracX) : Math.ceil(mouseFracX); const iy = mouseFracY >= 0 ? Math.floor(mouseFracY) : Math.ceil(mouseFracY); if(ix){ mouseDX += ix; mouseFracX -= ix; } if(iy){ mouseDY += iy; mouseFracY -= iy; } };
  const feedPointerMove = e => { if(config.portDevice !== 'mouse') return; let dx = 0, dy = 0; const locked = document.pointerLockElement === els.canvas || document.pointerLockElement === els.screenFrame; if(locked) { dx = e.movementX || 0; dy = e.movementY || 0; } else if(mouseLastValid) { dx = e.clientX - mouseLastX; dy = e.clientY - mouseLastY; } mouseLastX = e.clientX; mouseLastY = e.clientY; mouseLastValid = true; if(typeof e.buttons === 'number') mouseButtons = buttonMaskFromButtons(e.buttons); if(dx || dy) accumulateMouseDelta(dx, dy); };
  els.screenFrame.addEventListener('pointermove', feedPointerMove);
  document.addEventListener('mousemove', e=>{ if(document.pointerLockElement === els.canvas || document.pointerLockElement === els.screenFrame) feedPointerMove(e); });
  els.screenFrame.addEventListener('pointerenter', e=>{ mouseLastX=e.clientX; mouseLastY=e.clientY; mouseLastValid=true; });
  els.screenFrame.addEventListener('pointerleave',()=>{ mouseLastValid=false; if(!document.pointerLockElement) mouseButtons=0; });
  els.screenFrame.addEventListener('pointerdown', e=>{ if(e.target.closest('button,a,input,select,label')) return; const b=buttonMaskFromButton(e.button); if(b) mouseButtons |= b; els.screenFrame.focus(); if(config.portDevice === 'mouse' && document.pointerLockElement !== els.screenFrame && document.pointerLockElement !== els.canvas) requestMouseCapture(); e.preventDefault(); });
  window.addEventListener('pointerup', e=>{ const b=buttonMaskFromButton(e.button); if(b) mouseButtons &= ~b; });
  els.captureMouse?.addEventListener('click',()=>{ if(document.pointerLockElement === els.canvas || document.pointerLockElement === els.screenFrame) releaseMouseCapture(); else requestMouseCapture(); });
  function setVirtualButton(name, down){ if(down) virtualPressed.add(name); else virtualPressed.delete(name); }
  function updateVirtualStick(clientX, clientY){ if(!els.touchStick || !els.touchKnob) return; const rect = activeTouchStickRect || els.touchStick.getBoundingClientRect(); const cx = rect.left + rect.width / 2; const cy = rect.top + rect.height / 2; const max = Math.max(1, rect.width * 0.34); let dx = clientX - cx; let dy = clientY - cy; const len = Math.hypot(dx, dy); if(len > max){ dx = dx * max / len; dy = dy * max / len; } els.touchKnob.style.setProperty('--stick-x', `${dx}px`); els.touchKnob.style.setProperty('--stick-y', `${dy}px`); const dead = rect.width * 0.16; setVirtualButton('Left', dx < -dead); setVirtualButton('Right', dx > dead); setVirtualButton('Up', dy < -dead); setVirtualButton('Down', dy > dead); }
  function releaseVirtualStick(){ activeTouchStickId = null; activeTouchStickRect = null; els.touchKnob?.style.setProperty('--stick-x', '0px'); els.touchKnob?.style.setProperty('--stick-y', '0px'); for(const name of ['Left','Right','Up','Down']) virtualPressed.delete(name); els.touchStick?.classList.remove('active'); }
  els.touchStick?.addEventListener('pointerdown', e=>{ if(config.portDevice === 'mouse') return; unlockAudioFromGesture(e); activeTouchStickId = e.pointerId; activeTouchStickRect = els.touchStick.getBoundingClientRect(); els.touchStick.setPointerCapture?.(e.pointerId); els.touchStick.classList.add('active'); els.screenFrame.focus(); updateVirtualStick(e.clientX, e.clientY); e.preventDefault(); });
  els.touchStick?.addEventListener('pointermove', e=>{ if(e.pointerId === activeTouchStickId){ updateVirtualStick(e.clientX, e.clientY); e.preventDefault(); } });
  els.touchStick?.addEventListener('pointerup', e=>{ if(e.pointerId === activeTouchStickId) releaseVirtualStick(); });
  els.touchStick?.addEventListener('pointercancel', e=>{ if(e.pointerId === activeTouchStickId) releaseVirtualStick(); });
  document.querySelectorAll('.touch-btn').forEach(btn=>{ const name = btn.dataset.button; btn.addEventListener('pointerdown', e=>{ if(config.portDevice === 'mouse') return; unlockAudioFromGesture(e); btn.setPointerCapture?.(e.pointerId); btn.classList.add('active'); setVirtualButton(name, true); els.screenFrame.focus(); e.preventDefault(); }); const up = e=>{ btn.classList.remove('active'); setVirtualButton(name, false); e.preventDefault(); }; btn.addEventListener('pointerup', up); btn.addEventListener('pointercancel', up); btn.addEventListener('pointerleave', e=>{ if(e.buttons === 0){ btn.classList.remove('active'); setVirtualButton(name, false); } }); });
  document.addEventListener('pointerlockchange',()=>{ resetMouseTracking(); syncMouseCaptureUI(); });
  document.addEventListener('pointerlockerror',()=>{ syncMouseCaptureUI(); updateStorageStatus('browser denied mouse capture; click the game screen and try again', 'warn'); });
}
async function boot(){ populateStateSlots(); applyConfigToControls(); updateRomStatus(); rebuildControlMap(); db = await openDb(); await instantiateBackend(); wireEvents(); applyPortDevice({status:false}); updateStorageStatus('IndexedDB ready', 'ok'); if (manifest.roms?.bios) els.startupModal.classList.remove('open'); else els.startupModal.classList.add('open'); frameRequest = requestAnimationFrame(tick); }
boot().catch(err=>updateStorageStatus(`startup failed: ${err.message}`,'bad'));
