function togglePwd(){
  const p = $("p");
  p.type = p.type==="password" ? "text" : "password";
}

async function login(){
  const u = $("u").value.trim();
  const p = $("p").value;
  const msg = $("msg");
  if(!u){ msg.textContent="Username required."; return; }
  if(!p){ msg.textContent="Password required."; return; }
  try{
    const r = await fetch("/api/login",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({username:u,password:p})});
    if(r.ok){ location.href="/dashboard"; return; }
    const t = await r.text();
    if(t.toLowerCase().includes("invalid credentials")) {
      msg.textContent = "Invalid credentials.";
    } else {
      msg.textContent = t || "Invalid credentials.";
    }
  } catch(e){
    msg.textContent = e.message || "Network error.";
  }
}
  // Get all input tags inside it
  const inputs = $("login").getElementsByTagName("input");

  // Example: loop through and log values
  for (let i = 0; i < inputs.length; i++) {
    inputs[i].addEventListener("keydown", function(event) {
      if (event.key === "Enter") {
        login();
        event.preventDefault();
      }
    });
  }