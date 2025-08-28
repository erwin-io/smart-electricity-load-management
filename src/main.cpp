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
#include <vector>
#include <algorithm>
#include <DNSServer.h>

DNSServer dnsServer;

/* === AP defaults === */
#define AP_SSID "LoadDroppingWifi"
#define AP_PASSWORD "loaddropping2025"

/* ===================== PZEM wiring (UART2) ===================== */
static const int PZEM_RX = 26; // ESP32 RX2 <- PZEM TX
static const int PZEM_TX = 25; // ESP32 TX2 -> PZEM RX
HardwareSerial PZEMSerial(2);
PZEM004Tv30 pzem(PZEMSerial, PZEM_RX, PZEM_TX);

/* ===================== SD card (Mini Data Logger) ===================== */
#define SD_CS 5
RTC_DS3231 rtc;
static volatile bool sdMounted = false;

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

/* ===================== FS mount flags ===================== */
static bool littlefsMounted = false;

/* ===================== readiness / tasks ===================== */
volatile bool systemReady = false;  // flips true when slow init completes
void slowInitTask(void*);

/* ===================== Forwards ===================== */
double  virtualTotalKWh();
void    savePauseSnapshot();
bool    loadPauseSnapshot();
static  String urlDecode(const String &s);   // forward declare
static  bool   parseBoolStr(String v);

/* ===================== FS helpers ===================== */
bool fileExists(fs::FS &fs, const char* path){
  File f = fs.open(path); if(!f) return false; f.close(); return true;
}

/* ====== Robust SD mount (retries) ====== */
static void sd_mount_with_retries(uint8_t retries=5, uint32_t firstDelayMs=150, uint32_t betweenMs=200){
  if (sdMounted) return;
  SPI.end();
  SPI.begin(18,19,23,SD_CS);
  delay(firstDelayMs);
  for (uint8_t i=0;i<retries && !sdMounted;i++){
    if (SD.begin(SD_CS, SPI, 20000000)){
      sdMounted = true;
      Serial.println("SD mounted");
      break;
    }
    Serial.printf("SD mount try %u failed\n", (unsigned)i+1);
    delay(betweenMs);
  }
  if(!sdMounted) Serial.println("SD init failed");
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

// Quick, non-blocking NTP (<= 1s)
bool syncNTP_quick(){
  configTime(8*3600, 0, "pool.ntp.org", "time.nist.gov"); // Asia/Manila
  for(int i=0;i<5;i++){ struct tm t; if(getLocalTime(&t)) return true; delay(200); }
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
  if(!littlefsMounted) return;
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
  if(!littlefsMounted) return false;
  if(!LittleFS.exists("/state.json"))    return false;
  File f = LittleFS.open("/state.json","r");
  if(!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d,f);
  f.close();
  if(e) return false;

  energyBaseline = d["baseline_kwh"] | 0.0;
  baselineFromSnapshot = d["baseline_kwh"].is<double>();  // ArduinoJson v7 style

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
      String nm = String(f.name());
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
  int c1 = lastLine.indexOf(','); if(c1<0) return false;
  int c2 = lastLine.indexOf(',', c1+1); if(c2<0) return false;
  int c3 = lastLine.indexOf(',', c2+1); if(c3<0) return false;

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
  double vt = virtualTotalKWh();
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
  doc["ready"] = systemReady;

  String out; serializeJson(doc,out);
  req->send(200,"application/json",out);
}

/* ===== Config HTTP handlers (REST) ===== */
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
    // yield to avoid watchdog on huge dirs
    static uint32_t lastY = millis();
    if (millis() - lastY > 10) { delay(0); lastY = millis(); }
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

/* ===================== LOGS (merge+filter across logs_*.csv) ===================== */
struct LogRow {
  String ts;  // "YYYY-MM-DD HH:MM:SS"
  double budget, rem, used;
};
static time_t parseTimestampLocal(const String& s){
  if (s.length() < 19) return 0;
  int y = s.substring(0,4).toInt();
  int m = s.substring(5,7).toInt();
  int d = s.substring(8,10).toInt();
  int hh= s.substring(11,13).toInt();
  int mm= s.substring(14,16).toInt();
  int ss= s.substring(17,19).toInt();
  DateTime dt(y,m,d,hh,mm,ss);
  return dt.unixtime(); // treat as local
}
static bool isLogsCsv(const String& name){
  String low = name; low.toLowerCase();
  if (low.length() && low[0]=='/') low.remove(0,1);
  return low.startsWith("logs_") && low.endsWith(".csv");
}
static void collectLogFiles(std::vector<String>& out){
  if (!SD.begin(SD_CS)) return;
  File root = SD.open("/");
  if (!root || !root.isDirectory()) return;
  while(true){
    File f = root.openNextFile();
    if (!f) break;
    if (!f.isDirectory()){
      String nm = String(f.name());
      if (isLogsCsv(nm)){
        if (nm[0] != '/') nm = "/" + nm;
        out.push_back(nm);
      }
    }
    f.close();
    static uint32_t lastY = millis();
    if (millis() - lastY > 10) { delay(0); lastY = millis(); }
  }
  root.close();
  std::sort(out.begin(), out.end()); // filename order is chronological by hour
}
static bool parseCsvLine(const String& line, LogRow& row){
  int c1 = line.indexOf(','); if (c1<0) return false;
  int c2 = line.indexOf(',', c1+1); if (c2<0) return false;
  int c3 = line.indexOf(',', c2+1); if (c3<0) return false;
  row.ts    = line.substring(0, c1);
  row.budget= line.substring(c1+1, c2).toDouble();
  row.rem   = line.substring(c2+1, c3).toDouble();
  row.used  = line.substring(c3+1).toDouble();
  return true;
}
static void handleLogsQuery(AsyncWebServerRequest* req){
  if (!hasAuth(req)) { req->send(401); return; }

  time_t tFrom = 0, tTo = 0;
  if (req->hasParam("from")) { String f = req->getParam("from")->value(); f.replace('T',' '); tFrom = parseTimestampLocal(f); }
  if (req->hasParam("to"))   { String t = req->getParam("to")->value();   t.replace('T',' '); tTo   = parseTimestampLocal(t); }

  std::vector<String> files; files.reserve(64);
  collectLogFiles(files);

  auto *res = req->beginResponseStream("application/json");
  res->print("{\"items\":[");
  bool firstOut = true;
  uint32_t lastY = millis();
  size_t rowCount = 0;

  for (size_t i=0;i<files.size();++i){
    File f = SD.open(files[i], "r");
    if (!f) continue;
    while (f.available()){
      String line = f.readStringUntil('\n'); line.trim();
      if (line.length()==0 || line.startsWith("timestamp")) continue;
      LogRow r; if (!parseCsvLine(line, r)) continue;
      time_t tRow = parseTimestampLocal(r.ts);
      if (tFrom && tRow < tFrom) continue;
      if (tTo   && tRow > tTo)   continue;

      if (!firstOut) res->print(",");
      firstOut = false;
      res->print("{\"timestamp\":\""); res->print(r.ts);
      res->print("\",\"budget_kwh\":"); res->print(r.budget, 6);
      res->print(",\"remaining_kwh\":"); res->print(r.rem, 6);
      res->print(",\"used_kwh\":"); res->print(r.used, 6);
      res->print("}");

      // cooperative yield every ~200 rows or 10ms
      if (++rowCount % 200 == 0 || (millis() - lastY > 10)) { delay(0); lastY = millis(); }
    }
    f.close();
    if (millis() - lastY > 10) { delay(0); lastY = millis(); }
  }

  res->print("]}");
  req->send(res);
}
static void handleLogsExport(AsyncWebServerRequest* req){
  if (!hasAuth(req)) { req->send(401); return; }

  time_t tFrom = 0, tTo = 0;
  if (req->hasParam("from")) { String f = req->getParam("from")->value(); f.replace('T',' '); tFrom = parseTimestampLocal(f); }
  if (req->hasParam("to"))   { String t = req->getParam("to")->value();   t.replace('T',' '); tTo   = parseTimestampLocal(t); }

  std::vector<String> files; files.reserve(64);
  collectLogFiles(files);

  auto *res = req->beginResponseStream("text/csv");
  res->addHeader("Content-Disposition", "attachment; filename=\"smartload_logs.csv\"");
  res->print("timestamp,budget_kwh,remaining_kwh,used_kwh\n");

  uint32_t lastY = millis();
  size_t rowCount = 0;

  for (size_t i=0;i<files.size();++i){
    File f = SD.open(files[i], "r");
    if (!f) continue;
    while (f.available()){
      String line = f.readStringUntil('\n'); line.trim();
      if (line.length()==0 || line.startsWith("timestamp")) continue;
      LogRow r; if (!parseCsvLine(line, r)) continue;
      time_t tRow = parseTimestampLocal(r.ts);
      if (tFrom && tRow < tFrom) continue;
      if (tTo   && tRow > tTo)   continue;

      res->print(r.ts); res->print(",");
      res->print(String(r.budget, 6)); res->print(",");
      res->print(String(r.rem, 6));    res->print(",");
      res->print(String(r.used, 6));   res->print("\n");

      if (++rowCount % 200 == 0 || (millis() - lastY > 10)) { delay(0); lastY = millis(); }
    }
    f.close();
    if (millis() - lastY > 10) { delay(0); lastY = millis(); }
  }
  req->send(res);
}

/* ===== NEW: Excel .xls export (SpreadsheetML 2003) ===== */
static void handleLogsExportXls(AsyncWebServerRequest* req){
  if (!hasAuth(req)) { req->send(401); return; }

  time_t tFrom = 0, tTo = 0;
  if (req->hasParam("from")) { String f = req->getParam("from")->value(); f.replace('T',' '); tFrom = parseTimestampLocal(f); }
  if (req->hasParam("to"))   { String t = req->getParam("to")->value();   t.replace('T',' '); tTo   = parseTimestampLocal(t); }

  std::vector<String> files; files.reserve(64);
  collectLogFiles(files);

  auto *res = req->beginResponseStream("application/vnd.ms-excel");
  res->addHeader("Content-Disposition","attachment; filename=\"smartload_logs.xls\"");

  res->print("<?xml version=\"1.0\"?>\n");
  res->print("<?mso-application progid=\"Excel.Sheet\"?>\n");
  res->print("<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" "
             "xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
             "xmlns:x=\"urn:schemas-microsoft-com:office:excel\" "
             "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\" "
             "xmlns:html=\"http://www.w3.org/TR/REC-html40\">\n");
  res->print("<Worksheet ss:Name=\"Logs\"><Table>\n");
  res->print("<Row>"
               "<Cell><Data ss:Type=\"String\">timestamp</Data></Cell>"
               "<Cell><Data ss:Type=\"String\">budget_kwh</Data></Cell>"
               "<Cell><Data ss:Type=\"String\">remaining_kwh</Data></Cell>"
               "<Cell><Data ss:Type=\"String\">used_kwh</Data></Cell>"
             "</Row>\n");

  uint32_t lastY = millis();
  size_t rowCount = 0;

  for (size_t i=0;i<files.size();++i){
    File f = SD.open(files[i], "r");
    if (!f) continue;
    while (f.available()){
      String line = f.readStringUntil('\n'); line.trim();
      if (line.length()==0 || line.startsWith("timestamp")) continue;

      LogRow r; if (!parseCsvLine(line, r)) continue;
      time_t tRow = parseTimestampLocal(r.ts);
      if (tFrom && tRow < tFrom) continue;
      if (tTo   && tRow > tTo)   continue;

      res->print("<Row>");
      res->print("<Cell><Data ss:Type=\"String\">"); res->print(r.ts);        res->print("</Data></Cell>");
      res->print("<Cell><Data ss:Type=\"Number\">"); res->print(r.budget, 6); res->print("</Data></Cell>");
      res->print("<Cell><Data ss:Type=\"Number\">"); res->print(r.rem, 6);    res->print("</Data></Cell>");
      res->print("<Cell><Data ss:Type=\"Number\">"); res->print(r.used, 6);   res->print("</Data></Cell>");
      res->print("</Row>\n");

      if (++rowCount % 200 == 0 || (millis() - lastY > 10)) { delay(0); lastY = millis(); }
    }
    f.close();
    if (millis() - lastY > 10) { delay(0); lastY = millis(); }
  }

  res->print("</Table></Worksheet></Workbook>\n");
  req->send(res);
}

/* ===== NEW: Print (printer-friendly HTML) ===== */
static void handleLogsPrint(AsyncWebServerRequest* req){
  if (!hasAuth(req)) { req->send(401); return; }

  time_t tFrom = 0, tTo = 0;
  if (req->hasParam("from")) { String f = req->getParam("from")->value(); f.replace('T',' '); tFrom = parseTimestampLocal(f); }
  if (req->hasParam("to"))   { String t = req->getParam("to")->value();   t.replace('T',' '); tTo   = parseTimestampLocal(t); }

  std::vector<String> files; files.reserve(64);
  collectLogFiles(files);

  auto *res = req->beginResponseStream("text/html; charset=utf-8");
  res->print("<!doctype html><html><head><meta charset='utf-8'>"
             "<title>SmartLoad Logs</title>"
             "<style>body{font:14px Arial;margin:24px;} table{border-collapse:collapse;width:100%;}"
             "th,td{border:1px solid #ccc;padding:6px 8px;text-align:left;} th{background:#f5f5f5;}"
             "@media print{body{margin:0;} thead{display:table-header-group;}}</style>"
             "</head><body onload='window.print()'>"
             "<h2>SmartLoad Logs</h2>"
             "<table><thead><tr>"
             "<th>timestamp</th><th>budget_kwh</th><th>remaining_kwh</th><th>used_kwh</th>"
             "</tr></thead><tbody>");

  uint32_t lastY = millis();
  size_t rowCount = 0;

  for (size_t i=0;i<files.size();++i){
    File f = SD.open(files[i], "r");
    if (!f) continue;
    while (f.available()){
      String line = f.readStringUntil('\n'); line.trim();
      if (line.length()==0 || line.startsWith("timestamp")) continue;
      LogRow r; if (!parseCsvLine(line, r)) continue;
      time_t tRow = parseTimestampLocal(r.ts);
      if (tFrom && tRow < tFrom) continue;
      if (tTo   && tRow > tTo)   continue;

      res->print("<tr>");
      res->print("<td>"); res->print(r.ts);        res->print("</td>");
      res->print("<td>"); res->print(r.budget, 6); res->print("</td>");
      res->print("<td>"); res->print(r.rem, 6);    res->print("</td>");
      res->print("<td>"); res->print(r.used, 6);   res->print("</td>");
      res->print("</tr>");

      if (++rowCount % 200 == 0 || (millis() - lastY > 10)) { delay(0); lastY = millis(); }
    }
    f.close();
    if (millis() - lastY > 10) { delay(0); lastY = millis(); }
  }

  res->print("</tbody></table></body></html>");
  req->send(res);
}

/* ===================== URL decode helpers ===================== */
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

/* ===================== Wi-Fi ===================== */
void startWiFi(){
  // Force AP-only mode (no STA at all)
  WiFi.mode(WIFI_AP);
  delay(50);

  // Optional: set a friendly AP subnet and static IP (default 192.168.4.1)
  IPAddress apIP(192,168,4,1);
  IPAddress apGW(192,168,4,1);
  IPAddress apMASK(255,255,255,0);
  WiFi.softAPConfig(apIP, apGW, apMASK);

  // Start AP
  const int channel = 6;
  const bool hidden = false;
  const int max_conn = 8;
  WiFi.softAP(AP_SSID, AP_PASSWORD, channel, hidden, max_conn);

  // Captive-portal DNS (everything -> AP IP)
  dnsServer.start(53, "*", apIP);

  Serial.println("AP started.");
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());
}

/* ===================== SLOW INIT TASK ===================== */
void slowInitTask(void*){
  // SD (robust)
  sd_mount_with_retries();

  // NTP / RTC (quick)
  ntpSynced = syncNTP_quick();
  if(!rtc.begin()){
    Serial.println("RTC not found");
  } else {
    rtcReady = true;
    if(ntpSynced) rtc.adjust(nowLocal());
  }

  // Config + PZEM + snapshot restore
  loadConfig();

  PZEMSerial.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);
  lastEnergyUpdateMs = millis();

  // Default snapshot
  frozenRem = budgetKWh;
  frozenPct = 100.0f;
  frozenUsed = 0.0;

  bool restored = false;
  if(loadSnapshotFromCsv()){ restored = true; }
  else if(loadPauseSnapshot()){ restored = true; }

  paused = true;
  forceLogNext = true;

  systemReady = true;   // we’re good
  Serial.println("[INIT] Background init complete.");
  vTaskDelete(NULL);
}

/* ===================== Setup / Loop ===================== */
uint32_t lastPrint = 0;
uint32_t lastStateSaveMs = 0;

void setup(){
  Serial.begin(115200); delay(100);

  // Mount LittleFS (fast)
  if(LittleFS.begin(true, "/littlefs", 10, "littlefs")){
    littlefsMounted = true;
    Serial.println("[LittleFS] mounted");
  } else {
    littlefsMounted = false;
    Serial.println("[LittleFS] mount/format failed");
  }

  initGroupPins(); allGroups(false);

  // --- Wi-Fi & server ASAP ---
  startWiFi();

  // Minimal always-on routes (respond immediately)
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "application/json", systemReady ? "{\"ok\":true,\"ready\":true}" : "{\"ok\":true,\"ready\":false}");
  });
  server.on("/login",HTTP_GET,[](AsyncWebServerRequest* r){
    if(LittleFS.exists("/login.html")) r->send(LittleFS,"/login.html","text/html");
    else r->send(200,"text/html","<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><h3>SmartLoad</h3><p>Booting…</p><p><a href=\"/ping\">Check readiness</a></p>");
  });

  // Full routes
  server.on("/",HTTP_GET,[](AsyncWebServerRequest* r){ r->redirect("/dashboard"); });
  server.on("/dashboard",HTTP_GET,[](AsyncWebServerRequest* r){ if(hasAuth(r)) r->send(LittleFS,"/dashboard.html","text/html"); else r->redirect("/login"); });
  server.on("/configuration",HTTP_GET,[](AsyncWebServerRequest* r){ if(hasAuth(r)) r->send(LittleFS,"/configuration.html","text/html"); else r->redirect("/login"); });
  server.on("/logs",HTTP_GET,[](AsyncWebServerRequest* r){ if(hasAuth(r)) r->send(LittleFS,"/logs.html","text/html"); else r->redirect("/login"); });

  // Static after specific
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

  // Logs APIs
  server.on("/api/logs/query",      HTTP_GET, handleLogsQuery);
  server.on("/api/logs/export",     HTTP_GET, handleLogsExport);     // CSV
  server.on("/api/logs/export.xls", HTTP_GET, handleLogsExportXls);  // Excel
  server.on("/api/logs/print",      HTTP_GET, handleLogsPrint);      // Print

  // Always send *something* quickly
  server.onNotFound([](AsyncWebServerRequest* r){
    r->send(302, "text/plain", ""); r->redirect("/login");
  });

  server.begin();
  Serial.println("HTTP server started.");

  // --- Kick off the slow stuff in background (core 1 is usually WiFi/Net) ---
  xTaskCreatePinnedToCore(slowInitTask, "slowInit", 8192, nullptr, 1, nullptr, 1);

  manualMask = 0;
  forceLogNext = true;
}

void loop() {
  dnsServer.processNextRequest();   // keep captive-portal DNS responsive

  currentStatus = computeStatus();
  enforceRelays(currentStatus);
  appendLogMaybe();

  if (millis() - lastPrint >= 1000) {
    const double virtE = currentStatus.usedKWh + energyBaseline;
    Serial.printf(
      "pct %.2f%% used %.6f rem %.6f | P=%.1fW V=%.1fV I=%.3fA | virtE=%.6f base=%.6f rawE=%.6f | ready=%d\n",
      currentStatus.remainingPct, currentStatus.usedKWh, currentStatus.remKWh,
      lastPowerW, lastVoltageV, lastCurrentA,
      virtE, energyBaseline, lastGoodTotal, (int)systemReady
    );
    lastPrint = millis();
  }

  if (!paused && millis() - lastStateSaveMs > 60000UL) {
    savePauseSnapshot();
    lastStateSaveMs = millis();
  }

  delay(50);  // snappier than 200ms so DNS + web stay responsive
}
