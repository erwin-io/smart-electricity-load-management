/* global apiGet, apiPost, $ */

// ---------- Dashboard State ----------
const state = {
  p: { 1: false, 2: false, 3: false, 4: false },
  showGraph: true,
  showPrioStatus: true,
  showPrioControls: false,
  budgetKwh: 0,
  graph: { used: [], rem: [], t: [], pxPerSample: 4, capMin: 60 },
};

// ---------- Small UI helpers ----------
function setBtnToggle(el, on) {
  el.classList.toggle("is-on", !!on);
  el.setAttribute("aria-pressed", !!on);
}
function setStatusPill(el, on) {
  el.classList.toggle("is-on", !!on);
}
const cssVar = (name) =>
  getComputedStyle(document.documentElement).getPropertyValue(name).trim();

// ---------- Y-axis rules ----------
function yAxisTop(budget) {
  if (budget >= 2) return budget;
  if (budget >= 1) return 1;
  return Math.max(0.001, budget || 0.001);
}
function yAxisTicks(budget) {
  if (budget >= 2) {
    const s = budget / 4;
    return [budget, budget - s, budget - 2 * s, budget - 3 * s, 0];
  }
  if (budget >= 1) return [1, 0.75, 0.5, 0.25, 0];
  const top = yAxisTop(budget);
  return [top, top * 0.75, top * 0.5, top * 0.25, 0];
}
function decimalsFor(top) {
  if (top >= 10) return 1;
  if (top >= 2) return 2;
  if (top >= 1) return 2;
  if (top >= 0.1) return 3;
  if (top >= 0.01) return 4;
  return 5;
}
function formatKwh(v, top) {
  return v.toFixed(decimalsFor(top)) + " KWH";
}

// ---------- Capacity ----------
function computeCapacity() {
  const c = $("usageGraph");
  const Wcss = c.clientWidth || c.width || 600;
  const plotW = Math.max(60, Wcss - 72 - 12);
  return Math.max(
    state.graph.capMin,
    Math.floor(plotW / state.graph.pxPerSample)
  );
}
function trimToCapacity() {
  const cap = computeCapacity(),
    n = state.graph.t.length;
  if (n > cap) {
    const s = n - cap;
    state.graph.t = state.graph.t.slice(s);
    state.graph.used = state.graph.used.slice(s);
    state.graph.rem = state.graph.rem.slice(s);
  }
}

// ---------- Poll + render ----------
async function fetchStatusAndRender() {
  try {
    const s = await apiGet("/api/status");
    $("kpiUsed").textContent = (s.usedKWh ?? 0).toFixed(3);
    $("kpiRem").textContent = (s.remKWh ?? 0).toFixed(3);

    const pct = Math.max(0, Math.min(100, Number(s.remainingPct) || 0));
    $("gaugeRemain").style.setProperty("--p", pct);
    $("gaugeLabel").textContent = Math.round(pct) + "%";

    // In fetchStatusAndRender(), after computing pct and before drawing:
    const gEl = $("gaugeRemain");
    gEl.style.setProperty("--p", pct);
    gEl.style.setProperty("--gauge-color", gaugeColorFor(pct));
    $("gaugeLabel").textContent = Math.round(pct) + "%";

    state.budgetKwh = Number(s.budget) || 0;

    state.graph.t.push(Date.now());
    state.graph.used.push(s.usedKWh || 0);
    state.graph.rem.push(s.remKWh || 0);
    trimToCapacity();

    state.showGraph = !!s.show_usage_graph;
    state.showPrioStatus = !!s.show_prio_status;
    state.showPrioControls = !!s.show_prio_controls;
    $("secGraph").style.display = state.showGraph ? "" : "none";
    $("secPrioStatus").style.display = state.showPrioStatus ? "" : "none";
    $("secPrio").style.display = state.showPrioControls ? "" : "none";

    if (state.showPrioStatus) {
      setStatusPill($("ps1"), !!s.p1);
      setStatusPill($("ps2"), !!s.p2);
      setStatusPill($("ps3"), !!s.p3);
      setStatusPill($("ps4"), !!s.p4);
    }
    if (state.showPrioControls) {
      const list = [s.p1, s.p2, s.p3, s.p4];
      ["p1", "p2", "p3", "p4"].forEach((id, i) => {
        const on = !!list[i];
        state.p[i + 1] = on;
        setBtnToggle($(id), on);
      });
    }

    const depleted =
      (Number(s.remKWh) || 0) <= 0 || (Number(s.remainingPct) || 0) <= 0;
    $("btnResume").disabled = depleted || !s.paused;
    $("btnStop").disabled = depleted || s.paused;
    $("btnRestart").disabled = false;

    if (state.showGraph) drawGraph();
  } catch {}
}

// ---------- Canvas chart ----------
function drawGraph() {
  const c = $("usageGraph"),
    g = c.getContext("2d");
  const dpr = window.devicePixelRatio || 1,
    Wcss = c.clientWidth || c.width || 600,
    Hcss = c.clientHeight || c.height || 260;
  if (
    c.width !== Math.floor(Wcss * dpr) ||
    c.height !== Math.floor(Hcss * dpr)
  ) {
    c.width = Math.floor(Wcss * dpr);
    c.height = Math.floor(Hcss * dpr);
  }
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const W = Wcss,
    H = Hcss;

  g.clearRect(0, 0, W, H);

  const padL = 72,
    padR = 12,
    padT = 10,
    padB = 26,
    plotW = W - padL - padR,
    plotH = H - padT - padB;
  const ys = state.graph.used,
    n = ys.length;
  if (n < 2) return;

  const top = yAxisTop(state.budgetKwh),
    ticks = yAxisTicks(state.budgetKwh),
    yMin = 0,
    yMax = top;
  const xAt = (i) => padL + plotW * (i / (n - 1));
  const yAt = (v) => {
    const vv = Math.max(yMin, Math.min(yMax, v));
    return padT + plotH - (plotH * (vv - yMin)) / (yMax - yMin);
  };

  // Grid & axes from CSS vars
  g.strokeStyle = cssVar("--graph-grid");
  g.lineWidth = 1;
  g.beginPath();
  for (let i = 0; i < ticks.length; i++) {
    const y = yAt(ticks[i]);
    g.moveTo(padL, y);
    g.lineTo(W - padR, y);
  }
  g.stroke();

  g.strokeStyle = cssVar("--graph-axis");
  g.beginPath();
  g.moveTo(padL, padT);
  g.lineTo(padL, H - padB);
  g.lineTo(W - padR, H - padB);
  g.stroke();

  g.fillStyle = cssVar("--text-2") || "rgba(0,0,0,.6)";
  g.font = "12px system-ui,-apple-system,Segoe UI,Roboto,sans-serif";
  for (let i = 0; i < ticks.length; i++) {
    const y = yAt(ticks[i]);
    g.fillText(formatKwh(ticks[i], top), 6, y - 2);
  }

  g.beginPath();
  g.lineWidth = 2;
  g.strokeStyle = cssVar("--graph-line") || "#2563eb";
  for (let i = 0; i < n; i++) {
    const x = xAt(i),
      y = yAt(ys[i]);
    if (i === 0) g.moveTo(x, y);
    else g.lineTo(x, y);
  }
  g.stroke();
}

// ---------- Priority control ----------
async function togglePrio(n) {
  if (!state.showPrioControls) return;
  const btn = $("p" + n),
    to = state.p[n] ? 0 : 1;
  setBtnToggle(btn, !!to);
  try {
    await fetch(`/api/relays/set?prio=${n}&on=${to}`, {
      method: "POST",
      credentials: "same-origin",
    });
    state.p[n] = !!to;
    $("prioMsg").textContent = "";
  } catch (e) {
    setBtnToggle(btn, state.p[n]);
    $("prioMsg").textContent = e.message || "Failed to set relay.";
  }
}

// ---------- Run controls ----------
async function doRun(a) {
  const url =
    a === "stop"
      ? "/api/stop"
      : a === "resume"
      ? "/api/resume"
      : "/api/restart";
  try {
    await apiPost(url, {});
    $("runMsg").textContent = "OK";
  } catch (e) {
    $("runMsg").textContent = e.message || "Action failed";
  }
}

// ---------- Wire ----------
function wire() {
  ["1", "2", "3", "4"].forEach((n) =>
    $("p" + n).addEventListener("click", () => togglePrio(n))
  );
  $("btnResume").onclick = () => doRun("resume");
  $("btnStop").onclick = () => doRun("stop");
  $("btnRestart").onclick = () => doRun("restart");
  $("btnLogout").onclick = async () => {
    try {
      await apiPost("/api/logout", {});
      location.href = "/login";
    } catch {}
  };

  fetchStatusAndRender();
  setInterval(fetchStatusAndRender, 1000);
  window.addEventListener("resize", trimToCapacity);
  $("btnResume").disabled = true;
  $("btnStop").disabled = true;
}
function gaugeColorFor(pct) {
  if (pct >= 75) return "var(--gauge-blue)";
  if (pct >= 50) return "var(--gauge-green)";
  if (pct >= 25) return "var(--gauge-yellow)";
  if (pct >= 10) return "var(--gauge-orange)";
  return "var(--gauge-red)";
}

document.addEventListener("DOMContentLoaded", wire);
