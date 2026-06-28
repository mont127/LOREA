#include "dashboard.hpp"

// Self-contained LOREA web dashboard.
// One HTML document: inline <style> + <script>, no external assets.
// Talks to the server defined in src/dashboard.cpp over the FIXED CONTRACT
// endpoints (/api/info, /api/history, /api/chat) plus the shared-terminal
// endpoints (/api/term/stream SSE, /api/term/input, /api/term/resize).
// The right panel is a real xterm.js terminal bound to the persistent PTY
// shell that both the user and the AI type into.
//
// NOTE: the raw-string delimiter is DASH; the body must never contain the
// sequence )<DASH>.

namespace ocli {

const char* DASHBOARD_HTML = R"DASH(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LOREA &middot; dashboard</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.min.css">
<script src="https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.min.js"></script>
<style>
  :root{
    --bg:#070a08;
    --grid:rgba(103,185,106,.10);
    --green:#67b96a;
    --green-bright:#7fd083;
    --green-deep:#2c5b2f;
    --green-panel:#244a27;
    --green-panel-2:#1c3b1f;
    --panel:#0e120f;
    --panel-2:#11160f;
    --line:rgba(103,185,106,.18);
    --ink:#dfe9df;
    --ink-dim:#9bb29c;
    --ink-faint:#6f846f;
    --radius:22px;
    --radius-sm:14px;
  }
  *{box-sizing:border-box}
  html,body{height:100%}
  body{
    margin:0;
    color:var(--ink);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
    background-color:var(--bg);
    background-image:radial-gradient(var(--grid) 1px, transparent 1px);
    background-size:22px 22px;
    background-position:-11px -11px;
    -webkit-font-smoothing:antialiased;
  }
  .mono{font-family:"SF Mono",ui-monospace,Menlo,Consolas,"Roboto Mono",monospace}

  /* ---- top-level responsive grid ---- */
  .app{
    height:100vh;
    min-height:100vh;
    padding:18px;
    display:grid;
    gap:18px;
    grid-template-columns:minmax(0,2.1fr) minmax(0,1fr);
    grid-template-rows:auto minmax(0,1fr);
    grid-template-areas:
      "head head"
      "main term";
  }
  @media (max-width:980px){
    .app{
      height:auto;
      min-height:100vh;
      grid-template-columns:1fr;
      grid-template-rows:auto auto auto;
      grid-template-areas:
        "head"
        "main"
        "term";
    }
    .main,.term{min-height:62vh}
  }

  /* ---- header ---- */
  .head{
    grid-area:head;
    display:flex;
    align-items:center;
    gap:16px;
    flex-wrap:wrap;
  }
  .pill{
    display:inline-flex;
    align-items:center;
    gap:9px;
    background:var(--green);
    color:#06210a;
    font-weight:800;
    letter-spacing:.14em;
    font-size:15px;
    padding:11px 20px;
    border-radius:999px;
    box-shadow:0 6px 22px rgba(103,185,106,.30), inset 0 0 0 1px rgba(255,255,255,.18);
  }
  .pill .dot{
    width:9px;height:9px;border-radius:50%;
    background:#06210a;
    box-shadow:0 0 0 3px rgba(6,33,10,.25);
  }
  .subtitle{color:var(--ink-dim);font-size:13px;letter-spacing:.02em}
  .meta{
    margin-left:auto;
    display:flex;
    align-items:center;
    gap:8px;
    flex-wrap:wrap;
    justify-content:flex-end;
  }
  .chip{
    display:inline-flex;align-items:center;gap:7px;
    font-size:12px;
    color:var(--ink-dim);
    background:rgba(103,185,106,.06);
    border:1px solid var(--line);
    padding:7px 12px;
    border-radius:999px;
    white-space:nowrap;
  }
  .chip b{color:var(--ink);font-weight:600}
  .chip .k{color:var(--ink-faint);text-transform:uppercase;letter-spacing:.08em;font-size:10px}
  .chip.on b{color:var(--green-bright)}
  .chip a{color:var(--green-bright);text-decoration:none}
  .chip a:hover{text-decoration:underline}

  /* ---- main conversation panel ---- */
  .main{
    grid-area:main;
    min-height:0;
    display:flex;
    flex-direction:column;
    background:linear-gradient(180deg,var(--panel),var(--panel-2));
    border:1px solid var(--line);
    border-radius:var(--radius);
    box-shadow:0 24px 60px rgba(0,0,0,.45);
    overflow:hidden;
  }
  .main-head{
    display:flex;align-items:center;gap:10px;
    padding:16px 20px;
    border-bottom:1px solid var(--line);
  }
  .main-head .glyph{color:var(--green);font-size:15px}
  .main-head .t{font-size:14px;font-weight:600;letter-spacing:.01em}
  .main-head .s{margin-left:auto;color:var(--ink-faint);font-size:12px}

  .stream{
    flex:1 1 auto;
    min-height:0;
    overflow-y:auto;
    padding:18px 20px 8px;
    display:flex;
    flex-direction:column;
    gap:14px;
    scroll-behavior:smooth;
  }
  .msg{display:flex;flex-direction:column;gap:6px;max-width:92%}
  .msg .who{
    font-size:10px;letter-spacing:.14em;text-transform:uppercase;
    color:var(--ink-faint);
  }
  .bubble{
    padding:12px 15px;
    border-radius:16px;
    line-height:1.55;
    font-size:14px;
    white-space:pre-wrap;
    word-break:break-word;
  }
  .msg.user{align-self:flex-end;align-items:flex-end}
  .msg.user .bubble{
    background:rgba(103,185,106,.14);
    border:1px solid rgba(103,185,106,.30);
    color:var(--ink);
  }
  .msg.assistant{align-self:flex-start}
  .msg.assistant .who{color:var(--green)}
  .msg.assistant .bubble{
    background:rgba(11,18,12,.85);
    border:1px solid var(--line);
    color:var(--green-bright);
    font-family:"SF Mono",ui-monospace,Menlo,Consolas,"Roboto Mono",monospace;
    font-size:13px;
  }
  .empty{color:var(--ink-faint);font-size:13px;margin:auto;text-align:center;padding:30px}

  .typing{
    align-self:flex-start;
    display:none;
    align-items:center;gap:8px;
    color:var(--green);font-size:12px;
    padding:4px 2px;
  }
  .typing.show{display:flex}
  .typing .ds{display:inline-flex;gap:4px}
  .typing .ds i{
    width:7px;height:7px;border-radius:50%;background:var(--green);
    animation:blink 1.2s infinite ease-in-out;
  }
  .typing .ds i:nth-child(2){animation-delay:.2s}
  .typing .ds i:nth-child(3){animation-delay:.4s}
  @keyframes blink{0%,80%,100%{opacity:.25;transform:translateY(0)}40%{opacity:1;transform:translateY(-3px)}}

  /* ---- green input bar pinned at bottom of main panel ---- */
  .composer{
    margin:12px;
    padding:9px 9px 9px 16px;
    display:flex;
    align-items:flex-end;
    gap:10px;
    background:linear-gradient(180deg,var(--green),#56a85a);
    border-radius:18px;
    box-shadow:0 10px 30px rgba(103,185,106,.25), inset 0 0 0 1px rgba(255,255,255,.16);
  }
  .composer textarea{
    flex:1 1 auto;
    resize:none;
    border:none;
    outline:none;
    background:transparent;
    color:#08240b;
    font:inherit;
    font-size:14px;
    line-height:1.45;
    max-height:140px;
    min-height:24px;
    padding:8px 0;
  }
  .composer textarea::placeholder{color:rgba(8,36,11,.6)}
  .send{
    flex:0 0 auto;
    border:none;
    cursor:pointer;
    background:#08240b;
    color:var(--green-bright);
    font-weight:700;
    font-size:13px;
    letter-spacing:.04em;
    padding:11px 18px;
    border-radius:13px;
    transition:transform .08s ease, opacity .15s ease;
  }
  .send:hover{transform:translateY(-1px)}
  .send:active{transform:translateY(0)}
  .send:disabled{opacity:.55;cursor:default;transform:none}

  /* ---- green right-side interactive terminal panel ---- */
  .term{
    grid-area:term;
    min-height:0;
    display:flex;
    flex-direction:column;
    background:linear-gradient(180deg,var(--green-panel),var(--green-panel-2));
    border:1px solid rgba(103,185,106,.45);
    border-radius:var(--radius);
    box-shadow:0 24px 60px rgba(0,0,0,.45), inset 0 0 0 1px rgba(255,255,255,.04);
    overflow:hidden;
  }
  .term-head{
    display:flex;align-items:center;gap:9px;
    padding:14px 16px;
    color:#eafaea;
    border-bottom:1px solid rgba(0,0,0,.25);
    background:rgba(0,0,0,.12);
  }
  .term-head .lights{display:inline-flex;gap:6px}
  .term-head .lights i{width:10px;height:10px;border-radius:50%;background:rgba(255,255,255,.35)}
  .term-head .lights i:first-child{background:#ffd36a}
  .term-head .lights i:last-child{background:#8ef0a0}
  .term-head .t{font-size:13px;font-weight:600;letter-spacing:.02em}
  .term-head .s{margin-left:auto;font-size:11px;color:rgba(234,250,234,.7)}

  /* ---- shared xterm.js terminal mount ---- */
  .term-xterm{
    flex:1 1 auto;
    min-height:0;
    margin:12px;
    padding:10px 12px;
    background:#050805;
    border:1px solid rgba(103,185,106,.30);
    border-radius:var(--radius-sm);
    overflow:hidden;
  }
  .term-xterm .xterm{height:100%;padding:0}
  .term-xterm .xterm-viewport{background-color:transparent !important}
  .term-xterm .xterm-viewport::-webkit-scrollbar{width:10px}
  .term-xterm .xterm-viewport::-webkit-scrollbar-thumb{
    background:rgba(103,185,106,.25);border-radius:999px;border:3px solid transparent;background-clip:padding-box;
  }

  /* scrollbars */
  .stream::-webkit-scrollbar{width:10px}
  .stream::-webkit-scrollbar-thumb{
    background:rgba(103,185,106,.25);border-radius:999px;border:3px solid transparent;background-clip:padding-box;
  }
</style>
</head>
<body>
  <div class="app">

    <header class="head">
      <span class="pill"><span class="dot"></span>LOREA</span>
      <span class="subtitle">AI agent &middot; LAN control dashboard</span>
      <div class="meta" id="meta">
        <span class="chip"><span class="k">model</span><b id="m-model">&hellip;</b></span>
        <span class="chip"><span class="k">backend</span><b id="m-backend">&hellip;</b></span>
        <span class="chip" id="m-auto-chip"><span class="k">auto</span><b id="m-auto">&hellip;</b></span>
        <span class="chip" id="m-tools-chip"><span class="k">tools</span><b id="m-tools">&hellip;</b></span>
        <span class="chip"><span class="k">lan</span><a id="m-lan" href="#">&hellip;</a></span>
      </div>
    </header>

    <section class="main">
      <div class="main-head">
        <span class="glyph">&#9678;</span>
        <span class="t">Tool calls and AI answering</span>
        <span class="s" id="main-status">connecting&hellip;</span>
      </div>
      <div class="stream" id="stream">
        <div class="empty" id="empty">No conversation yet. Send the first prompt below.</div>
        <div class="typing" id="typing">
          <span class="ds"><i></i><i></i><i></i></span>
          <span>LOREA is working&hellip;</span>
        </div>
      </div>
      <form class="composer" id="composer" autocomplete="off">
        <textarea id="prompt" rows="1" placeholder="Follow up prompts / first prompts&hellip;"></textarea>
        <button class="send" id="send" type="submit">Send</button>
      </form>
    </section>

    <aside class="term">
      <div class="term-head">
        <span class="lights"><i></i><i></i><i></i></span>
        <span class="t">Shared terminal &middot; you and the AI type here</span>
        <span class="s">/api/term</span>
      </div>
      <div class="term-xterm" id="termXterm"></div>
    </aside>

  </div>

<script>
(function(){
  "use strict";

  var stream   = document.getElementById("stream");
  var emptyEl  = document.getElementById("empty");
  var typing   = document.getElementById("typing");
  var composer = document.getElementById("composer");
  var prompt   = document.getElementById("prompt");
  var sendBtn  = document.getElementById("send");
  var mainStat = document.getElementById("main-status");

  var termHost = document.getElementById("termXterm");

  var busy = false;          // chat turn in flight
  var lastHistory = "";      // last rendered history signature

  function nearBottom(el){
    return (el.scrollHeight - el.scrollTop - el.clientHeight) < 80;
  }
  function setText(id, v){
    var e = document.getElementById(id);
    if (e) e.textContent = (v === undefined || v === null || v === "") ? "n/a" : String(v);
  }

  /* ---------- header info ---------- */
  function loadInfo(){
    fetch("/api/info").then(function(r){ return r.json(); }).then(function(d){
      setText("m-model",   d.model);
      setText("m-backend", d.backend);
      var autoOn = !!d.auto;
      setText("m-auto", autoOn ? "on" : "off");
      document.getElementById("m-auto-chip").classList.toggle("on", autoOn);
      var tools = (d.tool_access === undefined || d.tool_access === null) ? "n/a" : String(d.tool_access);
      setText("m-tools", tools);
      var toolsOn = /full|on|all|enabled|true|yes/i.test(tools);
      document.getElementById("m-tools-chip").classList.toggle("on", toolsOn);
      var lan = document.getElementById("m-lan");
      if (d.lan_url){ lan.textContent = d.lan_url; lan.href = d.lan_url; }
      else { lan.textContent = "n/a"; lan.removeAttribute("href"); }
    }).catch(function(){ /* keep last values */ });
  }

  /* ---------- conversation ---------- */
  function renderHistory(messages){
    var sig = JSON.stringify(messages);
    if (sig === lastHistory) return;   // nothing changed, avoid flicker
    lastHistory = sig;

    var stick = nearBottom(stream);

    // wipe everything except the persistent typing indicator
    var node = stream.firstChild;
    while (node){
      var next = node.nextSibling;
      if (node !== typing) stream.removeChild(node);
      node = next;
    }

    if (!messages || !messages.length){
      stream.insertBefore(emptyEl, typing);
    } else {
      for (var i = 0; i < messages.length; i++){
        var m = messages[i] || {};
        var role = (m.role === "user") ? "user" : "assistant";
        var wrap = document.createElement("div");
        wrap.className = "msg " + role;

        var who = document.createElement("div");
        who.className = "who";
        who.textContent = (role === "user") ? "You" : "LOREA";

        var bub = document.createElement("div");
        bub.className = "bubble";
        bub.textContent = (m.content === undefined || m.content === null) ? "" : String(m.content);

        wrap.appendChild(who);
        wrap.appendChild(bub);
        stream.insertBefore(wrap, typing);
      }
    }
    if (stick) stream.scrollTop = stream.scrollHeight;
  }

  function loadHistory(){
    return fetch("/api/history").then(function(r){ return r.json(); }).then(function(d){
      mainStat.textContent = "live";
      renderHistory((d && d.messages) ? d.messages : []);
    }).catch(function(){
      mainStat.textContent = "offline";
    });
  }

  function showTyping(on){
    typing.classList.toggle("show", on);
    if (on){
      stream.appendChild(typing);     // keep it last
      if (nearBottom(stream)) stream.scrollTop = stream.scrollHeight;
    }
  }

  function sendChat(){
    var text = prompt.value.trim();
    if (!text || busy) return;
    busy = true;
    sendBtn.disabled = true;
    prompt.value = "";
    autoGrow();
    lastHistory = "";          // force a re-render so the user msg shows now
    showTyping(true);
    mainStat.textContent = "working";

    fetch("/api/chat", {
      method:"POST",
      headers:{ "Content-Type":"application/json" },
      body: JSON.stringify({ message: text })
    }).then(function(r){ return r.json(); })
      .then(function(){ return loadHistory(); })
      .catch(function(){ mainStat.textContent = "error"; })
      .then(function(){
        busy = false;
        sendBtn.disabled = false;
        showTyping(false);
        prompt.focus();
      });
  }

  /* ---------- shared interactive terminal (xterm.js) ---------- */
  /* One persistent PTY-backed shell that BOTH this panel and the AI type into.
     SSE pushes the live PTY output (snapshot first, then deltas); keystrokes
     and the AI's commands all land in the same TTY. */
  var term = null;
  var fitAddon = null;

  function pushResize(){
    if (!term || !term.rows || !term.cols) return;
    fetch("/api/term/resize", {
      method:"POST",
      headers:{ "Content-Type":"application/json" },
      body: JSON.stringify({ rows: term.rows, cols: term.cols })
    }).catch(function(){ /* server may be busy; harmless */ });
  }

  function fitTerm(){
    if (!fitAddon) return;
    try { fitAddon.fit(); } catch(e){ /* container not laid out yet */ }
    pushResize();
  }

  function setupTerminal(){
    if (typeof Terminal === "undefined"){
      // xterm CDN not loaded yet (slow link); retry without blocking the page.
      setTimeout(setupTerminal, 800);
      return;
    }
    term = new Terminal({
      cursorBlink:true,
      convertEol:false,
      scrollback:5000,
      fontFamily:'"SF Mono",ui-monospace,Menlo,Consolas,"Roboto Mono",monospace',
      fontSize:12.5,
      lineHeight:1.2,
      theme:{
        background:"#050805",
        foreground:"#7fd083",
        cursor:"#7fd083",
        cursorAccent:"#050805",
        selectionBackground:"rgba(103,185,106,.35)",
        black:"#0b120c",
        green:"#67b96a",
        brightGreen:"#7fd083"
      }
    });
    if (typeof FitAddon !== "undefined" && FitAddon.FitAddon){
      fitAddon = new FitAddon.FitAddon();
      term.loadAddon(fitAddon);
    }
    term.open(termHost);
    fitTerm();
    setTimeout(fitTerm, 60);

    // local keystrokes -> shared PTY master (raw bytes).
    term.onData(function(d){
      fetch("/api/term/input", {
        method:"POST",
        headers:{ "Content-Type":"application/json" },
        body: JSON.stringify({ data: d })
      }).catch(function(){ /* dropped keystroke; user can retype */ });
    });

    // shared PTY output (server sends JSON-encoded chunks) -> terminal.
    var es = new EventSource("/api/term/stream");
    es.onmessage = function(e){
      try { term.write(JSON.parse(e.data)); }
      catch(err){ term.write(e.data); }
    };
    es.onerror = function(){ /* EventSource reconnects on its own */ };

    if (window.ResizeObserver){
      try { new ResizeObserver(function(){ fitTerm(); }).observe(termHost); }
      catch(e){ /* older engines fall back to window resize */ }
    }
    window.addEventListener("resize", fitTerm);
  }

  /* ---------- textarea auto-grow + key handling ---------- */
  function autoGrow(){
    prompt.style.height = "auto";
    prompt.style.height = Math.min(prompt.scrollHeight, 140) + "px";
  }
  prompt.addEventListener("input", autoGrow);
  prompt.addEventListener("keydown", function(e){
    if (e.key === "Enter" && !e.shiftKey){
      e.preventDefault();
      sendChat();
    }
  });

  composer.addEventListener("submit", function(e){ e.preventDefault(); sendChat(); });

  /* ---------- boot ---------- */
  loadInfo();
  loadHistory();
  setupTerminal();
  setInterval(function(){ if (!busy) loadHistory(); }, 2000);
  setInterval(loadInfo, 15000);
  prompt.focus();
})();
</script>
</body>
</html>
)DASH";

}  // namespace ocli
