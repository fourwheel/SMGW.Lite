#include "webserver_optical.h"
#include "app_globals.h"
#include "serial_scan.h"

// ---------------------------------------------------------------------------
// Virtual flashlight – single IR pulse triggered from the web UI.
// LED polarity: LOW = LED on (standard active-low driver circuit).
// If your circuit is active-high, change OPTICAL_FLASH_LED_ON to HIGH.
// ---------------------------------------------------------------------------
#define OPTICAL_FLASH_LED_ON  LOW   // level that lights up the IR LED
#define OPTICAL_FLASH_MS      300   // short pulse duration [ms]
#define OPTICAL_FLASH_LONG_MS 1500  // long pulse duration for confirm [ms]

// ---------------------------------------------------------------------------
// Webserver_FlashPulse – fire one short IR pulse and return immediately.
// ---------------------------------------------------------------------------
void Webserver_FlashPulse()
{
  mySerial.end();
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, OPTICAL_FLASH_LED_ON);
  delay(OPTICAL_FLASH_MS);
  digitalWrite(TX_PIN, !OPTICAL_FLASH_LED_ON);
  mySerial.begin(SerialScan_getActiveBaud(), SerialScan_getActiveConfig(), RX_PIN, TX_PIN);
  server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------------------------
// Webserver_FlashLongPulse – fire one long IR pulse (confirm/select).
// ---------------------------------------------------------------------------
void Webserver_FlashLongPulse()
{
  mySerial.end();
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, OPTICAL_FLASH_LED_ON);
  delay(OPTICAL_FLASH_LONG_MS);
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
    gap: .9rem; padding: 1rem; min-height: 100vh;
  }
  h1 { font-size: 1rem; color: #bbb; letter-spacing: .05em; text-align: center; }
  #hint { font-size: .8rem; color: #666; text-align: center; max-width: 280px; line-height: 1.45; }
  #top-section {
    display: flex; flex-direction: column;
    align-items: center; gap: .65rem;
    width: min(300px, 90vw);
    padding-bottom: .9rem;
    border-bottom: 1px solid #2a2a2a;
  }
  .numpad {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: .5rem;
    width: min(300px, 90vw);
  }
  .btn {
    background: #222; color: #eee;
    border: 1px solid #333; border-radius: 10px;
    font-size: 1.3rem; font-weight: 700;
    height: 58px;
    cursor: pointer;
    touch-action: manipulation;
    -webkit-tap-highlight-color: transparent;
    user-select: none;
    transition: background .1s;
  }
  .btn:active:not(:disabled) { background: #444; }
  .btn:disabled { opacity: .35; cursor: default; }
  .btn-weiter {
    width: 100%; height: 52px;
    font-size: 1rem;
    background: #1a3799; border-color: #2a4aaa; color: #fff;
  }
  .btn-weiter:active:not(:disabled) { background: #0d2577; }
  .btn-confirm {
    width: 100%; height: 52px;
    font-size: 1rem;
    background: #5a3799; border-color: #6a4aaa; color: #fff;
  }
  .btn-confirm:active:not(:disabled) { background: #3d2577; }
  #status-area {
    width: min(300px, 90vw); min-height: 48px;
    text-align: center;
    display: flex; flex-direction: column; gap: .3rem;
  }
  #status-text  { font-size: .88rem; color: #aaa; }
  #progress-text { font-size: .8rem; color: #888; }
  #countdown-wrap {
    display: none;
    width: 100%; background: #2a2a2a; border-radius: 6px;
    height: 7px; overflow: hidden;
  }
  #countdown-bar { height: 100%; background: #1a3799; width: 100%; }
  #back-link {
    color: #aaa; font-size: .8rem; text-decoration: none;
    border: 1px solid #444; border-radius: 6px;
    padding: .3rem .9rem;
  }
  #back-link:hover { background: #222; color: #fff; }
</style>
</head>
<body>
<h1>&#128274; PIN Assistant</h1>
<div id='top-section'>
  <p id='hint'>Navigiere mit einem Klick auf Weiter durchs Men&#252;, bis du zur PIN-Eingabe aufgefordert wirst. Gib dann die PIN &#252;ber die Tasten ein.</p>
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
<hr style='width:min(300px,90vw); border:none; border-top:1px solid #2a2a2a;'>
<div id='top-section'>
  <p id='hint'>Bl&#228;tter mit &#8222;Weiter&#8220; durchs Men&#252;. Stelle sicher, dass &#8222;PIN&#8220; auf <span style='color:#e05252;font-weight:700;'>Off</span> steht und &#8222;INF&#8220; auf <span style='color:#4caf50;font-weight:700;'>ON</span>. Wenn nicht, &#228;ndere den Zustand mit dem Button.</p>
  <button class='btn btn-confirm' onclick='bestaetigen()'>Zustand wechseln</button>
</div>
<a id='back-link' href='/'>&#8592; Go Back</a>
<script>
  var PULSE_GAP = 400;
  var SETTLE    = 3000;

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
  async function pulse() { await fetch('/flash'); }

  async function countdown() {
    sTxt.textContent = '⏳ Nächste Stelle wird aktiv — dann nächste Ziffer eingeben';
    cdWrap.style.display = 'block';
    cdBar.style.transition = 'none';
    cdBar.style.width = '100%';
    cdBar.getBoundingClientRect();
    cdBar.style.transition = 'width ' + (SETTLE / 1000) + 's linear';
    cdBar.style.width = '0%';
    var rem = Math.round(SETTLE / 1000);
    pTxt.textContent = 'Warte ' + rem + 's …';
    var iv = setInterval(function() {
      rem--;
      pTxt.textContent = rem > 0 ? 'Warte ' + rem + 's …' : '';
    }, 1000);
    await wait(SETTLE);
    clearInterval(iv);
  }

  async function digit(n) {
    lock();
    try {
      if (n === 0) {
        unlock();
        sTxt.textContent = 'Warten, bis nächste Ziffer aktiv';
        return;
      } else {
        for (var i = 1; i <= n; i++) {
          pTxt.textContent = 'Puls ' + i + ' / ' + n;
          await pulse();
          if (i < n) await wait(PULSE_GAP);
        }
      }
      await countdown();
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
      await fetch('/flashlong');
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
    gap: .9rem; padding: 1rem; min-height: 100vh;
  }
  h1 { font-size: 1rem; color: #bbb; letter-spacing: .05em; text-align: center; }
  #pin-input {
    width: min(300px, 90vw);
    background: #222; color: #eee;
    border: 1px solid #444; border-radius: 10px;
    font-size: 2rem; font-weight: 700; letter-spacing: .4em;
    text-align: center; padding: .6rem .5rem;
    outline: none;
  }
  #pin-input:focus { border-color: #1a3799; }
  #pin-input::placeholder { letter-spacing: 0; font-weight: 400; font-size: 1.2rem; color: #555; }
  #pin-error { font-size: .8rem; color: #c0392b; text-align: center; min-height: 1.1em; }
  .divider {
    width: min(300px, 90vw);
    border: none; border-top: 1px solid #2a2a2a;
  }
  #top-section {
    display: flex; flex-direction: column;
    align-items: center; gap: .65rem;
    width: min(300px, 90vw);
  }
  #hint { font-size: .8rem; color: #666; text-align: center; max-width: 280px; line-height: 1.45; }
  .btn {
    background: #222; color: #eee;
    border: 1px solid #333; border-radius: 10px;
    font-size: 1rem; font-weight: 700;
    height: 52px; width: 100%;
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
    width: min(300px, 90vw); min-height: 48px;
    text-align: center;
    display: flex; flex-direction: column; gap: .3rem;
  }
  #status-text   { font-size: .88rem; color: #aaa; }
  #progress-text { font-size: .8rem;  color: #888; }
  #countdown-wrap {
    display: none;
    width: 100%; background: #2a2a2a; border-radius: 6px;
    height: 7px; overflow: hidden;
  }
  #countdown-bar { height: 100%; background: #1a3799; width: 100%; }
  #back-link {
    color: #aaa; font-size: .8rem; text-decoration: none;
    border: 1px solid #444; border-radius: 6px;
    padding: .3rem .9rem;
  }
  #back-link:hover { background: #222; color: #fff; }
</style>
</head>
<body>
<h1>&#128274; PIN Assistant Deluxe</h1>

<label for='pin-input' style='font-size:.8rem; color:#666; letter-spacing:.05em;'>PIN eingeben</label>
<input id='pin-input' type='text' inputmode='numeric' pattern='[0-9]*'
       maxlength='4' placeholder='z.&#8239;B. 1234' autocomplete='off'>
<div id='pin-error'></div>

<hr class='divider'>

<div id='top-section'>
  <p id='hint'>Navigiere mit einem Klick auf Weiter durchs Men&#252;, bis du zur PIN-Eingabe aufgefordert wirst.</p>
  <button class='btn btn-weiter' onclick='weiter()'>Weiter &#8594;</button>
</div>

<hr class='divider'>

<div id='top-section'>
  <p id='hint'>Wenn du bei der PIN-Eingabe bist, &#252;bertrage den PIN.</p>
  <button class='btn btn-transfer' onclick='transferPin()'>PIN &#252;bertragen &#8594;</button>
  <div id='status-area'>
    <div id='status-text'></div>
    <div id='progress-text'></div>
    <div id='countdown-wrap'><div id='countdown-bar'></div></div>
  </div>
</div>
<hr class='divider' style='margin-top:0;'>
<div id='top-section'>
  <p id='hint'>Bl&#228;tter mit &#8222;Weiter&#8220; durchs Men&#252;. Stelle sicher, dass &#8222;PIN&#8220; auf <span style='color:#e05252;font-weight:700;'>Off</span> steht und &#8222;INF&#8220; auf <span style='color:#4caf50;font-weight:700;'>ON</span>. Wenn nicht, &#228;ndere den Zustand mit dem Button.</p>
  <button class='btn btn-confirm' onclick='bestaetigen()'>Zustand wechseln</button>
</div>
<a id='back-link' href='/'>&#8592; Go Back</a>

<script>
  var PULSE_GAP = 400;
  var SETTLE    = 3000;

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
  async function pulse() { await fetch('/flash'); }

  async function countdown(label) {
    sTxt.textContent = label || '⏳ Nächste Stelle wird aktiv';
    cdWrap.style.display = 'block';
    cdBar.style.transition = 'none';
    cdBar.style.width = '100%';
    cdBar.getBoundingClientRect();
    cdBar.style.transition = 'width ' + (SETTLE / 1000) + 's linear';
    cdBar.style.width = '0%';
    var rem = Math.round(SETTLE / 1000);
    pTxt.textContent = 'Warte ' + rem + 's …';
    var iv = setInterval(function() {
      rem--;
      pTxt.textContent = rem > 0 ? 'Warte ' + rem + 's …' : '';
    }, 1000);
    await wait(SETTLE);
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
      await fetch('/flashlong');
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
            if (p < d) await wait(PULSE_GAP);
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
