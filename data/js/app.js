function $(id){ return document.getElementById(id); }
async function apiGet(url){ const r=await fetch(url,{credentials:"include"}); if(!r.ok) throw new Error(await r.text()); return r.json(); }
async function apiPost(url, data){
  const opt = data ? {method:"POST",headers:{"Content-Type":"application/json"},credentials:"include",body:JSON.stringify(data)}
                   : {method:"POST",credentials:"include"};
  const r = await fetch(url, opt); if(!r.ok) throw new Error(await r.text()); return r.text();
}
async function logout(){ try{ await apiPost("/api/logout"); location.href="/login"; }catch(e){ alert(e); } }

/* ---------- Global theme: light default; persist in localStorage ---------- */
function getThemeMode(){ return localStorage.getItem("themeMode") || "light"; }
function applyThemeMode(mode){
  const m = (mode === "dark") ? "dark" : "light";
  document.documentElement.setAttribute("data-theme", m);
  localStorage.setItem("themeMode", m);
  const btn = $("themeToggle");
  if (btn){
    const isDark = m === "dark";
    btn.textContent = isDark ? "☀️ Light mode" : "🌙 Night mode";
    btn.setAttribute("aria-pressed", String(isDark));
  }
}
function toggleThemeMode(){ applyThemeMode(getThemeMode()==="dark" ? "light" : "dark"); }

document.addEventListener("DOMContentLoaded", () => {
  applyThemeMode(getThemeMode());
  const b = $("themeToggle"); if (b) b.addEventListener("click", toggleThemeMode);
});
