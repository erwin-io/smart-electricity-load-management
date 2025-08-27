  async function loadConfig(){
    try{
      const c = await apiGet("/api/config");
      $("cfgUser").value = c.username || "";
      $("cfgBudget").value = (c.budget_kwh || 0).toFixed(3);
      $("tUsage").checked = !!c.show_usage_graph;
      $("tPrioS").checked = !!c.show_prio_status;
      $("tPrioC").checked = !!c.show_prio_controls;
      $("msgAuth").textContent = "";
      $("msgOpt").textContent = "";
    }catch(e){
      $("msgAuth").textContent = e.message || "Failed to load.";
    }
  }
  async function saveAuth(){
    const username = $("cfgUser").value.trim();
    const password = $("cfgPass").value;
    if(!username){ $("msgAuth").textContent="Username required."; return; }
    if(!password){ $("msgAuth").textContent="Password required."; return; }
    try{
      await apiPost("/api/config",{ username, password });
      $("msgAuth").textContent="Saved.";
      $("cfgPass").value="";
    }catch(e){ $("msgAuth").textContent=e.message; }
  }
  async function saveOptions(){
    const budget_kwh = parseFloat($("cfgBudget").value);
    const show_usage_graph = $("tUsage").checked;
    const show_prio_status = $("tPrioS").checked;
    const show_prio_controls = $("tPrioC").checked;
    if(isNaN(budget_kwh) || budget_kwh<=0){ $("msgOpt").textContent="Invalid budget"; return; }
    try{
      await apiPost("/api/config",{ budget_kwh, show_usage_graph, show_prio_status, show_prio_controls });
      $("msgOpt").textContent="Saved.";
    }catch(e){ $("msgOpt").textContent=e.message; }
  }
  loadConfig();
