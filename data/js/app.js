function $(id){ return document.getElementById(id); }
async function apiGet(url){ const r=await fetch(url,{credentials:"include"}); if(!r.ok) throw new Error(await r.text()); return r.json(); }
async function apiPost(url, data){
  const opt = data ? {method:"POST",headers:{"Content-Type":"application/json"},credentials:"include",body:JSON.stringify(data)} :
                      {method:"POST",credentials:"include"};
  const r=await fetch(url,opt); if(!r.ok) throw new Error(await r.text()); return r.text();
}
async function logout(){ try{ await apiPost("/api/logout"); location.href="/login"; }catch(e){ alert(e); } }

function getTheme(){
  try{ return JSON.parse(localStorage.getItem("theme")||"{}"); }catch(_){ return {}; }
}
function applyTheme(th){
  const root = document.documentElement;
  if(th.bg)    root.style.setProperty("--bg", th.bg);
  if(th.card)  root.style.setProperty("--card", th.card);
  if(th.text)  root.style.setProperty("--text", th.text);
  if(th.primary) root.style.setProperty("--primary", th.primary);
  if(th.accent)  root.style.setProperty("--accent", th.accent);
}
function setTheme(){
  const th={
    primary: $("#cPrimary")?.value,
    accent:  $("#cAccent")?.value,
    bg:      $("#cBg")?.value,
    card:    $("#cCard")?.value,
    text:    $("#cText")?.value,
  };
  localStorage.setItem("theme", JSON.stringify(th));
  applyTheme(th);
}
applyTheme(getTheme());
