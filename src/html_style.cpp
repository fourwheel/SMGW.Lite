#include "html_style.h"
#include <Arduino.h>

const char HTML_STYLE[] PROGMEM = R"rawliteral(
<style>
  html, body {
    background: #fdfdfd;
    margin: 0;
    padding: 15px;
    box-sizing: border-box;
  }

  body {
    font-family: sans-serif;
    /* 3-column layout for desktop */
    column-count: 3;
    column-gap: 25px;
    column-rule: 1px solid #eee;
  }

  /* Single column fallback for mobile */
  @media (max-width: 900px) {
    body { column-count: 1; }
  }

  /* Keep related blocks inside one column */
  h2, h3, table, ul, p, div {
    break-inside: avoid;
    display: block;
    max-width: 100%;
    overflow-x: auto;
  }

  table {
    border-collapse: collapse;
    width: 100%;
    background: white;
    margin-bottom: 1.2em;
    font-size: 0.85em;
  }

  th, td {
    border: 1px solid #ccc;
    padding: 5px 8px;
    text-align: left;
    word-break: break-word;
  }

  th { background: #eee; }
  ul { padding-left: 1.5em; margin-bottom: 1.2em; }
  li { margin-bottom: 0.3em; }
  a { color: #0066cc; text-decoration: none; }
  a:hover { text-decoration: underline; }
  .section {
    display: inline-block;
    width: 100%;
    break-inside: avoid;
    margin-bottom: 20px;
  }
</style>
)rawliteral";

const char HTML_STYLE_MODERN[] PROGMEM = R"rawliteral(
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#f0f2f7;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:1.5rem 1rem 3rem;gap:.8rem;color:#1a1a1a;}
.logo{font-size:1.4rem;font-weight:800;color:#1a3799;letter-spacing:-.02em;}
.back{color:#1a3799;font-size:.85rem;text-decoration:none;width:100%;max-width:600px;}
.back:hover{text-decoration:underline;}
.card{background:#fff;border-radius:14px;border:1px solid #d0d8f0;padding:1.1rem 1.3rem;width:100%;max-width:600px;}
.card-title{font-size:.95rem;font-weight:700;color:#1a3799;margin-bottom:.7rem;padding-bottom:.35rem;border-bottom:1px solid #e8edf5;}
.kv{display:flex;gap:.5rem;padding:.28rem 0;font-size:.84rem;border-bottom:1px solid #f0f2f7;}
.kv.last{border-bottom:none;}
.kl{color:#666;min-width:210px;flex-shrink:0;white-space:nowrap;}
a{color:#1a3799;text-decoration:none;}
a:hover{text-decoration:underline;}
table{border-collapse:collapse;width:100%;font-size:.81rem;margin-top:.2rem;}
th,td{border:1px solid #dde3f0;padding:4px 7px;text-align:left;word-break:break-word;}
th{background:#f0f2f7;color:#1a3799;font-weight:600;}
tr:nth-child(even) td{background:#fafbfd;}
.ok{color:#2e7d32;font-weight:600;}
.fail{color:#c62828;font-weight:600;}
.warn{color:#856404;font-weight:600;}
.btns{display:flex;gap:.5rem;flex-wrap:wrap;}
.btn{padding:.65rem .95rem;border-radius:8px;background:#1a3799;color:#fff;font-size:.84rem;font-weight:700;text-decoration:none;border:none;cursor:pointer;display:inline-block;min-height:44px;display:inline-flex;align-items:center;}
.btn:hover{background:#142b7a;text-decoration:none;}
.btn-s{background:#f0f2f7;color:#1a3799;border:1px solid #c0ccec;}
.btn-s:hover{background:#dde3f0;}
.btn-d{background:#c62828;color:#fff;border:none;}
.btn-d:hover{background:#a01c1c;}
.tbl{overflow-x:auto;-webkit-overflow-scrolling:touch;}
textarea{width:100%;font-family:monospace;font-size:.78rem;border:1px solid #d0d8f0;border-radius:8px;padding:.6rem;resize:vertical;}
.hint{font-size:.79rem;color:#888;margin-top:.3rem;}
code{font-family:monospace;font-size:.85em;background:#f0f2f7;padding:.1em .3em;border-radius:3px;}
small{font-size:.79rem;}
.kl.e::after{content:" \270F";font-size:.68rem;color:#1a3799;opacity:.6;vertical-align:middle;}
.cfg-link{display:flex;align-items:center;gap:.9rem;background:#fff;border-radius:14px;border:1px solid #d0d8f0;padding:.85rem 1.3rem;width:100%;max-width:600px;text-decoration:none;color:#1a1a1a;transition:border-color .15s;}
.cfg-link:hover{background:#f5f7ff;text-decoration:none;border-color:#1a3799;}
.cfg-icon{font-size:1.5rem;color:#1a3799;flex-shrink:0;line-height:1;}
.cfg-text strong{display:block;font-size:.9rem;font-weight:700;color:#1a3799;}
.cfg-text small{font-size:.77rem;color:#777;}
@media(max-width:440px){.kv{flex-wrap:wrap;}.kl{min-width:unset;width:100%;color:#888;font-size:.78rem;padding-bottom:0;white-space:normal;}}
</style>
)rawliteral";

const char HTML_STYLE_SERIAL_SCAN[] PROGMEM = R"rawliteral(
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f2f7;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:1.5rem 1rem 3rem;gap:.8rem;color:#1a1a1a}
.logo{font-size:1.4rem;font-weight:800;color:#1a3799;letter-spacing:-.02em}
.back{color:#1a3799;font-size:.85rem;text-decoration:none;width:100%;max-width:500px}
.card{background:#fff;border-radius:14px;border:1px solid #d0d8f0;padding:1.1rem 1.3rem;width:100%;max-width:500px}
.card-title{font-size:.78rem;font-weight:700;text-transform:uppercase;letter-spacing:.06em;color:#888;margin-bottom:.7rem}
table{width:100%;border-collapse:collapse;font-size:.85rem}
td{padding:.3rem .4rem;border-bottom:1px solid #f0f2f7;vertical-align:middle}
td:first-child{font-family:monospace;font-size:.88rem;width:40%}
td:nth-child(2){width:35%}
td:nth-child(3){width:25%;text-align:right}
.testing{color:#e8a800;font-weight:600}
.found{color:#1a9b3a;font-weight:700}
.fail{color:#bbb}
.pending{color:#ddd}
.btn{padding:.65rem .95rem;border-radius:8px;background:#1a3799;color:#fff;font-size:.84rem;font-weight:700;text-decoration:none;display:inline-flex;align-items:center;min-height:44px;margin-top:.8rem}
.set-btn{padding:.22rem .55rem;border-radius:6px;background:#1a3799;color:#fff;font-size:.75rem;font-weight:600;border:none;cursor:pointer;min-height:28px}
.set-btn.act{background:#2e7d32;}
.set-btn:disabled{background:#ccc;cursor:not-allowed}
tr.active-cfg td:first-child{color:#1a3799;font-weight:700}
tr.active-cfg td:first-child::before{content:"▶ "}
#result{margin-top:.75rem;font-size:.9rem;font-weight:600;display:none}
.ok{color:#1a9b3a}.err{color:#c0392b}
</style>
)rawliteral";
