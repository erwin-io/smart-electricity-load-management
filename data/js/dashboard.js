/* global apiGet, apiPost, $ */

// ---------- Dashboard State ----------
const state = {
  p: { 1:false, 2:false, 3:false, 4:false },
  showGraph: true,
  showPrioStatus: true,
  showPrioControls: false,
  budgetKwh: 0,
  graph: {
    used: [],
    rem:  [],
    t:    [],
    pxPerSample: 4,   // ~ one sample every 4px
    capMin: 60       // never less than 60 samples
  }
};

// ---------- Small UI helpers ----------
function setBtnToggle(el, on) {
  el.classList.toggle("is-on", !!on);
  el.setAttribute("aria-pressed", !!on);
}
function setStatusPill(el, on) {
  el.classList.toggle("is-on", !!on);
}

// ---------- Y-axis rules ----------
function yAxisTop(budget) {
  if (budget >= 2) return budget;
  if (budget >= 1) return 1;
  return Math.max(0.001, budget || 0.001);
}
function yAxisTicks(budget) {
  if (budget >= 2) {
    const step = budget / 4;
    return [budget, budget - step, budget - 2*step, budget - 3*step, 0];
  }
  if (budget >= 1) return [1, 0.75, 0.5, 0.25, 0];
  const top = yAxisTop(budget);
  return [top, top*0.75, top*0.5, top*0.25, 0];
}
function decimalsFor(top) {
  if (top >= 10) return 1;
  if (top >= 2)  return 2;
  if (top >= 1)  return 2;
  if (top >= 0.1)  return 3;
  if (top >= 0.01) return 4;
  return 5;
}
function formatKwh(v, top) {
  return v.toFixed(decimalsFor(top)) + " KWH";
}

// ---------- Capacity management (prevents compression) ----------
function computeCapacity() {
  const c = $("usageGraph");
  const Wcss = c.clientWidth || c.width || 600;
  const padL = 72, padR = 12;
  const plotW = Math.max(60, Wcss - padL - padR);
  const cap  = Math.floor(plotW / state.graph.pxPerSample);
  return Math.max(state.graph.capMin, cap);
}
function trimToCapacity() {
  const cap = computeCapacity();
  const n = state.graph.t.length;
  if (n > cap) {
    const start = n - cap;
    state.graph.t    = state.graph.t.slice(start);
    state.graph.used = state.graph.used.slice(start);
    state.graph.rem  = state.graph.rem.slice(start);
  }
}

// ---------- Poll + render ----------
async function fetchStatusAndRender() {
  try {
    const s = await apiGet("/api/status");

    // KPIs
    $("kpiPct").textContent  = (s.remainingPct ?? 0).toFixed(1) + "%";
    $("kpiUsed").textContent = (s.usedKWh ?? 0).toFixed(3);
    $("kpiRem").textContent  = (s.remKWh ?? 0).toFixed(3);

    // Gauge update
    const pct = Math.max(0, Math.min(100, Number(s.remainingPct) || 0));
    const g   = $("gaugeRemain");
    const gl  = $("gaugeLabel");
    g.style.setProperty("--p", pct);
    gl.textContent = Math.round(pct) + "%";

    // Budget for chart scale
    state.budgetKwh = Number(s.budget) || 0;

    // Graph sample push (always push; we trim by pixel-capacity)
    state.graph.t.push(Date.now());
    state.graph.used.push(s.usedKWh || 0);
    state.graph.rem.push(s.remKWh || 0);
    trimToCapacity();

    // Visibility toggles from config
    state.showGraph       = !!s.show_usage_graph;
    state.showPrioStatus  = !!s.show_prio_status;
    state.showPrioControls= !!s.show_prio_controls;
    $("secGraph").style.display      = state.showGraph ? "" : "none";
    $("secPrioStatus").style.display = state.showPrioStatus ? "" : "none";
    $("secPrio").style.display       = state.showPrioControls ? "" : "none";

    // Status pills
    if (state.showPrioStatus) {
      setStatusPill($("ps1"), !!s.p1);
      setStatusPill($("ps2"), !!s.p2);
      setStatusPill($("ps3"), !!s.p3);
      setStatusPill($("ps4"), !!s.p4);
    }

    // Control buttons reflect live status and stay blue while ON
    if (state.showPrioControls) {
      const list = [s.p1, s.p2, s.p3, s.p4];
      ["p1","p2","p3","p4"].forEach((id,i)=>{
        const btn = $(id);
        const on  = !!list[i];
        state.p[i+1] = on;
        setBtnToggle(btn, on);
      });
    }

    // Run Controls enable/disable:
    // - When depleted => disable Resume & Stop, enable Restart
    // - Otherwise     => Resume disabled if not paused; Stop disabled if paused
    const depleted = (Number(s.remKWh) || 0) <= 0 || (Number(s.remainingPct) || 0) <= 0;
    const btnResume  = $("btnResume");
    const btnStop    = $("btnStop");
    const btnRestart = $("btnRestart");
    if (depleted) {
      btnResume.disabled  = true;
      btnStop.disabled    = true;
      btnRestart.disabled = false;
    } else {
      btnResume.disabled  = !s.paused; // only enabled when paused
      btnStop.disabled    =  s.paused; // only enabled when running
      btnRestart.disabled = false;
    }

    if (state.showGraph) drawGraph();
  } catch {
    // ignore transient errors
  }
}

// ---------- Canvas chart (single series) ----------
function drawGraph() {
  const c = $("usageGraph");
  const g = c.getContext("2d");

  // HiDPI crispness
  const dpr  = window.devicePixelRatio || 1;
  const Wcss = c.clientWidth  || c.width  || 600;
  const Hcss = c.clientHeight || c.height || 260;
  if (c.width !== Math.floor(Wcss * dpr) || c.height !== Math.floor(Hcss * dpr)) {
    c.width  = Math.floor(Wcss * dpr);
    c.height = Math.floor(Hcss * dpr);
  }
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const W = Wcss, H = Hcss;

  g.clearRect(0, 0, W, H);

  // Layout
  const padL = 72, padR = 12, padT = 10, padB = 26;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;

  // Data (used line)
  const ys = state.graph.used;
  const n  = ys.length;
  if (n < 2) return;

  // Axis scale from budget rules
  const top   = yAxisTop(state.budgetKwh);
  const ticks = yAxisTicks(state.budgetKwh);
  const yMin  = 0, yMax = top;

  // Helpers
  const xAt = i => padL + (plotW * (i / (n - 1)));
  const yAt = v => {
    const vv = Math.max(yMin, Math.min(yMax, v));
    return padT + plotH - (plotH * (vv - yMin) / (yMax - yMin));
  };

  // Grid
  g.strokeStyle = "rgba(255,255,255,0.08)";
  g.lineWidth = 1;
  g.beginPath();
  for (let i = 0; i < ticks.length; i++) {
    const y = yAt(ticks[i]);
    g.moveTo(padL, y); g.lineTo(W - padR, y);
  }
  g.stroke();

  // Axes
  g.strokeStyle = "rgba(255,255,255,0.25)";
  g.beginPath();
  g.moveTo(padL, padT);
  g.lineTo(padL, H - padB);
  g.lineTo(W - padR, H - padB);
  g.stroke();

  // Y labels
  g.fillStyle = "rgba(255,255,255,0.75)";
  g.font = "12px system-ui, -apple-system, Segoe UI, Roboto, sans-serif";
  for (let i = 0; i < ticks.length; i++) {
    const y = yAt(ticks[i]);
    g.fillText(formatKwh(ticks[i], top), 6, y - 2);
  }

  // Line
  g.beginPath();
  g.lineWidth = 2;
  g.strokeStyle = "rgba(105,169,255,0.95)";
  for (let i = 0; i < n; i++) {
    const x = xAt(i), y = yAt(ys[i]);
    if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
  }
  g.stroke();
}

// ---------- Priority control handler ----------
async function togglePrio(n) {
  if (!state.showPrioControls) return;
  const btn = $("p" + n);
  const to = state.p[n] ? 0 : 1;
  setBtnToggle(btn, !!to); // optimistic
  try {
    await fetch(`/api/relays/set?prio=${n}&on=${to}`, { method:"POST", credentials:"same-origin" });
    state.p[n] = !!to;
    $("prioMsg").textContent = "";
  } catch (e) {
    setBtnToggle(btn, state.p[n]); // revert
    $("prioMsg").textContent = e.message || "Failed to set relay.";
  }
}

// ---------- Run controls ----------
async function doRun(action) {
  const url = action === "stop"   ? "/api/stop"
            : action === "resume" ? "/api/resume"
            :                       "/api/restart";
  try {
    await apiPost(url, {});
    $("runMsg").textContent = "OK";
  } catch (e) {
    $("runMsg").textContent = e.message || "Action failed";
  }
}

// ---------- Wire UI ----------
function wire() {
  ["1","2","3","4"].forEach(n => $("p"+n).addEventListener("click", () => togglePrio(n)));

  $("btnResume").onclick = () => doRun("resume");
  $("btnStop").onclick   = () => doRun("stop");
  $("btnRestart").onclick= () => doRun("restart");

  $("btnLogout").onclick = async () => {
    try { await apiPost("/api/logout", {}); location.href="/login"; } catch {}
  };

  fetchStatusAndRender();
  setInterval(fetchStatusAndRender, 1000);

  // Also retrim when window resizes so capacity follows width.
  window.addEventListener("resize", trimToCapacity);

  // Initial state of buttons on load
  $("btnResume").disabled = true;
  $("btnStop").disabled   = true;
}

// ---------- Boot ----------
document.addEventListener("DOMContentLoaded", wire);
