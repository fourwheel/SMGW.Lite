#include "webserver_optical.h"
#include "app_globals.h"
#include "serial_scan.h"

// ---------------------------------------------------------------------------
// Virtual flashlight – single IR pulse triggered from the web UI.
// LED polarity: LOW = LED on (standard active-low driver circuit).
// If your circuit is active-high, change OPTICAL_FLASH_LED_ON to HIGH.
//
// Default pulse durations follow the FNN "Lastenheft Basiszähler" optical-
// interface spec that certified German mME meters implement ("kurzer
// Tastendruck" < 2s, "langer Tastendruck" > 5s — verified against Landis+Gyr,
// DZG, Iskraemeco, Apator, EFR and ZPA manuals) but can be overridden per
// request via the "ms" query arg in case a given meter still deviates — the
// PIN Assistant pages expose this as an adjustable setting instead of
// hardcoding one meter's timing.
// ---------------------------------------------------------------------------
#define OPTICAL_FLASH_LED_ON  LOW   // level that lights up the IR LED
#define OPTICAL_FLASH_MS      300   // default short pulse duration [ms]
#define OPTICAL_FLASH_LONG_MS 6000  // default long pulse duration for confirm [ms]
#define OPTICAL_FLASH_MS_MIN       50
#define OPTICAL_FLASH_MS_MAX       4000
#define OPTICAL_FLASH_LONG_MS_MIN  2000
#define OPTICAL_FLASH_LONG_MS_MAX  20000

// ---------------------------------------------------------------------------
// Webserver_FlashPulse – fire one short IR pulse and return immediately.
// Optional "ms" query arg overrides the default duration (clamped).
// ---------------------------------------------------------------------------
void Webserver_FlashPulse()
{
  long ms = server.hasArg("ms") ? server.arg("ms").toInt() : OPTICAL_FLASH_MS;
  if (ms < OPTICAL_FLASH_MS_MIN) ms = OPTICAL_FLASH_MS_MIN;
  if (ms > OPTICAL_FLASH_MS_MAX) ms = OPTICAL_FLASH_MS_MAX;

  mySerial.end();
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, OPTICAL_FLASH_LED_ON);
  delay(ms);
  digitalWrite(TX_PIN, !OPTICAL_FLASH_LED_ON);
  mySerial.begin(SerialScan_getActiveBaud(), SerialScan_getActiveConfig(), RX_PIN, TX_PIN);
  server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------------------------
// Webserver_FlashLongPulse – fire one long IR pulse (confirm/select).
// Optional "ms" query arg overrides the default duration (clamped).
// ---------------------------------------------------------------------------
void Webserver_FlashLongPulse()
{
  long ms = server.hasArg("ms") ? server.arg("ms").toInt() : OPTICAL_FLASH_LONG_MS;
  if (ms < OPTICAL_FLASH_LONG_MS_MIN) ms = OPTICAL_FLASH_LONG_MS_MIN;
  if (ms > OPTICAL_FLASH_LONG_MS_MAX) ms = OPTICAL_FLASH_LONG_MS_MAX;

  mySerial.end();
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, OPTICAL_FLASH_LED_ON);
  delay(ms);
  digitalWrite(TX_PIN, !OPTICAL_FLASH_LED_ON);
  mySerial.begin(SerialScan_getActiveBaud(), SerialScan_getActiveConfig(), RX_PIN, TX_PIN);
  server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------------------------
// Webserver_Flashlight – PIN entry pad for the optical meter interface.
// ---------------------------------------------------------------------------
void Webserver_Flashlight()
{
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang='de'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'>
<title>SmartMeterLite – PIN Assistant</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  html, body {
    height: 100%; width: 100%;
    background: #111;
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    color: #eee;
    touch-action: manipulation;
  }
  body {
    display: flex; flex-direction: column;
    align-items: center; justify-content: center;
    gap: .4rem; padding: .5rem; min-height: 100vh;
  }
  h1 { font-size: .85rem; color: #bbb; letter-spacing: .05em; text-align: center; }
  #eye-notice {
    width: min(290px, 92vw);
    display: flex; align-items: center; gap: .4rem;
    background: rgba(255,255,255,.05);
    border: 1px solid #2a2a2a; border-radius: 8px;
    padding: .3rem .55rem;
    font-size: .68rem; color: #9fcfff; line-height: 1.3; text-align: left;
  }
  #eye-notice .icon { font-size: 1rem; line-height: 1; flex: none; }
  #hint { font-size: .68rem; color: #666; text-align: center; max-width: 280px; line-height: 1.3; }
  #top-section {
    display: flex; flex-direction: column;
    align-items: center; gap: .3rem;
    width: min(290px, 92vw);
    padding-bottom: .4rem;
    border-bottom: 1px solid #2a2a2a;
  }
  .numpad {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: .35rem;
    width: min(290px, 92vw);
  }
  .btn {
    background: #222; color: #eee;
    border: 1px solid #333; border-radius: 10px;
    font-size: 1.1rem; font-weight: 700;
    height: 42px;
    cursor: pointer;
    touch-action: manipulation;
    -webkit-tap-highlight-color: transparent;
    user-select: none;
    transition: background .1s;
  }
  .btn:active:not(:disabled) { background: #444; }
  .btn:disabled { opacity: .35; cursor: default; }
  .btn-weiter {
    width: 100%; height: 40px;
    font-size: .92rem;
    background: #1a3799; border-color: #2a4aaa; color: #fff;
  }
  .btn-weiter:active:not(:disabled) { background: #0d2577; }
  .btn-confirm {
    width: 100%; height: 40px;
    font-size: .92rem;
    background: #5a3799; border-color: #6a4aaa; color: #fff;
  }
  .btn-confirm:active:not(:disabled) { background: #3d2577; }
  #status-area {
    width: min(290px, 92vw); min-height: 18px;
    text-align: center;
    display: flex; flex-direction: row; flex-wrap: wrap;
    justify-content: center; align-items: baseline;
    column-gap: .4rem; row-gap: .15rem;
  }
  #status-text  { font-size: .78rem; color: #aaa; }
  #progress-text { font-size: .7rem; color: #888; }
  #countdown-wrap {
    display: none; flex-basis: 100%;
    width: 100%; background: #2a2a2a; border-radius: 6px;
    height: 5px; overflow: hidden;
  }
  #countdown-bar { height: 100%; background: #1a3799; width: 100%; }
  #back-link {
    color: #aaa; font-size: .68rem; text-decoration: none;
    border: 1px solid #444; border-radius: 6px;
    padding: .2rem .7rem;
  }
  #back-link:hover { background: #222; color: #fff; }
  #timing-settings {
    width: min(290px, 92vw); font-size: .66rem; color: #888;
    border: 1px solid #2a2a2a; border-radius: 8px; padding: .35rem .55rem;
  }
  #timing-settings summary { cursor: pointer; color: #aaa; }
  .cfg-row {
    display: flex; align-items: center; justify-content: space-between;
    gap: .5rem; margin-top: .35rem;
  }
  .cfg-row input {
    width: 70px; background: #222; color: #eee;
    border: 1px solid #333; border-radius: 6px;
    padding: .2rem .35rem; font-size: .7rem; text-align: right;
  }
</style>
</head>
<body>
<h1>&#128274; PIN Assistant</h1>
<div id='eye-notice'><span class='icon'>&#128064;</span><span>Behalte w&#228;hrend der Eingabe das Display deines Z&#228;hlers im Blick &#8212; die App sieht es nicht f&#252;r dich.</span></div>
<div id='top-section'>
  <p id='hint'>Mit &#8222;Weiter&#8220; zur PIN-Abfrage bl&#228;ttern, dann Ziffern eingeben.</p>
  <button class='btn btn-weiter' onclick='weiter()'>Weiter &#8594;</button>
</div>
<div id='status-area'>
  <div id='status-text'></div>
  <div id='progress-text'></div>
  <div id='countdown-wrap'><div id='countdown-bar'></div></div>
</div>
<div class='numpad'>
  <button class='btn' onclick='digit(1)'>1</button>
  <button class='btn' onclick='digit(2)'>2</button>
  <button class='btn' onclick='digit(3)'>3</button>
  <button class='btn' onclick='digit(4)'>4</button>
  <button class='btn' onclick='digit(5)'>5</button>
  <button class='btn' onclick='digit(6)'>6</button>
  <button class='btn' onclick='digit(7)'>7</button>
  <button class='btn' onclick='digit(8)'>8</button>
  <button class='btn' onclick='digit(9)'>9</button>
  <button class='btn' onclick='digit(0)'>0</button>
</div>
<hr style='width:min(290px,92vw); border:none; border-top:1px solid #2a2a2a;'>
<div id='top-section' style='border-bottom:none; padding-bottom:0;'>
  <p id='hint'>Mit &#8222;Weiter&#8220; zu &#8222;PIN&#8220; (soll: <span style='color:#e05252;font-weight:700;'>Off</span>) bzw. &#8222;INF&#8220; (soll: <span style='color:#4caf50;font-weight:700;'>ON</span>) bl&#228;ttern. Button &#228;ndert die Anzeige (langer Puls), &#8222;Weiter&#8220; &#252;bernimmt sie.</p>
  <button class='btn btn-confirm' onclick='bestaetigen()'>Zustand wechseln</button>
</div>
<details id='timing-settings'>
  <summary>Timing-Einstellungen (je nach Z&#228;hlermodell)</summary>
  <div class='cfg-row'><label for='cfg-short'>Kurzer Puls (ms)</label><input id='cfg-short' type='number' min='50' max='4000' step='50'></div>
  <div class='cfg-row'><label for='cfg-long'>Langer Puls (ms)</label><input id='cfg-long' type='number' min='2000' max='20000' step='100'></div>
  <div class='cfg-row'><label for='cfg-gap'>Pulspause (ms)</label><input id='cfg-gap' type='number' min='50' max='5000' step='50'></div>
  <div class='cfg-row'><label for='cfg-settle'>Wartezeit &#252;bernahme (ms)</label><input id='cfg-settle' type='number' min='500' max='10000' step='100'></div>
</details>
<a id='back-link' href='/'>&#8592; Go Back</a>
<script>
  var DEFAULTS = { shortMs: 300, longMs: 6000, gapMs: 400, settleMs: 3000 };
  var cfg = loadTiming();

  function loadTiming() {
    try {
      var saved = JSON.parse(localStorage.getItem('opticalTiming') || '{}');
      return {
        shortMs:  saved.shortMs  || DEFAULTS.shortMs,
        longMs:   saved.longMs   || DEFAULTS.longMs,
        gapMs:    saved.gapMs    || DEFAULTS.gapMs,
        settleMs: saved.settleMs || DEFAULTS.settleMs
      };
    } catch (e) { return Object.assign({}, DEFAULTS); }
  }
  function saveTiming() {
    try { localStorage.setItem('opticalTiming', JSON.stringify(cfg)); } catch (e) {}
  }

  var cfgShort = document.getElementById('cfg-short');
  var cfgLong  = document.getElementById('cfg-long');
  var cfgGap   = document.getElementById('cfg-gap');
  var cfgSettle = document.getElementById('cfg-settle');
  cfgShort.value = cfg.shortMs;
  cfgLong.value = cfg.longMs;
  cfgGap.value = cfg.gapMs;
  cfgSettle.value = cfg.settleMs;
  function bindCfg(input, key) {
    input.addEventListener('change', function() {
      var v = parseInt(input.value, 10);
      if (!isNaN(v) && v > 0) { cfg[key] = v; saveTiming(); }
    });
  }
  bindCfg(cfgShort, 'shortMs');
  bindCfg(cfgLong, 'longMs');
  bindCfg(cfgGap, 'gapMs');
  bindCfg(cfgSettle, 'settleMs');

  var btns  = document.querySelectorAll('.btn');
  var sTxt  = document.getElementById('status-text');
  var pTxt  = document.getElementById('progress-text');
  var cdWrap = document.getElementById('countdown-wrap');
  var cdBar  = document.getElementById('countdown-bar');

  function lock() { btns.forEach(function(b) { b.disabled = true; }); }
  function unlock() {
    sTxt.textContent = '';
    pTxt.textContent = '';
    cdWrap.style.display = 'none';
    cdBar.style.transition = 'none';
    cdBar.style.width = '100%';
    btns.forEach(function(b) { b.disabled = false; });
  }

  function wait(ms) { return new Promise(function(r) { setTimeout(r, ms); }); }
  async function pulse() { await fetch('/flash?ms=' + cfg.shortMs); }
  async function pulseLong() { await fetch('/flashlong?ms=' + cfg.longMs); }

  async function countdown() {
    sTxt.textContent = '⏳ Nächste Stelle wird aktiv — dann nächste Ziffer eingeben';
    cdWrap.style.display = 'block';
    cdBar.style.transition = 'none';
    cdBar.style.width = '100%';
    cdBar.getBoundingClientRect();
    cdBar.style.transition = 'width ' + (cfg.settleMs / 1000) + 's linear';
    cdBar.style.width = '0%';
    var rem = Math.round(cfg.settleMs / 1000);
    pTxt.textContent = 'Warte ' + rem + 's …';
    var iv = setInterval(function() {
      rem--;
      pTxt.textContent = rem > 0 ? 'Warte ' + rem + 's …' : '';
    }, 1000);
    await wait(cfg.settleMs);
    clearInterval(iv);
  }

  async function digit(n) {
    lock();
    try {
      if (n === 0) {
        await countdown();
      } else {
        for (var i = 1; i <= n; i++) {
          pTxt.textContent = 'Puls ' + i + ' / ' + n;
          await pulse();
          if (i < n) await wait(cfg.gapMs);
        }
        await countdown();
      }
    } catch(e) {
      sTxt.textContent = 'Verbindungsfehler';
    }
    unlock();
  }

  async function weiter() {
    lock();
    try {
      pTxt.textContent = 'Puls 1 / 1';
      await pulse();
    } catch(e) {
      sTxt.textContent = 'Verbindungsfehler';
    }
    unlock();
  }

  async function bestaetigen() {
    lock();
    try {
      sTxt.textContent = 'Langer Puls …';
      await pulseLong();
    } catch(e) {
      sTxt.textContent = 'Verbindungsfehler';
    }
    unlock();
    sTxt.textContent = '';
  }
</script>
</body>
</html>
)rawliteral");
}

// ---------------------------------------------------------------------------
// Webserver_PinAssistantDeluxe – type the 4-digit PIN, all pulses sent auto.
// ---------------------------------------------------------------------------
void Webserver_PinAssistantDeluxe()
{
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang='de'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'>
<title>SmartMeterLite – PIN Assistant Deluxe</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  html, body {
    height: 100%; width: 100%;
    background: #111;
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    color: #eee;
    touch-action: manipulation;
  }
  body {
    display: flex; flex-direction: column;
    align-items: center; justify-content: center;
    gap: .32rem; padding: .5rem; min-height: 100vh;
  }
  h1 { font-size: .85rem; color: #bbb; letter-spacing: .05em; text-align: center; }
  #eye-notice {
    width: min(290px, 92vw);
    display: flex; align-items: center; gap: .4rem;
    background: rgba(255,255,255,.05);
    border: 1px solid #2a2a2a; border-radius: 8px;
    padding: .3rem .55rem;
    font-size: .68rem; color: #9fcfff; line-height: 1.3; text-align: left;
  }
  #eye-notice .icon { font-size: 1rem; line-height: 1; flex: none; }
  #pin-input {
    width: min(290px, 92vw);
    background: #222; color: #eee;
    border: 1px solid #444; border-radius: 10px;
    font-size: 1.6rem; font-weight: 700; letter-spacing: .4em;
    text-align: center; padding: .4rem .5rem;
    outline: none;
  }
  #pin-input:focus { border-color: #1a3799; }
  #pin-input::placeholder { letter-spacing: 0; font-weight: 400; font-size: 1rem; color: #555; }
  #pin-error { font-size: .7rem; color: #c0392b; text-align: center; min-height: 1em; }
  .divider {
    width: min(290px, 92vw);
    border: none; border-top: 1px solid #2a2a2a;
  }
  #top-section {
    display: flex; flex-direction: column;
    align-items: center; gap: .3rem;
    width: min(290px, 92vw);
  }
  #hint { font-size: .68rem; color: #666; text-align: center; max-width: 280px; line-height: 1.3; }
  .btn {
    background: #222; color: #eee;
    border: 1px solid #333; border-radius: 10px;
    font-size: .92rem; font-weight: 700;
    height: 40px; width: 100%;
    cursor: pointer;
    touch-action: manipulation;
    -webkit-tap-highlight-color: transparent;
    user-select: none;
    transition: background .1s;
  }
  .btn:active:not(:disabled) { background: #444; }
  .btn:disabled { opacity: .35; cursor: default; }
  .btn-weiter { background: #1a3799; border-color: #2a4aaa; color: #fff; }
  .btn-weiter:active:not(:disabled) { background: #0d2577; }
  .btn-confirm { background: #5a3799; border-color: #6a4aaa; color: #fff; }
  .btn-confirm:active:not(:disabled) { background: #3d2577; }
  .btn-transfer { background: #1a6b3a; border-color: #2a8a4a; color: #fff; }
  .btn-transfer:active:not(:disabled) { background: #0d4a28; }
  #status-area {
    width: min(290px, 92vw); min-height: 18px;
    text-align: center;
    display: flex; flex-direction: row; flex-wrap: wrap;
    justify-content: center; align-items: baseline;
    column-gap: .4rem; row-gap: .15rem;
  }
  #status-text   { font-size: .78rem; color: #aaa; }
  #progress-text { font-size: .7rem;  color: #888; }
  #countdown-wrap {
    display: none; flex-basis: 100%;
    width: 100%; background: #2a2a2a; border-radius: 6px;
    height: 5px; overflow: hidden;
  }
  #countdown-bar { height: 100%; background: #1a3799; width: 100%; }
  #back-link {
    color: #aaa; font-size: .68rem; text-decoration: none;
    border: 1px solid #444; border-radius: 6px;
    padding: .2rem .7rem;
  }
  #back-link:hover { background: #222; color: #fff; }
  #timing-settings {
    width: min(290px, 92vw); font-size: .66rem; color: #888;
    border: 1px solid #2a2a2a; border-radius: 8px; padding: .35rem .55rem;
  }
  #timing-settings summary { cursor: pointer; color: #aaa; }
  .cfg-row {
    display: flex; align-items: center; justify-content: space-between;
    gap: .5rem; margin-top: .35rem;
  }
  .cfg-row input {
    width: 70px; background: #222; color: #eee;
    border: 1px solid #333; border-radius: 6px;
    padding: .2rem .35rem; font-size: .7rem; text-align: right;
  }
</style>
</head>
<body>
<h1>&#128274; PIN Assistant Deluxe</h1>
<div id='eye-notice'><span class='icon'>&#128064;</span><span>Behalte w&#228;hrend der Eingabe das Display deines Z&#228;hlers im Blick &#8212; die App sieht es nicht f&#252;r dich.</span></div>

<label for='pin-input' style='font-size:.68rem; color:#666; letter-spacing:.05em;'>PIN eingeben</label>
<input id='pin-input' type='text' inputmode='numeric' pattern='[0-9]*'
       maxlength='4' placeholder='z.&#8239;B. 1234' autocomplete='off'>
<div id='pin-error'></div>

<hr class='divider'>

<div id='top-section'>
  <p id='hint'>Mit &#8222;Weiter&#8220; zur PIN-Abfrage bl&#228;ttern.</p>
  <button class='btn btn-weiter' onclick='weiter()'>Weiter &#8594;</button>
</div>

<hr class='divider'>

<div id='top-section'>
  <p id='hint'>Bei der PIN-Eingabe angekommen? PIN &#252;bertragen.</p>
  <button class='btn btn-transfer' onclick='transferPin()'>PIN &#252;bertragen &#8594;</button>
  <div id='status-area'>
    <div id='status-text'></div>
    <div id='progress-text'></div>
    <div id='countdown-wrap'><div id='countdown-bar'></div></div>
  </div>
</div>
<hr class='divider'>
<div id='top-section'>
  <p id='hint'>Mit &#8222;Weiter&#8220; zu &#8222;PIN&#8220; (soll: <span style='color:#e05252;font-weight:700;'>Off</span>) bzw. &#8222;INF&#8220; (soll: <span style='color:#4caf50;font-weight:700;'>ON</span>) bl&#228;ttern. Button &#228;ndert die Anzeige (langer Puls), &#8222;Weiter&#8220; &#252;bernimmt sie.</p>
  <button class='btn btn-confirm' onclick='bestaetigen()'>Zustand wechseln</button>
</div>
<details id='timing-settings'>
  <summary>Timing-Einstellungen (je nach Z&#228;hlermodell)</summary>
  <div class='cfg-row'><label for='cfg-short'>Kurzer Puls (ms)</label><input id='cfg-short' type='number' min='50' max='4000' step='50'></div>
  <div class='cfg-row'><label for='cfg-long'>Langer Puls (ms)</label><input id='cfg-long' type='number' min='2000' max='20000' step='100'></div>
  <div class='cfg-row'><label for='cfg-gap'>Pulspause (ms)</label><input id='cfg-gap' type='number' min='50' max='5000' step='50'></div>
  <div class='cfg-row'><label for='cfg-settle'>Wartezeit &#252;bernahme (ms)</label><input id='cfg-settle' type='number' min='500' max='10000' step='100'></div>
</details>
<a id='back-link' href='/'>&#8592; Go Back</a>

<script>
  var DEFAULTS = { shortMs: 300, longMs: 6000, gapMs: 400, settleMs: 3000 };
  var cfg = loadTiming();

  function loadTiming() {
    try {
      var saved = JSON.parse(localStorage.getItem('opticalTiming') || '{}');
      return {
        shortMs:  saved.shortMs  || DEFAULTS.shortMs,
        longMs:   saved.longMs   || DEFAULTS.longMs,
        gapMs:    saved.gapMs    || DEFAULTS.gapMs,
        settleMs: saved.settleMs || DEFAULTS.settleMs
      };
    } catch (e) { return Object.assign({}, DEFAULTS); }
  }
  function saveTiming() {
    try { localStorage.setItem('opticalTiming', JSON.stringify(cfg)); } catch (e) {}
  }

  var cfgShort = document.getElementById('cfg-short');
  var cfgLong  = document.getElementById('cfg-long');
  var cfgGap   = document.getElementById('cfg-gap');
  var cfgSettle = document.getElementById('cfg-settle');
  cfgShort.value = cfg.shortMs;
  cfgLong.value = cfg.longMs;
  cfgGap.value = cfg.gapMs;
  cfgSettle.value = cfg.settleMs;
  function bindCfg(input, key) {
    input.addEventListener('change', function() {
      var v = parseInt(input.value, 10);
      if (!isNaN(v) && v > 0) { cfg[key] = v; saveTiming(); }
    });
  }
  bindCfg(cfgShort, 'shortMs');
  bindCfg(cfgLong, 'longMs');
  bindCfg(cfgGap, 'gapMs');
  bindCfg(cfgSettle, 'settleMs');

  var allBtns = document.querySelectorAll('.btn');
  var pinInput = document.getElementById('pin-input');
  var pinError = document.getElementById('pin-error');
  var sTxt     = document.getElementById('status-text');
  var pTxt     = document.getElementById('progress-text');
  var cdWrap   = document.getElementById('countdown-wrap');
  var cdBar    = document.getElementById('countdown-bar');

  function lock() {
    allBtns.forEach(function(b) { b.disabled = true; });
    pinInput.disabled = true;
  }
  function unlock() {
    allBtns.forEach(function(b) { b.disabled = false; });
    pinInput.disabled = false;
    pTxt.textContent = '';
    cdWrap.style.display = 'none';
    cdBar.style.transition = 'none';
    cdBar.style.width = '100%';
  }

  function wait(ms) { return new Promise(function(r) { setTimeout(r, ms); }); }
  async function pulse() { await fetch('/flash?ms=' + cfg.shortMs); }
  async function pulseLong() { await fetch('/flashlong?ms=' + cfg.longMs); }

  async function countdown(label) {
    sTxt.textContent = label || '⏳ Nächste Stelle wird aktiv';
    cdWrap.style.display = 'block';
    cdBar.style.transition = 'none';
    cdBar.style.width = '100%';
    cdBar.getBoundingClientRect();
    cdBar.style.transition = 'width ' + (cfg.settleMs / 1000) + 's linear';
    cdBar.style.width = '0%';
    var rem = Math.round(cfg.settleMs / 1000);
    pTxt.textContent = 'Warte ' + rem + 's …';
    var iv = setInterval(function() {
      rem--;
      pTxt.textContent = rem > 0 ? 'Warte ' + rem + 's …' : '';
    }, 1000);
    await wait(cfg.settleMs);
    clearInterval(iv);
  }

  async function weiter() {
    lock();
    try {
      pTxt.textContent = 'Puls 1 / 1';
      await pulse();
    } catch(e) {
      sTxt.textContent = 'Verbindungsfehler';
    }
    unlock();
    sTxt.textContent = '';
  }

  async function bestaetigen() {
    lock();
    try {
      sTxt.textContent = 'Langer Puls …';
      await pulseLong();
    } catch(e) {
      sTxt.textContent = 'Verbindungsfehler';
    }
    unlock();
    sTxt.textContent = '';
  }

  async function transferPin() {
    pinError.textContent = '';
    var pin = pinInput.value.trim();
    if (!/^[0-9]{4}$/.test(pin)) {
      pinError.textContent = 'Bitte eine vierstellige PIN eingeben.';
      return;
    }
    lock();
    try {
      for (var i = 0; i < 4; i++) {
        var d = parseInt(pin[i]);
        var stelle = 'Stelle ' + (i + 1) + ' / 4';
        if (d === 0) {
          await countdown(stelle + ': Ziffer 0 — warte …');
        } else {
          for (var p = 1; p <= d; p++) {
            sTxt.textContent = stelle;
            pTxt.textContent = 'Puls ' + p + ' / ' + d;
            await pulse();
            if (p < d) await wait(cfg.gapMs);
          }
          await countdown(stelle + ': Übernehme …');
        }
      }
      sTxt.textContent = '✓ PIN übertragen';
      pTxt.textContent = '';
    } catch(e) {
      sTxt.textContent = 'Verbindungsfehler';
    }
    unlock();
  }
</script>
</body>
</html>
)rawliteral");
}
