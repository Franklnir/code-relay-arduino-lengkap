// ====================== BOARD & WIFI ======================
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecure.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
#endif

#include <Wire.h>
#include <time.h>
#include <sys/time.h>   // settimeofday

#define FIREBASE_DISABLE_SD
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ====== BMP280 (I2C) ======
#include <Adafruit_BMP280.h>  // by Adafruit
Adafruit_BMP280 bmp;          // I2C
#define BMP280_I2C_ADDR 0x76

#if defined(ESP8266)
  #define I2C_SDA_PIN 4   // D2
  #define I2C_SCL_PIN 5   // D1
#elif defined(ESP32)
  #define I2C_SDA_PIN 21
  #define I2C_SCL_PIN 22
#endif

// ====================== KONFIG PROJECT ======================
#define WIFI_SSID      "GEORGIA"
#define WIFI_PASSWORD  "Georgia12345"
#define API_KEY        "AIzaSyD69fUIQw6sKicbp3kbkqB114gMFonfklM"
#define DATABASE_URL   "https://projectrumah-6e924-default-rtdb.firebaseio.com/"
#define USER_EMAIL     "myfrankln@gmail.com"
#define USER_PASSWORD  "123456"

// ====================== RELAY (ACTIVE LOW) ======================
// Catatan ESP8266: GPIO0(D3)/GPIO2(D4) pin boot; pastikan HIGH saat boot.
const int relayPins[4] = {14, 12, 0, 2}; // D5, D6, D3, D4
bool lastRelayState[4] = {false, false, false, false};
const char* RELAY_LABELS[4] = {"relay 1","relay 2","relay 3","relay 4"};

// ====================== FIREBASE OBJECTS ======================
FirebaseData fbdo;
FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig config;

// ====================== TIME (WIB) ======================
const long  gmtOffset_sec      = 7 * 3600;
const int   daylightOffset_sec = 0;
const char* ntpServers[] = {
  "id.pool.ntp.org",
  "pool.ntp.org",
  "time.google.com",
  "time.nist.gov"
};

// ====================== TICKERS ======================
unsigned long lastScheduleTick = 0;
const unsigned long TICK_MS = 1000;

unsigned long lastTempRead = 0;
const unsigned long TEMP_PERIOD_MS = 5000;   // baca suhu tiap 5s

String lastMinuteChecked = "";

// ====================== PATH RUNTIME ======================
String uid;
String baseUserPath;    // "/users/{uid}"
String baseRelaysPath;  // "/users/{uid}/relay_4_channel"
String tempPath;        // "/users/{uid}/sensors/temperature"

// ====================== UTIL: timegm & waktu ======================
static bool isLeap(int y){ return ((y%4==0)&&(y%100!=0))||(y%400==0); }
static long daysBeforeYear(int y){ long d=0; for(int yr=1970; yr<y; ++yr) d+=365+(isLeap(yr)?1:0); return d; }
static int  daysBeforeMonth(int y,int m0){ static const int cum[12]={0,31,59,90,120,151,181,212,243,273,304,334}; int d=cum[m0]; if(m0>=2&&isLeap(y)) d+=1; return d; }
static time_t timegm_portable(const struct tm* tmv){
  int y=tmv->tm_year+1900, m=tmv->tm_mon, d=tmv->tm_mday;
  long long days=(long long)daysBeforeYear(y)+daysBeforeMonth(y,m)+(d-1);
  return (time_t)(days*86400LL + tmv->tm_hour*3600LL + tmv->tm_min*60LL + tmv->tm_sec);
}
static bool myGetLocalTime(struct tm* info, uint32_t ms=5000){
  time_t now; uint32_t t0=millis();
  while((millis()-t0)<=ms){ time(&now); if(now>1609459200){ localtime_r(&now, info); return true; } delay(10); yield(); }
  return false;
}
bool syncTimeFromHTTP(){
  WiFiClient client; const char* host="google.com"; if(!client.connect(host,80)) return false;
  client.print(String("HEAD / HTTP/1.1\r\nHost: ")+host+"\r\nConnection: close\r\n\r\n");
  uint32_t t0=millis();
  while(client.connected() && millis()-t0<3000){
    String line=client.readStringUntil('\n');
    if(line.startsWith("Date: ")){
      struct tm tmv{}; char wk[4],mon[4]; int d,y,H,M,S;
      if(sscanf(line.c_str(),"Date: %3s, %d %3s %d %d:%d:%d GMT",wk,&d,mon,&y,&H,&M,&S)==7){
        const char* months="JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* p=strstr(months,mon); int m=p?((int)(p-months)/3):0;
        tmv.tm_year=y-1900; tmv.tm_mon=m; tmv.tm_mday=d; tmv.tm_hour=H; tmv.tm_min=M; tmv.tm_sec=S;
        time_t tutc=timegm_portable(&tmv); struct timeval now={tutc+gmtOffset_sec,0}; settimeofday(&now,nullptr);
        client.stop(); return true;
      }
    }
    if(line=="\r") break;
  }
  client.stop(); return false;
}
static inline void waitForWifi(){
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED){
    Serial.print("."); delay(500);
    if(millis()-t0>30000){ Serial.println("\nRetry WiFi..."); t0=millis(); WiFi.disconnect(true); delay(300); WiFi.begin(WIFI_SSID, WIFI_PASSWORD); }
  }
  Serial.printf("\nWiFi OK. IP=%s\n", WiFi.localIP().toString().c_str());
}
static inline void ensureTime(){
  Serial.print("Sinkronisasi waktu");
  bool ok=false;
  for(size_t i=0;i<sizeof(ntpServers)/sizeof(ntpServers[0]);++i){
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServers[i]);
    struct tm ti; unsigned long t0=millis();
    while(!myGetLocalTime(&ti,250) && (millis()-t0<3500)){ Serial.print("."); delay(250); }
    if(myGetLocalTime(&ti,1)){ ok=true; break; }
  }
  if(!ok){ Serial.print("... fallback HTTP time"); if(syncTimeFromHTTP()) ok=true; }
  Serial.println(ok? " -> OK":" -> GAGAL");
}
static inline bool looksLikeEmail(const String& s){ int at=s.indexOf('@'); int dot=s.lastIndexOf('.'); return (s.length()>5 && at>0 && dot>at+1 && dot<(int)s.length()-1); }
static inline void safeDelay(uint32_t ms){ uint32_t t0=millis(); while(millis()-t0<ms){ delay(1); yield(); } }

// Waktu helpers
String now_HHMM(){ struct tm t; if(!myGetLocalTime(&t)) return ""; char b[6]; snprintf(b,sizeof(b),"%02d:%02d",t.tm_hour,t.tm_min); return String(b); }
String todayKey(){ struct tm t; if(!myGetLocalTime(&t)) return ""; const char* keys[7]={"sun","mon","tue","wed","thu","fri","sat"}; return String(keys[t.tm_wday]); }

// ======= Helper baru: menit sejak tengah malam + window =======
int nowMinutes(){
  struct tm t; if(!myGetLocalTime(&t)) return -1;
  return t.tm_hour*60 + t.tm_min;
}
bool parseHHMM_toMinutes(const String& hhmm, int& outMin){ // return true jika valid
  if(hhmm.length()!=5 || hhmm.charAt(2)!=':') return false;
  int hh = hhmm.substring(0,2).toInt();
  int mm = hhmm.substring(3,5).toInt();
  if(hh<0||hh>23||mm<0||mm>59) return false;
  outMin = hh*60 + mm;
  return true;
}
bool isInWindow(const String& startHHMM, const String& endHHMM, int nowMin){
  int s,e; 
  if(!parseHHMM_toMinutes(startHHMM, s) || !parseHHMM_toMinutes(endHHMM, e)) return true; // kalau tak ada / invalid, anggap bebas
  if(s==e) return true;                    // 24 jam
  if(s<e)  return (nowMin >= s && nowMin < e);
  // lintas tengah malam
  return (nowMin >= s) || (nowMin < e);
}

// ====================== RELAY I/O ======================
void applyRelay(int idx, bool on){
  if(idx<0 || idx>=4) return;
  if(lastRelayState[idx] == on) return;           // hindari flip/flop tulis ulang
  lastRelayState[idx]=on;
  digitalWrite(relayPins[idx], on?LOW:HIGH); // ACTIVE LOW
  Serial.printf("[RELAY] %s -> %s\n", RELAY_LABELS[idx], on? "ON":"OFF");
}
void writeStateToCloud(int idx, bool state){
  Firebase.RTDB.setBool(&fbdo, baseRelaysPath + "/" + String(idx) + "/state", state);
}
void ackMeta(int idx, const char* byTag){
  String base = baseRelaysPath + "/" + String(idx) + "/meta";
  Firebase.RTDB.setString(&fbdo, base + "/by", byTag);
  Firebase.RTDB.setInt(&fbdo,    base + "/ts", (int)time(nullptr));
}

// ====================== STREAM (STATE) ======================
bool beginStreamWithRetry(const String& path, uint8_t maxTry=5){
  for(uint8_t i=0;i<maxTry;i++){
    if(Firebase.RTDB.beginStream(&stream, path.c_str())){
      Serial.printf("Stream aktif di %s\n", path.c_str());
      return true;
    }
    Serial.printf("Gagal mulai stream (%d/%d): %s\n", i+1, maxTry, stream.errorReason().c_str());
    safeDelay(800);
  }
  return false;
}
void ensureStreamAlive(){
  if(!stream.httpConnected() || stream.httpCode()<=0){
    Serial.println("Stream tidak aktif, mencoba re-konek...");
    beginStreamWithRetry(baseRelaysPath);
  }
}
int parseIndexFromStatePath(const String& p){
  // ekspektasi: "/<idx>/state"
  if(!p.startsWith("/")) return -1;
  int slash2 = p.indexOf('/', 1);
  if(slash2 < 0) return -1;
  String mid = p.substring(1, slash2);      // "<idx>"
  String tail= p.substring(slash2+1);       // "state" / ...
  if(tail != "state") return -1;
  int idx = mid.toInt();
  return (idx>=0 && idx<4) ? idx : -1;
}

// ====================== AUTH ======================
bool authAndBegin(){
  String email=String(USER_EMAIL); email.trim();
  String pass =String(USER_PASSWORD); pass.trim();
  Serial.printf("Cek kredensial: email='%s' len=%d, pass len=%d\n",
                email.c_str(), (int)email.length(), (int)pass.length());
  if(!looksLikeEmail(email)){ Serial.println("ERROR: USER_EMAIL invalid."); return false; }
  if(!pass.length()){         Serial.println("ERROR: USER_PASSWORD kosong."); return false; }

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  Serial.print("Auth (coba signUp)...");
  if(!Firebase.signUp(&config, &auth, email.c_str(), pass.c_str())){
    String emsg = config.signer.signupError.message.c_str();
    Serial.printf("Gagal: %s\n", emsg.c_str());
    if(emsg.indexOf("EMAIL_EXISTS") >= 0){
      Serial.println("User sudah ada → login via Firebase.begin()");
      auth.user.email = email.c_str();
      auth.user.password = pass.c_str();
    } else {
      return false;
    }
  } else {
    Serial.println("OK (akun baru dibuat).");
    auth.user.email = email.c_str();
    auth.user.password = pass.c_str();
  }
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  unsigned long t0=millis();
  while(!Firebase.ready() && (millis()-t0<20000)){ Serial.print("."); delay(250); }
  Serial.println();

  uid = auth.token.uid.c_str();
  if(uid.length()==0){ Serial.println("ERROR: UID kosong."); return false; }
  Serial.printf("UID: %s\n", uid.c_str());
  return true;
}

// ====================== SETUP ======================
void setup(){
  Serial.begin(115200); Serial.println();

  // Relay pins default OFF
  for(int i=0;i<4;i++){ pinMode(relayPins[i], OUTPUT); digitalWrite(relayPins[i], HIGH); }

  // I2C begin (pin default board atau custom di atas)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Init BMP280
  bool bmpOK = bmp.begin(BMP280_I2C_ADDR) || bmp.begin(0x77);
  if(!bmpOK){
    Serial.println("BMP280 tidak terdeteksi! Cek wiring/alamat I2C (0x76/0x77). Lanjut tanpa suhu.");
  } else {
    bmp.setSampling(
      Adafruit_BMP280::MODE_NORMAL,
      Adafruit_BMP280::SAMPLING_X2,   // suhu
      Adafruit_BMP280::SAMPLING_X16,  // tekanan
      Adafruit_BMP280::FILTER_X16,
      Adafruit_BMP280::STANDBY_MS_500
    );
  }

  waitForWifi();
  ensureTime();

  if(!authAndBegin()){ Serial.println("Inisialisasi Firebase gagal. Stop."); while(true){ delay(1000); } }

  baseUserPath   = "/users/" + uid;
  baseRelaysPath = baseUserPath + "/relay_4_channel";
  tempPath       = baseUserPath + "/sensors/temperature";

  // Seed /state awal
  for(int i=0;i<4;i++){
    String p = baseRelaysPath + "/" + String(i) + "/state";
    if(Firebase.RTDB.getBool(&fbdo, p)){
      applyRelay(i, fbdo.boolData());
    } else {
      Firebase.RTDB.setBool(&fbdo, p, false);
      applyRelay(i, false);
    }
  }

  // Stream folder base (proses hanya event .../state)
  beginStreamWithRetry(baseRelaysPath);
}

// ====================== LOOP ======================
void loop(){
  if(WiFi.status()!=WL_CONNECTED){ Serial.println("WiFi putus, reconnect..."); waitForWifi(); }

  // 1) Stream: perubahan state manual dari app
  if(Firebase.RTDB.readStream(&stream)){
    if(stream.dataAvailable()){
      if(stream.dataTypeEnum()==fb_esp_rtdb_data_type_boolean){
        String p = stream.dataPath(); // contoh: "/0/state"
        int idx = parseIndexFromStatePath(p);
        if(idx >= 0){
          bool v = stream.boolData();
          if(v != lastRelayState[idx]){
            applyRelay(idx, v);
            writeStateToCloud(idx, v);
            ackMeta(idx, "app");
            Serial.printf("[CMD] %s => %s\n", RELAY_LABELS[idx], v? "ON":"OFF");
          }
        }
      }
    }
  } else {
    if(stream.httpCode()!=0){
      Serial.printf("Stream error (%d): %s\n", stream.httpCode(), stream.errorReason().c_str());
    }
    ensureStreamAlive();
  }

  // 2) Scheduler berdasarkan JAM (cek setiap menit & jaga state di dalam rentang)
  unsigned long nowMs = millis();
  if(nowMs - lastScheduleTick >= TICK_MS){
    lastScheduleTick = nowMs;

    String hhmm = now_HHMM();
    if(hhmm.length() && hhmm != lastMinuteChecked){
      lastMinuteChecked = hhmm;

      String today = todayKey(); // "sun","mon",...
      int nowMin = nowMinutes();

      for(int i=0;i<4;i++){
        String chBase = baseRelaysPath + "/" + String(i);

        // Hari aktif? default true (kalau node tidak ada -> kompatibel mundur)
        bool todayEnabled = true;
        if(Firebase.RTDB.getBool(&fbdo, chBase + "/days/" + today)){ todayEnabled = fbdo.boolData(); }
        if(!todayEnabled) continue;

        String onStr, offStr;
        bool haveOn=false, haveOff=false;
        if(Firebase.RTDB.getString(&fbdo, chBase + "/waktuON"))  { onStr  = fbdo.stringData(); haveOn=true; }
        if(Firebase.RTDB.getString(&fbdo, chBase + "/waktuOFF")) { offStr = fbdo.stringData(); haveOff=true; }

        if(haveOn && haveOff){
          // jaga state selama berada di dalam window (mendukung lintas tengah malam)
          bool shouldBeOn = isInWindow(onStr, offStr, nowMin);
          if(shouldBeOn && !lastRelayState[i]){
            applyRelay(i, true);
            writeStateToCloud(i, true);
            ackMeta(i, "schedule_hold");
            Serial.printf("[AUTO] %s HOLD ON (now=%s window %s-%s)\n", RELAY_LABELS[i], hhmm.c_str(), onStr.c_str(), offStr.c_str());
          } else if(!shouldBeOn && lastRelayState[i]){
            applyRelay(i, false);
            writeStateToCloud(i, false);
            ackMeta(i, "schedule_hold");
            Serial.printf("[AUTO] %s HOLD OFF (now=%s window %s-%s)\n", RELAY_LABELS[i], hhmm.c_str(), onStr.c_str(), offStr.c_str());
          }
        } else {
          // fallback edge-trigger (jika hanya salah satu waktu ada)
          if(haveOn && onStr == hhmm && !lastRelayState[i]){
            applyRelay(i, true);  writeStateToCloud(i, true);  ackMeta(i, "schedule");
            Serial.printf("[AUTO] %s ON @ %s\n", RELAY_LABELS[i], hhmm.c_str());
          }
          if(haveOff && offStr == hhmm && lastRelayState[i]){
            applyRelay(i, false); writeStateToCloud(i, false); ackMeta(i, "schedule");
            Serial.printf("[AUTO] %s OFF @ %s\n", RELAY_LABELS[i], hhmm.c_str());
          }
        }
      }
    }
  }

  // 3) Temperature: baca BMP280 & publish + eksekusi tempRule (hysteresis + window hari/jam)
  if(nowMs - lastTempRead >= TEMP_PERIOD_MS){
    lastTempRead = nowMs;

    float t = bmp.readTemperature(); // Celsius
    if(!isnan(t) && t > -100 && t < 150){ // sanity check
      // publish suhu
      Firebase.RTDB.setFloat(&fbdo, tempPath + "/value", t);
      Firebase.RTDB.setInt(&fbdo,   tempPath + "/ts", (int)time(nullptr));

      String today = todayKey();
      int nowMin = nowMinutes();

      for(int i=0;i<4;i++){
        String base = baseRelaysPath + "/" + String(i) + "/tempRule";

        bool enabled=false; double onAt=0, offAt=0;
        if(Firebase.RTDB.getBool(&fbdo, base + "/enabled")) enabled = fbdo.boolData();
        if(!enabled) continue;

        bool haveOn=false, haveOff=false;
        if(Firebase.RTDB.getFloat(&fbdo, base + "/onAt"))  { onAt  = fbdo.floatData(); haveOn=true; }
        if(Firebase.RTDB.getFloat(&fbdo, base + "/offAt")) { offAt = fbdo.floatData(); haveOff=true; }
        if(!(haveOn && haveOff)) continue;

        // ====== FILTER HARI (opsional) ======
        bool dayOk = true;
        if(Firebase.RTDB.getBool(&fbdo, base + "/days/" + today)){
          dayOk = fbdo.boolData();
        } else {
          // kalau node days tidak ada sama sekali, dayOk tetap true
          // (opsional) cek cepat: jika ada node 'days' tetapi key harinya false, tetap false:
          // tidak perlu get extra, cukup seperti di atas.
        }
        if(!dayOk) continue;

        // ====== FILTER JAM (opsional, mendukung lintas tengah malam) ======
        String winStart, winEnd;
        bool haveStart=false, haveEnd=false;
        if(Firebase.RTDB.getString(&fbdo, base + "/start")) { winStart = fbdo.stringData(); haveStart=true; }
        if(Firebase.RTDB.getString(&fbdo, base + "/end"))   { winEnd   = fbdo.stringData(); haveEnd=true;   }

        if(haveStart && haveEnd){
          if(!isInWindow(winStart, winEnd, nowMin)) {
            // di luar jendela tempRule → lewati (jadwal jam akan memegang kendali)
            continue;
          }
        }
        // ====== HISTERESIS ======
        bool cur = lastRelayState[i];

        if(t >= onAt && !cur){
          applyRelay(i, true);
          writeStateToCloud(i, true);
          ackMeta(i, "temp_rule_on");
          Serial.printf("[TEMP] %s ON (t=%.1f, onAt=%.1f)\n", RELAY_LABELS[i], t, onAt);
        } else if(t <= offAt && cur){
          applyRelay(i, false);
          writeStateToCloud(i, false);
          ackMeta(i, "temp_rule_off");
          Serial.printf("[TEMP] %s OFF (t=%.1f, offAt=%.1f)\n", RELAY_LABELS[i], t, offAt);
        }
        // Jika t berada di antara onAt & offAt → tahan state terakhir (tidak berubah)
      }
    } else {
      Serial.println("[TEMP] gagal baca BMP280 (cek wiring/alamat)");
    }
  }
}
