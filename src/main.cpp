#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <PZEM004Tv30.h>
#include <math.h>

/* ===================== Wi-Fi (STA) ===================== */
#define STA_SSID "HG8145V5_D0A04"
#define STA_PASS "p75z~${Tn2Iy"

/* ===================== PZEM wiring (UART2) ===================== */
// ESP32 RX2 <- PZEM TX (via divider), ESP32 TX2 -> PZEM RX
static const int PZEM_RX = 26;
static const int PZEM_TX = 25;
HardwareSerial PZEMSerial(2);
PZEM004Tv30 pzem(PZEMSerial, PZEM_RX, PZEM_TX);

/* ===================== SD card (Mini Data Logger) ===================== */
#define SD_CS 5
RTC_DS3231 rtc;

/* ===================== Relays ===================== */
struct Relay { uint8_t pin; bool activeHigh; };
// P4: 1 relay
Relay prio4[] = { {4,  true} };
// P3: 4 relays
Relay prio3[] = { {13, true}, {14, true}, {16, true}, {27, true} };
// P2: 3 relays
Relay prio2[] = { {17, true}, {32, true}, {33, true} };
// P1: 3 relays (strap pins safe)
Relay prio1[] = { {12, true}, {15, false}, {2, false} };

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

/* ===================== Control state ===================== */
float  budgetKWh = 0.004f;     // default 4 Wh
double energyBaseline = 0.0;   // baseline of *virtual* energy
bool   paused = true;          // start paused
bool   firstResume = true;
bool   baselineFromSnapshot = false;

// PZEM readings / hybrid integrator
double   lastGoodTotal = 0.0;
bool     haveLastGood  = false;
uint32_t lastEnergyUpdateMs = 0;
double   softAccumKWh = 0.0;
double   lastPowerW    = 0.0;
double   lastVoltageV  = 0.0;
double   lastCurrentA  = 0.0;

const float  BAND_HYST = 2.0f;
const float  PCT_EPS   = 0.0001f;
const double ENERGY_BACKSTEP_EPS = 0.0005; // kWh
const uint32_t STALE_MS = 8000;            // hardware energy stale window
const double POWER_STALE_W = 10.0;         // consider load present

AsyncWebServer server(80);
uint8_t manualMask = 0; // bit0=P1..bit3=P4

/* ===================== Status ===================== */
struct Status {
  float  remainingPct = 100.0f;
  double usedKWh = 0.0;
  double remKWh  = 0.0;
  bool p1=false,p2=false,p3=false,p4=false;
  bool paused=true;
  float budget=0.004f;
};
Status currentStatus;

enum Zone : uint8_t { Z0_CUTOFF=0, Z1_ONLY1=1, Z2_ONLY12=2, Z3_OFF4=3, Z4_ALL=4 };
Zone currentZone = Z4_ALL;

/* ===================== Config (CSV) ===================== */
struct AppConfig {
  String username = "admin";
  String password = "admin";
  float  budget_kwh = 0.004f;
  bool   show_usage_graph = true;
  bool   show_prio_status = true;
  bool   show_prio_controls = false;
} appcfg;

/* ===================== Forwards ===================== */
double virtualTotalKWh();
void   savePauseSnapshot();
bool   loadPauseSnapshot();

/* ===================== FS helpers ===================== */
bool fileExists(fs::FS &fs, const char* path){
  File f = fs.open(path); if(!f) return false; f.close(); return true;
}

/* ===================== Config load/save ===================== */
bool loadConfig(){
  if(!SD.begin(SD_CS)) { Serial.println("[SD] begin failed in loadConfig"); return false; }
  if(!fileExists(SD, "/config.csv")){
    File f = SD.open("/config.csv","w");
    if(!f) return false;
    f.printf("username,password,budget_kwh,show_usage_graph,show_prio_status,show_prio_controls\n");
    f.printf("%s,%s,%.3f,%d,%d,%d\n",
      appcfg.username.c_str(), appcfg.password.c_str(), appcfg.budget_kwh,
      appcfg.show_usage_graph?1:0, appcfg.show_prio_status?1:0, appcfg.show_prio_controls?1:0);
    f.close();
    return true;
  }
  File f = SD.open("/config.csv","r");
  if(!f) return false;
  String header = f.readStringUntil('\n');
  String line   = f.readStringUntil('\n');
  f.close();
  if(line.length()==0) return false;

  int idx=0; String parts[6]; int start=0;
  for(int i=0;i<line.length();++i){
    if(line[i]==',' || i==line.length()-1){
      int end = (i==line.length()-1)? i+1 : i;
      parts[idx++] = line.substring(start,end);
      start = i+1; if(idx>=6) break;
    }
  }
  if(idx<6) return false;

  appcfg.username = parts[0];
  appcfg.password = parts[1];
  appcfg.budget_kwh = parts[2].toFloat();
  appcfg.show_usage_graph = parts[3].toInt()!=0;
  appcfg.show_prio_status = parts[4].toInt()!=0;
  appcfg.show_prio_controls = parts[5].toInt()!=0;

  budgetKWh = appcfg.budget_kwh;
  currentStatus.budget = budgetKWh;
  return true;
}

void persistConfig(){
  if(!SD.begin(SD_CS)) return;
  File f = SD.open("/config.csv","w");
  if(!f) return;
  f.printf("username,password,budget_kwh,show_usage_graph,show_prio_status,show_prio_controls\n");
  f.printf("%s,%s,%.3f,%d,%d,%d\n",
    appcfg.username.c_str(), appcfg.password.c_str(), appcfg.budget_kwh,
    appcfg.show_usage_graph?1:0, appcfg.show_prio_status?1:0, appcfg.show_prio_controls?1:0);
  f.close();
}

/* ===================== Time / NTP / RTC ===================== */
bool rtcReady=false;
bool ntpSynced=false;

String two(int v){ if(v<10) return "0"+String(v); return String(v); }
String ampmHour(int h24){ int h=h24%12; if(h==0) h=12; return String(h); }
String ampmStr(int h24){ return (h24<12)?"AM":"PM"; }

bool syncNTP(){
  configTime(8*3600, 0, "pool.ntp.org", "time.nist.gov"); // Asia/Manila
  for(int i=0;i<25;i++){ struct tm t; if(getLocalTime(&t)) return true; delay(200); }
  return false;
}
DateTime nowLocal(){
  struct tm tt;
  if(getLocalTime(&tt)){ return DateTime(1900+tt.tm_year, 1+tt.tm_mon, tt.tm_mday, tt.tm_hour, tt.tm_min, tt.tm_sec); }
  if(rtcReady) return rtc.now();
  return DateTime(2025,1,1,0,0,0);
}

/* ===================== Logging to CSV (SD) ===================== */
String currentLogName=""; int currentLogHour=-1;
String makeLogName(const DateTime& dt){
  String y = String(dt.year());
  String m = two(dt.month());
  String d = two(dt.day());
  String ap = ampmStr(dt.hour());
  String hh = ampmHour(dt.hour());
  return "/logs_" + y + m + d + "_" + hh + "_" + ap + ".csv";
}
File openLogFile(const String& name, bool &created){
  created=false; if(!SD.begin(SD_CS)) return File();
  bool exists = fileExists(SD, name.c_str());
  File f = SD.open(name.c_str(), exists ? "a" : "w");
  if(!f) return File();
  if(!exists){ created=true; f.println("timestamp,budget_kwh,remaining_kwh,used_kwh"); }
  return f;
}
double lastLoggedUsed=-1,lastLoggedRem=-1,lastLoggedBudget=-1;
const double LOG_EPS=0.0005;
bool significantlyDiff(double a, double b){ if(a<0||b<0) return true; return fabs(a-b)>LOG_EPS; }
bool forceLogNext = false;

void appendLogMaybe(){
  if(paused && !forceLogNext) return;

  DateTime dt=nowLocal();
  String fname=makeLogName(dt);
  int hourNow=dt.hour();
  if(currentLogName!=fname){
    currentLogName=fname; currentLogHour=hourNow;
    lastLoggedUsed=-1; lastLoggedRem=-1; lastLoggedBudget=-1;
  }

  bool changed = significantlyDiff(currentStatus.usedKWh,lastLoggedUsed)
              || significantlyDiff(currentStatus.remKWh,lastLoggedRem)
              || significantlyDiff(budgetKWh,lastLoggedBudget)
              || forceLogNext;

  if(!changed) return;

  bool created=false; File f=openLogFile(currentLogName,created); if(!f) return;
  String ts = String(dt.year())+"-"+two(dt.month())+"-"+two(dt.day())+" "+two(dt.hour())+":"+two(dt.minute())+":"+two(dt.second());
  f.printf("%s,%.6f,%.6f,%.6f\n", ts.c_str(), (double)budgetKWh, (double)currentStatus.remKWh, (double)currentStatus.usedKWh);
  f.close();

  lastLoggedUsed=currentStatus.usedKWh;
  lastLoggedRem =currentStatus.remKWh;
  lastLoggedBudget=budgetKWh;
  forceLogNext = false;
}

/* ===================== Pause snapshot on LittleFS ===================== */
double frozenUsed = 0.0;
double frozenRem  = 0.004;
float  frozenPct  = 100.0f;

void savePauseSnapshot(){
  if(!LittleFS.begin(false,"/littlefs")) return;
  JsonDocument d;
  d["paused"]       = paused;
  d["usedKWh"]      = frozenUsed;
  d["remKWh"]       = frozenRem;
  d["pct"]          = frozenPct;
  d["budget"]       = budgetKWh;
  d["baseline_kwh"] = energyBaseline;
  File f = LittleFS.open("/state.json","w");
  if(f){ serializeJson(d,f); f.close(); }
}

bool loadPauseSnapshot(){
  if(!LittleFS.begin(false,"/littlefs")) return false;
  if(!LittleFS.exists("/state.json"))    return false;
  File f = LittleFS.open("/state.json","r");
  if(!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d,f);
  f.close();
  if(e) return false;

  energyBaseline = d["baseline_kwh"] | 0.0;
  baselineFromSnapshot = d.containsKey("baseline_kwh");

  frozenUsed = d["usedKWh"] | 0.0;
  frozenRem  = d["remKWh"]  | budgetKWh;
  frozenPct  = d["pct"]     | 100.0f;

  paused = true;
  Serial.printf("[STATE] restored: budget=%.6f rem=%.6f used=%.6f (%.1f%%)\n",
                (double)budgetKWh,(double)frozenRem,(double)frozenUsed,(double)frozenPct);
  return true;
}

/* ===================== CSV restore (FIXED) ===================== */
static bool isLogCsvName(String nm){
  // accept with or without leading '/'
  String low = nm; low.toLowerCase();
  if(low.length() && low[0]=='/') low.remove(0,1);
  return low.startsWith("logs_") && low.endsWith(".csv");
}
static String ensureLeadingSlash(String nm){
  if(nm.length() && nm[0] != '/') nm = "/" + nm;
  return nm;
}
static String findLatestLogCsv(){
  if(!SD.begin(SD_CS)) { Serial.println("[SD] begin failed in findLatestLogCsv"); return ""; }
  File root = SD.open("/");
  if(!root || !root.isDirectory()) { Serial.println("[CSV] root open failed"); return ""; }
  String latest = "";
  while(true){
    File f = root.openNextFile();
    if(!f) break;
    if(!f.isDirectory()){
      String nm = String(f.name());  // often "logs_*.csv" (no slash)
      if(isLogCsvName(nm)){
        nm = ensureLeadingSlash(nm);
        if(latest == "" || nm > latest) latest = nm;
      }
    }
    f.close();
  }
  root.close();
  if(latest!="") Serial.printf("[CSV] Latest=%s\n", latest.c_str());
  else Serial.println("[CSV] No logs_*.csv found");
  return latest;
}
static bool loadSnapshotFromCsv(){
  if(!SD.begin(SD_CS)) { Serial.println("[SD] begin failed in loadSnapshotFromCsv"); return false; }
  String csv = findLatestLogCsv();
  if(csv == "") return false;

  File f = SD.open(csv, "r");
  if(!f){ Serial.println("[CSV] open failed"); return false; }

  String lastLine = "";
  while (f.available()){
    String line = f.readStringUntil('\n');
    line.trim();
    if(line.length()==0) continue;
    if(line.startsWith("timestamp")) continue;
    lastLine = line;
  }
  f.close();
  if(lastLine==""){ Serial.println("[CSV] empty file"); return false; }

  // timestamp,budget_kwh,remaining_kwh,used_kwh
  int c1 = lastLine.indexOf(',');
  if(c1<0) return false;
  int c2 = lastLine.indexOf(',', c1+1);
  if(c2<0) return false;
  int c3 = lastLine.indexOf(',', c2+1);
  if(c3<0) return false;

  float b   = lastLine.substring(c1+1, c2).toFloat();
  float rem = lastLine.substring(c2+1, c3).toFloat();
  float used= lastLine.substring(c3+1).toFloat();
  if(!(b>0.0f)) { Serial.println("[CSV] invalid budget in last line"); return false; }

  budgetKWh  = b;
  appcfg.budget_kwh = b;
  currentStatus.budget = b;

  frozenRem  = rem;
  frozenUsed = used;
  frozenPct  = (b>0) ? (float)(rem*100.0/b) : 0.0f;
  frozenPct  = min(100.0f, max(0.0f, frozenPct));

  // align baseline so that used = (virtualTotal - baseline)
  double vt = virtualTotalKWh();            // single read; may be ~0 at boot
  energyBaseline = vt - (double)frozenUsed;
  baselineFromSnapshot = true;

  paused = true;
  savePauseSnapshot();

  Serial.printf("[RESTORE] budget=%.6f rem=%.6f used=%.6f (%.1f%%)\n",
                (double)budgetKWh, (double)frozenRem, (double)frozenUsed, (double)frozenPct);
  return true;
}

/* ===================== Energy model ===================== */
double virtualTotalKWh(){
  static uint32_t lastMs = millis();
  uint32_t now = millis();
  double dtHours = (now - lastMs) / 3600000.0;
  lastMs = now;

  double v = pzem.voltage();  if(!isnan(v) && v>=0) lastVoltageV = v;
  double c = pzem.current();  if(!isnan(c) && c>=0) lastCurrentA = c;
  double p = pzem.power();    if(!isnan(p) && p>=0) lastPowerW   = p;

  double e = pzem.energy();
  if(!isnan(e) && e>=0){
    if(!haveLastGood || e >= lastGoodTotal - ENERGY_BACKSTEP_EPS){
      haveLastGood = true;
      lastGoodTotal = max(lastGoodTotal, e);
      lastEnergyUpdateMs = now;
    }
  }

  if((now - lastEnergyUpdateMs) > STALE_MS && lastPowerW > POWER_STALE_W){
    softAccumKWh += (lastPowerW/1000.0) * dtHours;
  }

  return (haveLastGood ? lastGoodTotal : 0.0) + softAccumKWh;
}

void restartCycle(){
  double vt = virtualTotalKWh();
  energyBaseline = vt;
  softAccumKWh = 0.0;
  firstResume=false;
  baselineFromSnapshot = true;
  savePauseSnapshot();
}

/* ===================== Auto zoning / status ===================== */
static inline Zone zoneFromPctWithHyst(float pct, Zone prev){
  if(pct<=PCT_EPS) return Z0_CUTOFF;
  switch(prev){
    case Z4_ALL:   if(pct<(69.0f-BAND_HYST)) return Z3_OFF4;   return Z4_ALL;
    case Z3_OFF4:  if(pct>=(70.0f+BAND_HYST)) return Z4_ALL;   if(pct<(39.0f-BAND_HYST)) return Z2_ONLY12; if(pct<=(0.0f+BAND_HYST)) return Z0_CUTOFF; return Z3_OFF4;
    case Z2_ONLY12:if(pct>=(40.0f+BAND_HYST)) return Z3_OFF4;  if(pct<(9.0f-BAND_HYST))  return Z1_ONLY1;  if(pct<=(0.0f+BAND_HYST)) return Z0_CUTOFF; return Z2_ONLY12;
    case Z1_ONLY1: if(pct>=(10.0f+BAND_HYST)) return Z2_ONLY12; if(pct<=(0.0f+BAND_HYST)) return Z0_CUTOFF; return Z1_ONLY1;
    case Z0_CUTOFF:if(pct>(0.0f+BAND_HYST))  return Z1_ONLY1;  return Z0_CUTOFF;
  }
  return prev;
}

Status computeStatus(){
  Status s=currentStatus;

  if(paused){
    s.usedKWh      = frozenUsed;
    s.remKWh       = frozenRem;
    s.remainingPct = frozenPct;
    s.paused       = true;
    s.budget       = budgetKWh;
    s.p1=s.p2=s.p3=s.p4=false;
    return s;
  }

  double total = virtualTotalKWh();
  double used  = max(0.0, total - energyBaseline);
  double rem   = max(0.0, budgetKWh - used);
  float  pct   = 0.0f;
  if(budgetKWh>0.0f){
    pct = (float)(rem*100.0/budgetKWh);
    pct = min(100.0f, max(0.0f, pct));
  }

  frozenUsed = used;
  frozenRem  = rem;
  frozenPct  = pct;

  s.usedKWh=used; s.remKWh=rem; s.remainingPct=pct; s.paused=false; s.budget=budgetKWh;

  Zone newZone = zoneFromPctWithHyst(pct, currentZone);
  currentZone=newZone;
  bool z1=false,z2=false,z3=false,z4=false;
  switch(newZone){
    case Z4_ALL:  z1=z2=z3=z4=true; break;
    case Z3_OFF4: z1=z2=z3=true; break;
    case Z2_ONLY12: z1=z2=true; break;
    case Z1_ONLY1: z1=true; break;
    case Z0_CUTOFF: default: break;
  }

  if(appcfg.show_prio_controls && manualMask){
    s.p1 = (manualMask & 0x01) ? currentStatus.p1 : z1;
    s.p2 = (manualMask & 0x02) ? currentStatus.p2 : z2;
    s.p3 = (manualMask & 0x04) ? currentStatus.p3 : z3;
    s.p4 = (manualMask & 0x08) ? currentStatus.p4 : z4;
  } else {
    s.p1=z1; s.p2=z2; s.p3=z3; s.p4=z4;
  }

  return s;
}
void enforceRelays(const Status& s){
  if(s.paused){ allGroups(false); return; }
  setGroup(prio1,s.p1); setGroup(prio2,s.p2); setGroup(prio3,s.p3); setGroup(prio4,s.p4);
}

/* ===================== Auth ===================== */
String sessionId=""; bool isLoggedIn=false;
String randomHex(uint8_t n=16){
  String s; s.reserve(n*2);
  for(uint8_t i=0;i<n;i++){ uint8_t b=random(0,256); if(b<16)s+='0'; s+=String(b,HEX); }
  return s;
}
bool hasAuth(AsyncWebServerRequest* req){
  if(!isLoggedIn) return false;
  if(!req->hasHeader("Cookie")) return false;
  String ck=req->header("Cookie");
  return ck.indexOf("SID="+sessionId)>=0;
}
void requireAuth(AsyncWebServerRequest* req, const char* pathIfOk){
  if(hasAuth(req)) req->send(LittleFS, pathIfOk, "text/html");
  else req->redirect("/login");
}

/* ===================== HTTP APIs ===================== */
void handleStatus(AsyncWebServerRequest*req){
  JsonDocument doc;
  doc["remainingPct"]=currentStatus.remainingPct;
  doc["usedKWh"]=currentStatus.usedKWh;
  doc["remKWh"]=currentStatus.remKWh;
  doc["p1"]=currentStatus.p1; doc["p2"]=currentStatus.p2; doc["p3"]=currentStatus.p3; doc["p4"]=currentStatus.p4;
  doc["paused"]=currentStatus.paused;
  doc["budget"]=currentStatus.budget;
  doc["show_usage_graph"]=appcfg.show_usage_graph;
  doc["show_prio_status"]=appcfg.show_prio_status;
  doc["show_prio_controls"]=appcfg.show_prio_controls;
  doc["depleted"] = (currentStatus.remKWh <= 0.0 || currentStatus.remainingPct <= 0.0f);
  doc["powerW"]   = lastPowerW;
  doc["voltageV"] = lastVoltageV;
  doc["currentA"] = lastCurrentA;
  doc["energy_raw_kwh"]     = lastGoodTotal;
  doc["energy_virtual_kwh"] = virtualTotalKWh();

  String out; serializeJson(doc,out);
  req->send(200,"application/json",out);
}

/* ===== CSV end-points ===== */
bool isCsvNameSafe(String p) {
  if (p.length() == 0) return false;
  if (p.indexOf("..") >= 0) return false;
  if (p[0] != '/') p = "/" + p;
  String low = p; low.toLowerCase();
  if (!low.endsWith(".csv")) return false;
  for (size_t i = 0; i < p.length(); ++i) {
    char c = p[i];
    bool ok = (c=='/' || c=='_' || c=='-' || c=='.' ||
               (c>='0'&&c<='9') || (c>='A'&&c<='Z') || (c>='a'&&c<='z'));
    if (!ok) return false;
  }
  return true;
}
void handleCsvList(AsyncWebServerRequest* req) {
  if (!hasAuth(req)) { req->send(401); return; }
  String dir = "/";
  if (req->hasParam("dir")) {
    dir = req->getParam("dir")->value();
    if (dir.length()==0 || dir[0] != '/') dir = "/" + dir;
  }
  if (!SD.begin(SD_CS)) { req->send(500, "application/json", "{\"error\":\"SD not mounted\"}"); return; }
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) { req->send(404, "application/json", "{\"error\":\"dir not found\"}"); return; }

  auto *res = req->beginResponseStream("application/json");
  res->print("["); bool first = true;
  while (true) {
    File f = root.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String name = String(f.name()); String low=name; low.toLowerCase();
      if (low.endsWith(".csv")) {
        if (!first) res->print(",");
        first = false;
        res->print("{\"name\":\""); res->print(name);
        res->print("\",\"size\":");  res->print(f.size()); res->print("}");
      }
    }
    f.close();
  }
  root.close();
  res->print("]");
  req->send(res);
}
void handleCsvGet(AsyncWebServerRequest* req) {
  if (!hasAuth(req)) { req->send(401); return; }
  if (!req->hasParam("name")) { req->send(400, "text/plain", "Missing name"); return; }
  String name = req->getParam("name")->value();
  if (name.length()==0) { req->send(400, "text/plain", "Invalid name"); return; }
  if (name[0] != '/') name = "/" + name;
  if (!isCsvNameSafe(name)) { req->send(400, "text/plain", "Invalid or unsupported filename"); return; }
  if (!SD.begin(SD_CS))      { req->send(500, "text/plain", "SD not mounted"); return; }
  if (!SD.exists(name))      { req->send(404, "text/plain", "Not found"); return; }
  AsyncWebServerResponse *res = req->beginResponse(SD, name, "text/csv");
  if (req->hasParam("download")) {
    String leaf = name.substring(name.lastIndexOf('/') + 1);
    res->addHeader("Content-Disposition", "attachment; filename=\"" + leaf + "\"");
  }
  req->send(res);
}

/* ===== Config HTTP helpers & handlers ===== */
static String urlDecode(const String &s){
  String out; out.reserve(s.length());
  for (size_t i=0;i<s.length();++i){
    char c=s[i];
    if (c=='%'){
      if (i+2<s.length()){
        char h1=s[i+1], h2=s[i+2];
        auto hex=[&](char h)->int{ if(h>='0'&&h<='9') return h-'0'; if(h>='A'&&h<='F') return h-'A'+10; if(h>='a'&&h<='f') return h-'a'+10; return 0; };
        out += char((hex(h1)<<4)|hex(h2)); i+=2;
      }
    } else if (c=='+') out+=' ';
    else out+=c;
  }
  return out;
}
static bool parseBoolStr(String v){ v.toLowerCase(); return (v=="1"||v=="true"||v=="yes"||v=="on"); }

void handleConfigGet(AsyncWebServerRequest* req){
  if(!hasAuth(req)){ req->send(401); return; }
  JsonDocument d;
  d["username"]=appcfg.username;
  d["budget_kwh"]=appcfg.budget_kwh;
  d["show_usage_graph"]=appcfg.show_usage_graph;
  d["show_prio_status"]=appcfg.show_prio_status;
  d["show_prio_controls"]=appcfg.show_prio_controls;
  String out; serializeJson(d,out);
  req->send(200,"application/json",out);
}
void handleConfigBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
  if (!hasAuth(req)) { req->send(401); return; }
  if (index==0) req->_tempObject = new String();
  String* body = (String*)req->_tempObject;
  body->reserve(total);
  body->concat((const char*)data, len);
  if (index + len != total) return;

  String contentType="";
  if (req->hasHeader("Content-Type")) contentType = req->getHeader("Content-Type")->value();
  contentType.toLowerCase();

  String u = appcfg.username;
  String p = appcfg.password;
  float  budget = appcfg.budget_kwh;
  bool   sGraph = appcfg.show_usage_graph;
  bool   sStatus= appcfg.show_prio_status;
  bool   sCtrl  = appcfg.show_prio_controls;

  if (contentType.indexOf("application/json")>=0) {
    JsonDocument d;
    if (deserializeJson(d, *body) == DeserializationError::Ok) {
      if (d["username"].is<const char*>()) u = d["username"].as<const char*>();
      if (d["password"].is<const char*>()) p = d["password"].as<const char*>();
      if (d["budget_kwh"].is<float>())     budget = d["budget_kwh"].as<float>();
      if (d["show_usage_graph"].is<bool>())    sGraph = d["show_usage_graph"].as<bool>();
      if (d["show_prio_status"].is<bool>())    sStatus= d["show_prio_status"].as<bool>();
      if (d["show_prio_controls"].is<bool>())  sCtrl  = d["show_prio_controls"].as<bool>();
    }
  } else if (contentType.indexOf("application/x-www-form-urlencoded")>=0 || contentType.indexOf("text/plain")>=0) {
    String b = *body;
    int start=0;
    while (start < (int)b.length()) {
      int amp = b.indexOf('&', start); if (amp<0) amp = b.length();
      int eq  = b.indexOf('=', start);
      if (eq>start && eq<amp) {
        String key = b.substring(start, eq);
        String val = b.substring(eq+1, amp);
        key.toLowerCase(); val = urlDecode(val);
        if (key=="username") u = val;
        else if (key=="password") p = val;
        else if (key=="budget_kwh") budget = val.toFloat();
        else if (key=="show_usage_graph") sGraph = parseBoolStr(val);
        else if (key=="show_prio_status") sStatus= parseBoolStr(val);
        else if (key=="show_prio_controls") sCtrl = parseBoolStr(val);
      }
      start = amp+1;
    }
  } else {
    JsonDocument d;
    if (deserializeJson(d, *body) == DeserializationError::Ok) {
      if (d["username"].is<const char*>()) u = d["username"].as<const char*>();
      if (d["password"].is<const char*>()) p = d["password"].as<const char*>();
      if (d["budget_kwh"].is<float>())     budget = d["budget_kwh"].as<float>();
      if (d["show_usage_graph"].is<bool>())    sGraph = d["show_usage_graph"].as<bool>();
      if (d["show_prio_status"].is<bool>())    sStatus= d["show_prio_status"].as<bool>();
      if (d["show_prio_controls"].is<bool>())  sCtrl  = d["show_prio_controls"].as<bool>();
    }
  }

  delete (String*)req->_tempObject; req->_tempObject=nullptr;

  if (budget <= 0.0f) { req->send(400, "text/plain", "budget_kwh>0"); return; }

  appcfg.username = u;
  appcfg.password = p;
  appcfg.budget_kwh = budget;
  appcfg.show_usage_graph = sGraph;
  appcfg.show_prio_status = sStatus;
  appcfg.show_prio_controls = sCtrl;

  if(!appcfg.show_prio_controls) manualMask = 0;

  budgetKWh = appcfg.budget_kwh;
  currentStatus.budget = budgetKWh;

  persistConfig();

  JsonDocument out;
  out["ok"] = true;
  out["username"] = appcfg.username;
  out["budget_kwh"] = appcfg.budget_kwh;
  out["show_usage_graph"] = appcfg.show_usage_graph;
  out["show_prio_status"] = appcfg.show_prio_status;
  out["show_prio_controls"] = appcfg.show_prio_controls;
  String s; serializeJson(out, s);
  req->send(200, "application/json", s);
}

/* ===== Login/Logout ===== */
void handleLoginBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
  if (index==0) req->_tempObject = new String();
  String* body = (String*)req->_tempObject;
  body->reserve(total);
  body->concat((const char*)data, len);
  if (index + len != total) return;

  String contentType="";
  if (req->hasHeader("Content-Type")) contentType = req->getHeader("Content-Type")->value();
  contentType.toLowerCase();

  String u, p;

  if (contentType.indexOf("application/json")>=0) {
    JsonDocument d;
    DeserializationError e = deserializeJson(d, *body);
    if (!e) {
      const char* uu = d["username"].is<const char*>() ? d["username"].as<const char*>() : "";
      const char* pp = d["password"].is<const char*>() ? d["password"].as<const char*>() : "";
      u = String(uu);
      p = String(pp);
    }
  } else if (contentType.indexOf("application/x-www-form-urlencoded")>=0) {
    String b = *body;
    int start=0;
    while (start < (int)b.length()) {
      int amp = b.indexOf('&', start); if (amp<0) amp = b.length();
      int eq  = b.indexOf('=', start);
      if (eq>start && eq<amp) {
        String key = b.substring(start, eq);
        String val = b.substring(eq+1, amp);
        key.toLowerCase();
        val = urlDecode(val);
        if (key=="username") u = val;
        else if (key=="password") p = val;
      }
      start = amp+1;
    }
  } else {
    JsonDocument d;
    if (deserializeJson(d, *body) == DeserializationError::Ok) {
      const char* uu = d["username"].is<const char*>() ? d["username"].as<const char*>() : "";
      const char* pp = d["password"].is<const char*>() ? d["password"].as<const char*>() : "";
      u = String(uu);
      p = String(pp);
    }
  }

  delete (String*)req->_tempObject; req->_tempObject=nullptr;

  if (u == appcfg.username && p == appcfg.password){
    isLoggedIn = true; sessionId = randomHex(16);
    AsyncWebServerResponse *res = req->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", "SID="+sessionId+"; Path=/; HttpOnly");
    req->send(res);
  } else {
    req->send(401, "application/json", "{\"ok\":false,\"err\":\"Invalid credentials\"}");
  }
}
void handleLogout(AsyncWebServerRequest* req){
  isLoggedIn=false; sessionId="";
  AsyncWebServerResponse *res = req->beginResponse(200, "application/json", "{\"ok\":true}");
  res->addHeader("Set-Cookie","SID=deleted; Path=/; Max-Age=0");
  req->send(res);
}

/* ===================== Wi-Fi ===================== */
void startWiFi(){
  WiFi.mode(WIFI_STA); WiFi.begin(STA_SSID,STA_PASS);
  Serial.printf("Connecting to %s",STA_SSID);
  int tries=0; while(WiFi.status()!=WL_CONNECTED && tries++<40){ delay(250); Serial.print("."); }
  if(WiFi.status()==WL_CONNECTED){ Serial.printf("\nConnected! IP=%s\n",WiFi.localIP().toString().c_str()); }
  else{ Serial.println("\nWiFi not connected (offline)."); }
}

/* ===================== Setup / Loop ===================== */
uint32_t lastPrint = 0;
uint32_t lastStateSaveMs = 0;

void setup(){
  Serial.begin(115200); delay(100);

  if(!LittleFS.begin(true,"/littlefs",10,"littlefs")){
    Serial.println("[LittleFS] mount/format failed");
  } else {
    Serial.println("[LittleFS] mounted");
  }

  initGroupPins(); allGroups(false);

  startWiFi(); ntpSynced=syncNTP();
  if(!rtc.begin()){ Serial.println("RTC not found"); }
  else { rtcReady=true; if(ntpSynced) rtc.adjust(nowLocal()); }

  SPI.begin(18,19,23,SD_CS);
  if(!SD.begin(SD_CS,SPI,20000000)){ Serial.println("SD init failed"); }
  else { Serial.println("SD mounted"); }

  loadConfig();

  PZEMSerial.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);
  lastEnergyUpdateMs = millis();

  // Default snapshot
  frozenRem = budgetKWh;
  frozenPct = 100.0f;
  frozenUsed = 0.0;

  // Prefer CSV restore; fall back to state.json
  bool restored = false;
  if(loadSnapshotFromCsv()){ restored = true; }
  else if(loadPauseSnapshot()){ restored = true; }

  if(restored){
    paused = true;
    forceLogNext = true; // log what we show
  } else {
    paused = true;
    forceLogNext = true;
  }

  // Routes
  server.on("/",HTTP_GET,[](AsyncWebServerRequest* r){ r->redirect("/dashboard"); });
  server.on("/dashboard",HTTP_GET,[](AsyncWebServerRequest* r){ if(hasAuth(r)) r->send(LittleFS,"/dashboard.html","text/html"); else r->redirect("/login"); });
  server.on("/configuration",HTTP_GET,[](AsyncWebServerRequest* r){ if(hasAuth(r)) r->send(LittleFS,"/configuration.html","text/html"); else r->redirect("/login"); });
  server.on("/login",HTTP_GET,[](AsyncWebServerRequest* r){ r->send(LittleFS,"/login.html","text/html"); });
  server.serveStatic("/",LittleFS,"/");

  // APIs
  server.on("/api/status",HTTP_GET,handleStatus);
  server.on("/api/stop",HTTP_POST,[](AsyncWebServerRequest* req){
    paused = true; manualMask = 0;
    currentStatus.p1=currentStatus.p2=currentStatus.p3=currentStatus.p4=false;
    allGroups(false);
    forceLogNext = true;
    savePauseSnapshot();
    req->send(200);
  });
  server.on("/api/resume",HTTP_POST,[](AsyncWebServerRequest* req){
    manualMask = 0;
    if(firstResume && !baselineFromSnapshot) {
      restartCycle();
    } else {
      firstResume = false;
    }
    paused = false;
    lastStateSaveMs = 0;
    savePauseSnapshot();
    req->send(200);
  });
  server.on("/api/restart",HTTP_POST,[](AsyncWebServerRequest* req){
    manualMask = 0;
    restartCycle();
    paused = false;
    lastStateSaveMs = 0;
    savePauseSnapshot();
    req->send(200);
  });
  server.on("/api/budget",HTTP_POST,[](AsyncWebServerRequest*req){
    if(!req->hasParam("val")){ req->send(400,"text/plain","Missing val"); return; }
    float v=req->getParam("val")->value().toFloat();
    if(v<=0.0f||isnan(v)){ req->send(400,"text/plain","Invalid val"); return; }
    budgetKWh=v; currentStatus.budget=budgetKWh;
    appcfg.budget_kwh=v;
    if(paused){
      double usedFromPct = budgetKWh * (1.0 - (frozenPct/100.0));
      frozenUsed = usedFromPct;
      frozenRem  = max(0.0, budgetKWh - frozenUsed);
      savePauseSnapshot();
    }
    persistConfig();
    req->send(200,"text/plain","OK");
  });
  server.on("/api/relays",HTTP_GET,[](AsyncWebServerRequest* req){
    JsonDocument doc;
    doc["p1"]=currentStatus.p1; doc["p2"]=currentStatus.p2; doc["p3"]=currentStatus.p3; doc["p4"]=currentStatus.p4;
    String out; serializeJson(doc,out);
    req->send(200,"application/json",out);
  });
  server.on("/api/relays/set",HTTP_POST,[](AsyncWebServerRequest* req){
    if(!req->hasParam("prio") || !req->hasParam("on")){ req->send(400,"text/plain","Missing prio/on"); return; }
    int pr = req->getParam("prio")->value().toInt();
    bool on = req->getParam("on")->value()=="1";
    if(!appcfg.show_prio_controls){ req->send(403,"text/plain","Controls disabled"); return; }
    switch(pr){
      case 1: setGroup(prio1,on); currentStatus.p1=on; break;
      case 2: setGroup(prio2,on); currentStatus.p2=on; break;
      case 3: setGroup(prio3,on); currentStatus.p3=on; break;
      case 4: setGroup(prio4,on); currentStatus.p4=on; break;
      default: req->send(400,"text/plain","Invalid prio"); return;
    }
    manualMask |= (1u << (pr-1));
    req->send(200,"text/plain","OK");
  });
  server.on("/api/config",HTTP_GET,handleConfigGet);
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* req){}, NULL, handleConfigBody);
  server.on("/api/login",  HTTP_POST, [](AsyncWebServerRequest* req){}, NULL, handleLoginBody);
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/sd/csvs", HTTP_GET, handleCsvList);
  server.on("/api/sd/csv",  HTTP_GET, handleCsvGet);

  server.begin();

  manualMask = 0;
  forceLogNext = true;
}

void loop() {
  currentStatus = computeStatus();
  enforceRelays(currentStatus);
  appendLogMaybe();

  if (millis() - lastPrint >= 1000) {
    const double virtE = currentStatus.usedKWh + energyBaseline;
    Serial.printf(
      "pct %.2f%% used %.6f rem %.6f | P=%.1fW V=%.1fV I=%.3fA | virtE=%.6f base=%.6f rawE=%.6f\n",
      currentStatus.remainingPct, currentStatus.usedKWh, currentStatus.remKWh,
      lastPowerW, lastVoltageV, lastCurrentA,
      virtE, energyBaseline, lastGoodTotal
    );
    lastPrint = millis();
  }

  if (!paused && millis() - lastStateSaveMs > 60000UL) {
    savePauseSnapshot();
    lastStateSaveMs = millis();
  }

  delay(200);
}
