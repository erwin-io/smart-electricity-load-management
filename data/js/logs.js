/* global $, apiGet, apiPost */

const qs = new URLSearchParams(location.search);
function toISOLocal(dt){ const pad=v=>String(v).padStart(2,"0"); return dt.getFullYear()+"-"+pad(dt.getMonth()+1)+"-"+pad(dt.getDate())+"T"+pad(dt.getHours())+":"+pad(dt.getMinutes()); }
function getRange(){ const f=$("dtFrom").value, t=$("dtTo").value, p=new URLSearchParams(); if(f) p.set("from",f+":00"); if(t) p.set("to",t+":59"); return p.toString(); }
function setDefaultRange(){ const now=new Date(), from=new Date(now); from.setHours(0,0,0,0); $("dtFrom").value=toISOLocal(from); $("dtTo").value=toISOLocal(now); $("hint").textContent="Showing local time (Asia/Manila)."; }

function renderRows(items){
  const tb=$("rows"); tb.innerHTML="";
  const nf6=new Intl.NumberFormat(undefined,{minimumFractionDigits:6, maximumFractionDigits:6});
  for(const r of items){
    const tr=document.createElement("tr");
    tr.innerHTML = `
      <td style="padding:8px;border-bottom:1px solid var(--border)">${r.timestamp}</td>
      <td style="padding:8px;border-bottom:1px solid var(--border);text-align:right">${nf6.format(r.budget_kwh)}</td>
      <td style="padding:8px;border-bottom:1px solid var(--border);text-align:right">${nf6.format(r.remaining_kwh)}</td>
      <td style="padding:8px;border-bottom:1px solid var(--border);text-align:right">${nf6.format(r.used_kwh)}</td>`;
    tb.appendChild(tr);
  }
  $("meta").textContent = `${items.length} row(s)`;
}

async function load(){
  const q = getRange(); const res = await fetch(`/api/logs/query?${q}`,{credentials:"include"});
  if(!res.ok){ alert(await res.text()||"Failed to load"); return; }
  const json = await res.json(); renderRows(json.items||[]);
}

$("btnLoad").onclick=load;
$("btnExport").onclick=()=>{ const q=getRange(); const a=document.createElement("a"); a.href=`/api/logs/export.xls?${q}`; a.download="smartload_logs.xls"; document.body.appendChild(a); a.click(); a.remove(); };
$("btnPrint").onclick=()=>{ const q=getRange(); window.open(`/logs_print.html?${q}`,"_blank"); };

$("btnLogout").onclick=async()=>{ try{ await apiPost("/api/logout",{}); location.href="/login"; }catch(e){ alert(e); } };

setDefaultRange(); load();
