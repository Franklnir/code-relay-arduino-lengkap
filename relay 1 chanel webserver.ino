/*
 * ESP8266 (ESP-01) + Relay 1CH WebServer
 * - Multi-SSID (4 jaringan) dengan IP statik (STA)
 * - Fallback AP otomatis bila STA gagal
 * - Halaman web: tombol ON/OFF + status real-time
 * - Endpoint: /, /on, /off, /toggle, /status (JSON)
 * - Simpan status terakhir relay ke EEPROM
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// ======================= Konfigurasi Multi WiFi (STA) =======================
const char* WIFI_SSIDS[] = {
  "apa ini kok",   // SSID 1
  "Xiaomi 12",     // SSID 2
  "GEORGIA",       // SSID 3
  "Universitas Pelita Bangsa New" // SSID 4
};

const char* WIFI_PASSWORDS[] = {
  "12345678",      // Pass 1
  "123456788",     // Pass 2
  "Georgia12345",  // Pass 3
  "megah123"       // Pass 4
};

const int WIFI_COUNT = sizeof(WIFI_SSIDS) / sizeof(WIFI_SSIDS[0]);

// ---------- IP Statik (sesuaikan dgn jaringan router) ----------
IPAddress local_IP(192, 168, 1, 77);
IPAddress gateway (192, 168, 1, 1);
IPAddress subnet  (255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(1, 1, 1, 1);

// ============== Konfigurasi Access Point (fallback) ===============
const char* AP_SSID = "Newera";
const char* AP_PASS = "1234567888";   // min 8 char
IPAddress apIP   (192, 168, 4, 1);
IPAddress apGW   (192, 168, 4, 1);
IPAddress apMASK (255, 255, 255, 0);

// ====================== Konfigurasi Relay =======================
#define RELAY_PIN         2       // GPIO2 (ESP-01)
#define RELAY_ACTIVE_LOW  true    // LOW = ON

// ====================== EEPROM untuk simpan status ==============
#define EEPROM_SIZE 1
#define EEPROM_ADDR 0

void saveRelayState(bool state) {
  EEPROM.write(EEPROM_ADDR, state ? 1 : 0);
  EEPROM.commit();
}
bool loadRelayState() {
  byte val = EEPROM.read(EEPROM_ADDR);
  return (val == 1);
}

// ========================= Web Server ===========================
ESP8266WebServer server(80);
bool relayOn = false;

// Terapkan output relay
void applyRelayOutput() {
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, relayOn ? LOW : HIGH);
  else                  digitalWrite(RELAY_PIN, relayOn ? HIGH : LOW);
}

// ====================== Halaman HTML =====================
String htmlPage() {
  String state    = relayOn ? "ON" : "OFF";
  String btnText  = relayOn ? "Matikan" : "Nyalakan";
  String btnClass = relayOn ? "off" : "on";

  String page = F(
"<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>Relay ESP-01</title>"
"<style>"
"body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,'Helvetica Neue',Arial,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;display:flex;min-height:100vh;align-items:center;justify-content:center}"
".card{background:#111827;border:1px solid #1f2937;border-radius:16px;padding:24px;box-shadow:0 10px 30px rgba(0,0,0,.35);width:min(420px,90vw)}"
".title{font-size:20px;margin:0 0 6px}.ip{opacity:.8;font-size:13px;margin-bottom:16px}"
".state{font-size:48px;font-weight:700;letter-spacing:2px;margin:16px 0}"
".row{display:flex;gap:12px;margin-top:8px}"
"button{flex:1;border:none;border-radius:12px;padding:14px 18px;font-size:16px;cursor:pointer}"
"button.on{background:#10b981;color:#052e2b}button.on:hover{filter:brightness(1.07)}"
"button.off{background:#ef4444;color:#2f0b0b}button.off:hover{filter:brightness(1.07)}"
".pill{display:inline-block;padding:6px 10px;border-radius:999px;font-size:12px;background:#1f2937}"
".ok{background:#064e3b}.err{background:#7f1d1d}"
"footer{margin-top:14px;opacity:.7;font-size:12px;text-align:center}"
"</style></head><body><div class='card'>"
"<h1 class='title'>Kontrol Relay</h1>"
"<div class='ip'>IP: ");

  page += (WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  page += F(
"</div>"
"<div id='state' class='state'>");

  page += state;

  page += F(
"</div>"
"<div class='row'>"
"<button id='toggle' class='");

  page += btnClass;
  page += F("'>");
  page += btnText;

  page += F(
"</button>"
"<button id='refresh' class='pill'>Refresh</button>"
"</div>"
"<footer id='wifi' class='pill ok'>WiFi: Terhubung</footer>"
"</div>"
"<script>"
"const stateEl=document.getElementById('state');"
"const btn=document.getElementById('toggle');"
"const refresh=document.getElementById('refresh');"
"function render(s){stateEl.textContent=s?'ON':'OFF';"
"btn.textContent=s?'Matikan':'Nyalakan';"
"btn.className=s?'off':'on';}"
"async function getStatus(){try{const r=await fetch('/status');"
"const j=await r.json();render(j.relay);}catch(e){console.log(e);}}"
"btn.onclick=async()=>{try{const r=await fetch('/toggle');"
"const j=await r.json();render(j.relay);}catch(e){alert('Gagal toggle');}};"
"refresh.onclick=getStatus;setInterval(getStatus,2000);"
"</script></body></html>");

  return page;
}

// ================== HTTP Handlers ==================
void handleRoot()   { server.send(200, "text/html", htmlPage()); }
void handleOn()     { relayOn = true;  applyRelayOutput(); saveRelayState(relayOn); server.send(200, "application/json", "{\"relay\":true}"); }
void handleOff()    { relayOn = false; applyRelayOutput(); saveRelayState(relayOn); server.send(200, "application/json", "{\"relay\":false}"); }
void handleToggle() { relayOn = !relayOn; applyRelayOutput(); saveRelayState(relayOn); server.send(200, "application/json", relayOn? "{\"relay\":true}" : "{\"relay\":false}"); }
void handleStatus() { server.send(200, "application/json", relayOn? "{\"relay\":true}" : "{\"relay\":false}"); }
void handle404()    { server.send(404, "text/plain", "404 Not Found"); }

// ============ Helper: konek ke salah satu SSID ============
bool connectToWiFi() {
  for (int i = 0; i < WIFI_COUNT; i++) {
    Serial.print("Mencoba SSID ["); Serial.print(i+1); Serial.print("]: ");
    Serial.println(WIFI_SSIDS[i]);

    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSIDS[i], WIFI_PASSWORDS[i]);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Tersambung ke "); Serial.print(WIFI_SSIDS[i]);
      Serial.print("  IP: "); Serial.println(WiFi.localIP());
      return true;
    } else {
      Serial.println("Gagal, coba SSID berikutnya...");
    }
  }
  return false;
}

// ============================= Setup =============================
void setup() {
  pinMode(RELAY_PIN, OUTPUT);

  EEPROM.begin(EEPROM_SIZE);

  // Ambil status terakhir dari EEPROM
  relayOn = loadRelayState();
  applyRelayOutput();

  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println("Booting...");

  // ---------- Mode STA + IP statik ----------
  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Gagal set IP statik (WiFi.config)");
  }

  if (connectToWiFi()) {
    Serial.println("MODE STA OK.");
  } else {
    // ---------- Fallback: Access Point ----------
    Serial.println("Semua SSID gagal. Mengaktifkan AP...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apGW, apMASK);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("AP aktif. SSID: "); Serial.print(AP_SSID);
    Serial.print("  PASS: "); Serial.print(AP_PASS);
    Serial.print("  IP: "); Serial.println(WiFi.softAPIP());
  }

  // Routes & server
  server.on("/",        handleRoot);
  server.on("/on",      handleOn);
  server.on("/off",     handleOff);
  server.on("/toggle",  handleToggle);
  server.on("/status",  handleStatus);
  server.onNotFound(    handle404);
  server.begin();
  Serial.println("HTTP server start @ port 80");
}

// ============================== Loop =============================
void loop() {
  server.handleClient();
}
