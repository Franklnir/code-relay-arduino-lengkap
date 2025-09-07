/*
 * ESP8266 + Relay 2CH — AP Only WebServer (Relay 1 pakai slide toggle)
 * - SoftAP saja, SSID/PASS di bawah
 * - IP AP: 192.168.4.1 (softAPConfig)
 * - Captive Portal (DNSServer) redirect semua domain ke portal
 * - UI realtime via /api/status (1 detik)
 * - EEPROM persist (status ON/OFF tersimpan & dipakai saat boot)
 * - Relay ACTIVE LOW (LOW=ON, HIGH=OFF)
 *
 * Board target: NodeMCU / Wemos D1 mini (ESP8266)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

#define FW_VERSION "ap-only v1.1 (R1 slider)"

// ================== SoftAP Config ==================
const char* AP_SSID = "ESP8266-Relay-2CH";
const char* AP_PASS = "12345678";   // minimal 8 karakter; kosong = open
const uint8_t AP_CHANNEL = 6;
const bool AP_HIDDEN = false;
const uint8_t AP_MAX_CONN = 4;

// IP AP default 192.168.4.1
IPAddress apIP(192, 168, 4, 1);
IPAddress apGW(192, 168, 4, 1);
IPAddress apMASK(255, 255, 255, 0);

// ================== Relay Pin (ACTIVE LOW) =========
const uint8_t RELAY1_PIN = D5; // GPIO14
const uint8_t RELAY2_PIN = D6; // GPIO12

// ================== EEPROM Layout ==================
struct EepromLayout {
  uint32_t magic;     // 0xA5A5BEEF
  uint16_t version;   // 0x0001
  bool relay1_on;     // true=ON
  bool relay2_on;     // true=ON
};

EepromLayout g_cfg;
const uint32_t EEPROM_MAGIC   = 0xA5A5BEEF;
const uint16_t EEPROM_VERSION = 0x0001;
const size_t   EEPROM_SIZE    = 64;

// ================== Servers ========================
ESP8266WebServer server(80);
DNSServer dns;                 // Captive portal

// ================== Helpers ========================
inline void applyRelayState() {
  digitalWrite(RELAY1_PIN, g_cfg.relay1_on ? LOW : HIGH);
  digitalWrite(RELAY2_PIN, g_cfg.relay2_on ? LOW : HIGH);
}

bool saveConfig() {
  EEPROM.put(0, g_cfg);
  return EEPROM.commit();
}

void loadOrInitConfig() {
  EEPROM.get(0, g_cfg);
  if (g_cfg.magic != EEPROM_MAGIC || g_cfg.version != EEPROM_VERSION) {
    g_cfg.magic     = EEPROM_MAGIC;
    g_cfg.version   = EEPROM_VERSION;
    g_cfg.relay1_on = false;
    g_cfg.relay2_on = false;
    saveConfig();
  }
}

unsigned long bootMillis = 0;

// ================== UI (PROGMEM) ===================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>ESP8266 Relay 2CH (AP)</title>
<style>
  :root { --bg:#0f172a; --card:#111827; --muted:#94a3b8; --on:#22c55e; --off:#ef4444; --btn2:#f97316; --border:#1f2937;}
  *{box-sizing:border-box} body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,"Helvetica Neue",Arial}
  body{background:var(--bg);color:#e5e7eb}
  .wrap{max-width:920px;margin:28px auto;padding:16px}
  .card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:20px;box-shadow:0 8px 30px rgba(0,0,0,.25)}
  h1{margin:0 0 8px;font-size:22px}
  .muted{color:var(--muted);font-size:13px;margin-bottom:16px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px}
  .relay{padding:16px;border:1px solid var(--border);border-radius:14px}
  .relay h3{margin:0 0 6px}
  .state{display:inline-block;padding:4px 10px;border-radius:999px;font-size:12px;border:1px solid var(--border)}
  .state.on{background:rgba(34,197,94,.1);color:#86efac;border-color:#14532d}
  .state.off{background:rgba(239,68,68,.08);color:#fca5a5;border-color:#7f1d1d}
  .btns{display:flex;gap:10px;margin-top:10px;flex-wrap:wrap}
  button{appearance:none;border:none;cursor:pointer;border-radius:12px;padding:10px 14px;font-weight:600}
  .b-on{background:var(--on);color:#03220e}
  .b-off{background:var(--off);color:#2a0606}
  .b-toggle{background:var(--btn2)}

  /* ===== toggle switch untuk Relay 1 ===== */
  .switch {position:relative;display:inline-block;width:64px;height:34px;vertical-align:middle; margin-left:10px;}
  .switch input {opacity:0;width:0;height:0}
  .slider {position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#374151;border:1px solid var(--border);transition:.2s;border-radius:999px}
  .slider:before {position:absolute;content:"";height:26px;width:26px;left:4px;top:3px;background:white;border-radius:50%;transition:.2s}
  input:checked + .slider {background:rgba(34,197,94,.25);border-color:#14532d}
  input:checked + .slider:before {transform:translateX(30px)}

  .row{display:flex;gap:16px;flex-wrap:wrap;margin-top:18px}
  .pill{padding:8px 12px;border-radius:999px;border:1px solid var(--border);font-size:12px}
  .ok{border-color:#14532d;background:rgba(16,185,129,.08);color:#86efac}
  .warn{border-color:#78350f;background:rgba(245,158,11,.08);color:#fbbf24}
  code{background:#0b1220;border:1px solid var(--border);padding:2px 6px;border-radius:8px}
  footer{margin-top:16px;color:var(--muted);font-size:12px;text-align:right}
</style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>ESP8266 Relay 2CH (AP) <span id="fw" class="muted"></span></h1>
      <div class="muted">AP SSID: <code id="ssid">-</code> • IP: <code id="ip">-</code> • Uptime: <span id="uptime">-</span>s</div>

      <div class="grid">
        <!-- ===== Relay 1: pakai slide toggle ===== -->
        <div class="relay" id="r1">
          <h3 style="display:flex;align-items:center;justify-content:space-between;">
            <span>Relay 1 <span class="state" id="s1">-</span></span>
            <label class="switch" title="Geser untuk ON/OFF">
              <input type="checkbox" id="sw1">
              <span class="slider"></span>
            </label>
          </h3>
          <div class="muted">Geser switch untuk menyalakan/mematikan. Tersimpan ke EEPROM.</div>
        </div>

        <!-- ===== Relay 2: tetap tombol ===== -->
        <div class="relay" id="r2">
          <h3>Relay 2 <span class="state" id="s2">-</span></h3>
          <div class="btns">
            <button class="b-on"    onclick="setRelay(2,true)">ON</button>
            <button class="b-off"   onclick="setRelay(2,false)">OFF</button>
            <button class="b-toggle"onclick="toggleRelay(2)">TOGGLE</button>
          </div>
        </div>
      </div>

      <div class="row">
        <div class="pill ok">EEPROM R1: <b id="e1">-</b></div>
        <div class="pill ok">EEPROM R2: <b id="e2">-</b></div>
        <div class="pill warn">Relay Mode: <b>ACTIVE LOW</b></div>
      </div>

      <footer>Realtime by <code>/api/status</code> • EEPROM persisted • %FW%</footer>
    </div>
  </div>

<script>
const el = (id)=>document.getElementById(id);

function applyState(r, on){
  const s = el("s"+r);
  s.textContent = on ? "ON" : "OFF";
  s.className = "state " + (on ? "on":"off");
}

function paint(data){
  el("fw").textContent = "(" + data.fw + ")";
  el("ssid").textContent = data.ssid;
  el("ip").textContent = data.ip;
  el("uptime").textContent = data.uptime_sec;

  // sinkron UI dengan status server/EEPROM
  applyState(1, data.relay1);
  applyState(2, data.relay2);
  el("e1").textContent = data.eeprom.relay1 ? "ON":"OFF";
  el("e2").textContent = data.eeprom.relay2 ? "ON":"OFF";

  // set posisi awal switch Relay 1 (tanpa memicu event)
  const sw1 = el("sw1");
  if (sw1) {
    const want = !!data.relay1;
    if (sw1.checked !== want) {
      sw1.checked = want;
    }
  }
}

async function fetchStatus(){
  try{
    const r = await fetch("/api/status");
    const j = await r.json();
    paint(j);
  }catch(e){ /* ignore */ }
}

async function setRelay(ch, on){
  try{
    const r = await fetch(`/api/set?ch=${ch}&state=${on?"on":"off"}`);
    const j = await r.json();
    paint(j);
  }catch(e){}
}

async function toggleRelay(ch){
  try{
    const r = await fetch(`/api/toggle?ch=${ch}`);
    const j = await r.json();
    paint(j);
  }catch(e){}
}

// === event listener untuk slide toggle Relay 1 ===
(function(){
  const sw1 = el("sw1");
  if (!sw1) return;
  let busy = false; // simple debounce agar tidak spam request
  sw1.addEventListener("change", async (ev)=>{
    if (busy) return;
    busy = true;
    const wantOn = sw1.checked;
    try{
      await setRelay(1, wantOn);
    } finally {
      busy = false;
    }
  });
})();

setInterval(fetchStatus, 1000);
fetchStatus();
</script>
</body>
</html>
)HTML";

void sendJsonStatus() {
  String json = "{";
  json += "\"fw\":\"" + String(FW_VERSION) + "\",";
  json += "\"ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"uptime_sec\":" + String((millis() - bootMillis)/1000) + ",";
  json += "\"relay1\":" + String(g_cfg.relay1_on ? "true":"false") + ",";
  json += "\"relay2\":" + String(g_cfg.relay2_on ? "true":"false") + ",";
  json += "\"eeprom\":{\"relay1\":" + String(g_cfg.relay1_on ? "true":"false") + ",\"relay2\":" + String(g_cfg.relay2_on ? "true":"false") + "}";
  json += "}";
  server.send(200, "application/json", json);
}

void handleIndex() {
  String html = FPSTR(INDEX_HTML);
  html.replace("%FW%", FW_VERSION);
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSet() {
  if (!server.hasArg("ch") || !server.hasArg("state")) { server.send(400, "text/plain", "Bad Request"); return; }
  int ch = server.arg("ch").toInt();
  String st = server.arg("state");
  bool on = (st == "on" || st == "ON" || st == "1" || st == "true");

  if (ch == 1) g_cfg.relay1_on = on;
  else if (ch == 2) g_cfg.relay2_on = on;
  else { server.send(400, "text/plain", "Invalid channel"); return; }

  applyRelayState();
  if (!saveConfig()) { server.send(500, "application/json", "{\"error\":\"EEPROM commit failed\"}"); return; }
  sendJsonStatus();
}

void handleToggle() {
  if (!server.hasArg("ch")) { server.send(400, "text/plain", "Bad Request"); return; }
  int ch = server.arg("ch").toInt();

  if (ch == 1) g_cfg.relay1_on = !g_cfg.relay1_on;
  else if (ch == 2) g_cfg.relay2_on = !g_cfg.relay2_on;
  else { server.send(400, "text/plain", "Invalid channel"); return; }

  applyRelayState();
  if (!saveConfig()) { server.send(500, "application/json", "{\"error\":\"EEPROM commit failed\"}"); return; }
  sendJsonStatus();
}

void handleNotFound() {
  // Captive: selalu redirect ke /
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// ================== Setup & Loop ===================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== ESP8266 Relay 2CH — AP Only Mode (R1 slider) ==="));

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);

  EEPROM.begin(EEPROM_SIZE);
  loadOrInitConfig();
  applyRelayState();

  // SoftAP IP
  if (!WiFi.softAPConfig(apIP, apGW, apMASK)) {
    Serial.println(F("softAPConfig gagal, gunakan default 192.168.4.1"));
  }

  // Start AP
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONN);
  if (ok) {
    Serial.printf("AP STARTED: SSID=%s  PASS=%s  CH=%d  hidden=%d  max=%d\n",
                  AP_SSID, AP_PASS, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONN);
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("Gagal start AP! (cek SSID/PASS)");
  }

  // DNS Captive Portal: arahkan semua domain ke 192.168.4.1
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIP);

  // Routes
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, [](){ sendJsonStatus(); });
  server.on("/api/set", HTTP_GET, handleSet);
  server.on("/api/toggle", HTTP_GET, handleToggle);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("WebServer port 80 siap. Buka http://192.168.4.1/");
  Serial.println("==================================================");

  bootMillis = millis();
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}
