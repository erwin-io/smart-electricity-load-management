#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PZEM004Tv30.h>

// ===== Wi-Fi creds =====
#define STA_SSID "HG8145V5_D0A04"
#define STA_PASS "p75z~${Tn2Iy"

// ===== PZEM wiring =====
static const int PZEM_RX = 26; // ESP32 RX2 <- PZEM TX
static const int PZEM_TX = 25; // ESP32 TX2 -> PZEM RX
HardwareSerial PZEMSerial(2);
PZEM004Tv30 pzem(PZEMSerial, PZEM_RX, PZEM_TX);

// ===== Relays =====
struct Relay { uint8_t pin; bool activeHigh; };
Relay prio4[] = { {4,true} };
Relay prio3[] = { {5,true},{13,true},{14,true},{16,true} };
Relay prio2[] = { {17,true},{18,true},{19,true} };
Relay prio1[] = { {21,true},{22,false},{23,false} };

static inline void relayWrite(const Relay &r, bool on){
  digitalWrite(r.pin, r.activeHigh ? (on?HIGH:LOW) : (on?LOW:HIGH));
}
template<size_t N> static inline void setGroup(Relay (&grp)[N], bool on){ for(auto&r:grp) relayWrite(r,on); }
static inline void allGroups(bool on){ setGroup(prio1,on); setGroup(prio2,on); setGroup(prio3,on); setGroup(prio4,on); }
static void initGroupPins(){
  auto init=[&](Relay* a,size_t n){ for(size_t i=0;i<n;i++){ pinMode(a[i].pin,OUTPUT); relayWrite(a[i],false);} };
  init(prio4,sizeof(prio4)/sizeof(Relay));
  init(prio3,sizeof(prio3)/sizeof(Relay));
  init(prio2,sizeof(prio2)/sizeof(Relay));
  init(prio1,sizeof(prio1)/sizeof(Relay));
}

// ===== Control state =====
float  budgetKWh = 0.004f;        // default 4 Wh
double energyBaseline = 0.0;      // baseline when (re)started
bool   paused = true;             // start paused, relays OFF
bool   firstResume = true;

// Robust energy reading
double lastGoodTotal = 0.0;       // last accepted total kWh from PZEM
bool   haveLastGood  = false;

// Hysteresis (percentage points) to prevent chattering at band edges
const float BAND_HYST = 2.0f;
const float PCT_EPS   = 0.0001f;  // [CHANGED] epsilon for "effectively zero"

// API / server
AsyncWebServer server(80);

// ===== Status & Zones =====
struct Status {
  float  remainingPct = 100.0f;
  double usedKWh = 0.0;
  double remKWh  = 0.0;
  bool p1=false,p2=false,p3=false,p4=false;
  bool paused=true;
  float budget=0.004f;
};

Status currentStatus;   // global, exported to UI

enum Zone : uint8_t {
  Z0_CUTOFF  = 0, // [CHANGED] 0% => ALL OFF
  Z1_ONLY1   = 1, // <10%
  Z2_ONLY12  = 2, // 10-39%
  Z3_OFF4    = 3, // 40-69%
  Z4_ALL     = 4  // 70-100%
};
Zone currentZone = Z4_ALL;

// ===== Helpers =====
double readTotalKWhFiltered() {
  double e = pzem.energy();
  // Reject invalid/NaN
  if (isnan(e) || e < 0) {
    return haveLastGood ? lastGoodTotal : 0.0;
  }
  // Monotonic check with small tolerance
  if (haveLastGood && e + 0.0005 < lastGoodTotal) {
    return lastGoodTotal;
  }
  lastGoodTotal = e;
  haveLastGood  = true;
  return lastGoodTotal;
}

// [CHANGED] Explicit cutoff handling integrated with hysteresis
static inline Zone zoneFromPctWithHyst(float pct, Zone prev) {
  // Hard cutoff: if effectively zero, force all OFF regardless of previous zone
  if (pct <= PCT_EPS) return Z0_CUTOFF;

  switch (prev) {
    case Z4_ALL:    // dropping
      if (pct < (69.0f - BAND_HYST)) return Z3_OFF4;
      return Z4_ALL;

    case Z3_OFF4:
      if (pct >= (70.0f + BAND_HYST)) return Z4_ALL;
      if (pct <  (39.0f - BAND_HYST)) return Z2_ONLY12;
      // also guard for near-zero dips
      if (pct <= (0.0f + BAND_HYST))  return Z0_CUTOFF;
      return Z3_OFF4;

    case Z2_ONLY12:
      if (pct >= (40.0f + BAND_HYST)) return Z3_OFF4;
      if (pct <  (9.0f  - BAND_HYST)) return Z1_ONLY1;
      if (pct <= (0.0f + BAND_HYST))  return Z0_CUTOFF;
      return Z2_ONLY12;

    case Z1_ONLY1:
      if (pct >= (10.0f + BAND_HYST)) return Z2_ONLY12;
      if (pct <= (0.0f + BAND_HYST))  return Z0_CUTOFF;
      return Z1_ONLY1;

    case Z0_CUTOFF:
      // rising out of zero -> next valid zone starts at ~>0
      if (pct >  (0.0f + BAND_HYST))  return Z1_ONLY1;
      return Z0_CUTOFF;
  }
  return prev;
}

void restartCycle() {
  double t = readTotalKWhFiltered();
  energyBaseline = t;
  firstResume = false;
}

// ===== Compute (no GPIO writes here!) =====
Status computeStatus() {
  static double frozenUsed = 0.0, frozenRem = budgetKWh;
  static float  frozenPct  = 100.0f;

  Status s = currentStatus; // start from last (keeps budget/paused)

  if (paused) {
    // Keep last shown numbers; do not recompute used from sensor while paused.
    s.usedKWh      = frozenUsed;
    s.remKWh       = frozenRem;
    s.remainingPct = frozenPct;
    s.paused       = true;
    s.p1=s.p2=s.p3=s.p4=false;
    s.budget       = budgetKWh;
    return s;
  }

  // Running: compute from filtered sensor value
  double total = readTotalKWhFiltered();
  double used  = max(0.0, total - energyBaseline);
  double rem   = max(0.0, budgetKWh - used);

  // [CHANGED] Consistent clamping
  float pct = 0.0f;
  if (budgetKWh > 0.0f) {
    pct = (float)(rem * 100.0 / budgetKWh);
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
  }

  // Update frozen copies (so Stop can freeze the latest values)
  frozenUsed = used;
  frozenRem  = rem;
  frozenPct  = pct;

  s.usedKWh      = used;
  s.remKWh       = rem;
  s.remainingPct = pct;
  s.paused       = false;
  s.budget       = budgetKWh;

  // Determine desired zone with hysteresis (now includes hard 0% cutoff)
  Zone newZone = zoneFromPctWithHyst(pct, currentZone);
  currentZone  = newZone;

  // Map zone to desired relay states (no GPIO writes here)
  switch (newZone) {
    case Z4_ALL:     s.p1=s.p2=s.p3=s.p4=true;  break;
    case Z3_OFF4:    s.p1=s.p2=s.p3=true;  s.p4=false; break;
    case Z2_ONLY12:  s.p1=s.p2=true;       s.p3=s.p4=false; break;
    case Z1_ONLY1:   s.p1=true;            s.p2=s.p3=s.p4=false; break;
    case Z0_CUTOFF:  // [CHANGED] 0% => ALL OFF
    default:         s.p1=s.p2=s.p3=s.p4=false; break;
  }
  return s;
}

// ===== Enforce (GPIO writes only here) =====
void enforceRelays(const Status& s) {
  if (s.paused) { allGroups(false); return; }
  setGroup(prio1, s.p1);
  setGroup(prio2, s.p2);
  setGroup(prio3, s.p3);
  setGroup(prio4, s.p4);
}

// ===== UI HTML =====
String htmlPage(){
  return R"HTML(

<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Load</title>
<style>
body{font-family:Arial;background:#0b1220;color:#eee;padding:16px}
.card{background:#111a2b;border-radius:12px;padding:16px;margin:10px 0}
.gauge{width:200px;height:200px;border-radius:50%;margin:auto;display:grid;place-items:center;
background:conic-gradient(#3ad53a var(--p,0%),#2c394f 0%)}
.gauge>div{width:70%;height:70%;border-radius:50%;background:#0b1220;display:grid;place-items:center}
.val{font-weight:bold;font-size:22px}
.on{color:#3ad53a}.off{color:#ef5350}
button{margin:5px;padding:10px 20px;border-radius:8px;border:none;cursor:pointer}
button:disabled{opacity:0.5;cursor:not-allowed}
.small{font-size:12px;opacity:0.75}
</style></head><body>
<h2>⚡ Smart Load Manager</h2>
<div class="card" style="text-align:center">
  <div id="g" class="gauge" style="--p:0%"><div><div>
    <div class="val"><span id="pctInt">0</span>%</div>
    <div class="small">exact: <span id="pctExact">0.00</span>%</div>
  </div></div></div>
</div>
<div class="card">
  <div>
    Budget: <input id="budgetInput" type="number" step="0.001" min="0" style="width:100px" disabled>
    kWh <button id="saveBtn" onclick="saveBudget()" disabled>Save</button>
  </div>
  <div>Used: <span id="used">0.000</span> kWh</div>
  <div>Remaining: <span id="rem">0.004</span> kWh</div>
</div>
<div class="card">
  <div>Relay P1: <span id="p1">OFF</span></div>
  <div>Relay P2: <span id="p2">OFF</span></div>
  <div>Relay P3: <span id="p3">OFF</span></div>
  <div>Relay P4: <span id="p4">OFF</span></div>
</div>
<div class="card" style="text-align:center">
  <button id="stopBtn" onclick="stop()">Stop</button>
  <button id="resumeBtn" onclick="resume()">Resume</button>
  <button id="restartBtn" onclick="restart()" disabled>Restart</button>
</div>
<script>
let paused=true;
let budgetDirty=false;

async function fetchStatus(){
  const r=await fetch('/api/status'); const s=await r.json();
  const pct=Math.max(0,Math.min(100,s.remainingPct));
  document.getElementById('g').style.setProperty('--p',Math.floor(pct)+'%');           // gauge fill (int)
  document.getElementById('pctInt').textContent=Math.floor(pct);                        // [CHANGED]
  document.getElementById('pctExact').textContent=pct.toFixed(2);                       // [CHANGED]

  if(!budgetDirty){ document.getElementById('budgetInput').value=s.budget.toFixed(3); }
  document.getElementById('used').textContent=s.usedKWh.toFixed(3);
  document.getElementById('rem').textContent=s.remKWh.toFixed(3);

  ['p1','p2','p3','p4'].forEach(id=>{
    const el=document.getElementById(id);
    el.textContent=s[id]?'ON':'OFF'; el.className=s[id]?'on':'off';
  });

  paused=s.paused;
  document.getElementById('budgetInput').disabled=!paused;
  document.getElementById('stopBtn').disabled=paused;
  document.getElementById('resumeBtn').disabled=!paused;
  if(paused && !budgetDirty) document.getElementById('restartBtn').disabled=false;
}
function onBudgetEdit(){
  if(paused){
    budgetDirty=true;
    document.getElementById('saveBtn').disabled=false;
    document.getElementById('restartBtn').disabled=true;
  }
}
document.addEventListener('DOMContentLoaded',()=>{
  document.getElementById('budgetInput').addEventListener('input',onBudgetEdit);
});
async function saveBudget(){
  const v=parseFloat(document.getElementById('budgetInput').value);
  if(isNaN(v)||v<=0) return alert('Invalid budget');
  await fetch('/api/budget?val='+encodeURIComponent(v),{method:'POST'});
  document.getElementById('saveBtn').disabled=true;
  document.getElementById('restartBtn').disabled=false;
  budgetDirty=false;
}
async function stop(){await fetch('/api/stop',{method:'POST'});fetchStatus();}
async function resume(){await fetch('/api/resume',{method:'POST'});fetchStatus();}
async function restart(){await fetch('/api/restart',{method:'POST'});fetchStatus();}
setInterval(fetchStatus,1000);fetchStatus(); </script></body></html>
)HTML";
}

// ===== HTTP Handlers =====
void handleStatus(AsyncWebServerRequest*req){
  JsonDocument doc;
  doc["remainingPct"]=currentStatus.remainingPct;
  doc["usedKWh"]=currentStatus.usedKWh;
  doc["remKWh"]=currentStatus.remKWh;
  doc["p1"]=currentStatus.p1; doc["p2"]=currentStatus.p2; doc["p3"]=currentStatus.p3; doc["p4"]=currentStatus.p4;
  doc["paused"]=currentStatus.paused;
  doc["budget"]=currentStatus.budget;
  String out; serializeJson(doc,out);
  req->send(200,"application/json",out);
}
void handleStop(AsyncWebServerRequest*req){ paused=true; req->send(200); }
void handleResume(AsyncWebServerRequest*req){ if(firstResume) restartCycle(); paused=false; req->send(200); }
void handleRestart(AsyncWebServerRequest*req){ restartCycle(); paused=false; req->send(200); }
void handleBudget(AsyncWebServerRequest*req){
  if(!req->hasParam("val")){ req->send(400,"text/plain","Missing val"); return; }
  float v=req->getParam("val")->value().toFloat();
  if(v<=0.0f||isnan(v)){ req->send(400,"text/plain","Invalid val"); return; }
  budgetKWh=v; currentStatus.budget=budgetKWh; req->send(200,"text/plain","OK");
}

// ===== Wi-Fi =====
void startWiFi(){
  WiFi.mode(WIFI_STA); WiFi.begin(STA_SSID,STA_PASS);
  Serial.printf("Connecting to %s",STA_SSID);
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.printf("\nConnected! IP=%s\n",WiFi.localIP().toString().c_str());
}

// ===== Setup / Loop =====
void setup(){
  Serial.begin(115200);
  initGroupPins(); allGroups(false); // default OFF
  startWiFi();
  PZEMSerial.begin(9600,SERIAL_8N1,PZEM_RX,PZEM_TX);

  // Web
  server.on("/",HTTP_GET,[](AsyncWebServerRequest*r){r->send(200,"text/html",htmlPage());});
  server.on("/api/status",HTTP_GET,handleStatus);
  server.on("/api/stop",HTTP_POST,handleStop);
  server.on("/api/resume",HTTP_POST,handleResume);
  server.on("/api/restart",HTTP_POST,handleRestart);
  server.on("/api/budget",HTTP_POST,handleBudget);
  server.begin();
}

void loop(){
  // 1) Compute next status from sensor + state
  currentStatus = computeStatus();
  // 2) Enforce relays once here (never inside HTTP handlers)
  enforceRelays(currentStatus);

  // Debug print once per second
  static uint32_t lastPrint=0;
  if (millis() - lastPrint >= 1000) {
    Serial.printf("pct %.6f%%\n", currentStatus.remainingPct);
    Serial.printf("usedKWh %.6f\n", currentStatus.usedKWh);
    lastPrint = millis();
  }

  delay(200);
}
