// ====================== BOARD & WIFI ======================
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecure.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
#endif

#include <time.h>
#include <sys/time.h>   // settimeofday

#define FIREBASE_DISABLE_SD
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ====================== KONFIG PROJECT ======================
#define WIFI_SSID      "GEORGIA"
#define WIFI_PASSWORD  "Georgia12345"

#define API_KEY        "AIzaSyD69fUIQw6sKicbp3kbkqB114gMFonfklM"
#define DATABASE_URL   "https://projectrumah-6e924-default-rtdb.firebaseio.com/"

#define USER_EMAIL     "myfrankln@gmail.com"
#define USER_PASSWORD  "123456"

// ====================== RELAY (ACTIVE LOW) ======================
const int relayPins[4] = {14, 12, 0, 2}; // D5, D6, D3, D4
bool lastRelayState[4] = {false, false, false, false};
const char* RELAY_LABELS[4] = {"relay 1","relay 2","relay 3","relay 4"}; // hanya untuk log

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

// ====================== SCHEDULER ======================
unsigned long lastScheduleTick = 0;
const unsigned long TICK_MS = 1000;
String lastMinuteChecked = "";

// ====================== PATH RUNTIME ======================
String uid;
String baseUserPath;
String baseRelaysPath;   // "/users/{uid}/relay_4_channel"

// ====================== UTIL: timegm portable ======================
static bool isLeap(int y){ return ((y%4==0)&&(y%100!=0))||(y%400==0); }
static long daysBeforeYear(int y){ long d=0; for(int yr=1970; yr<y; ++yr) d+=365+(isLeap(yr)?1:0); return d; }
static int  daysBeforeMonth(int y,int m0){ static const int cum[12]={0,31,59,90,120,151,181,212,243,273,304,334}; int d=cum[m0]; if(m0>=2&&isLeap(y)) d+=1; return d; }
static time_t timegm_portable(const struct tm* tmv){
  int y=tmv->tm_year+1900, m=tmv->tm_mon, d=tmv->tm_mday;
  long long days=(long long)daysBeforeYear(y)+daysBeforeMonth(y,m)+(d-1);
  return (time_t)(days*86400LL + tmv->tm_hour*3600LL + tmv->tm_min*60LL + tmv->tm_sec);
}

// ====================== UTIL: getLocalTime wrapper ======================
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

// ====================== UTIL: LOG & GUARD ======================
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

// ====================== WAKTU HELPERS ======================
String now_HHMM(){ struct tm t; if(!myGetLocalTime(&t)) return ""; char b[6]; snprintf(b,sizeof(b),"%02d:%02d",t.tm_hour,t.tm_min); return String(b); }
String todayKey(){ struct tm t; if(!myGetLocalTime(&t)) return ""; const char* keys[7]={"sun","mon","tue","wed","thu","fri","sat"}; return String(keys[t.tm_wday]); }

// ====================== RELAY I/O ======================
void applyRelay(int idx, bool on){
  if(idx<0 || idx>=4) return;
  lastRelayState[idx]=on;
  digitalWrite(relayPins[idx], on?LOW:HIGH); // ACTIVE LOW
  Serial.printf("[RELAY] %s -> %s\n", RELAY_LABELS[idx], on? "ON":"OFF");
}
// tulis kembali STATE aktual (sumber kebenaran = 'state')
void writeStateToCloud(int idx, bool state){
  Firebase.RTDB.setBool(&fbdo, baseRelaysPath + "/" + String(idx) + "/state", state);
}
// meta opsional
void ackMeta(int idx, const char* byTag){
  String base = baseRelaysPath + "/" + String(idx) + "/meta";
  Firebase.RTDB.setString(&fbdo, base + "/by", byTag);
  Firebase.RTDB.setInt(&fbdo,    base + "/ts", (int)time(nullptr));
}

// ====================== STREAM ======================
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
// parse "/0/state" → 0
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

  for(int i=0;i<4;i++){ pinMode(relayPins[i], OUTPUT); digitalWrite(relayPins[i], HIGH); } // default OFF

  waitForWifi();
  ensureTime();

  if(!authAndBegin()){ Serial.println("Inisialisasi Firebase gagal. Stop."); while(true){ delay(1000); } }

  // Bentuk path BARU (sesuai Android):
  baseUserPath   = "/users/" + uid;
  baseRelaysPath = baseUserPath + "/relay_4_channel";   // <— inti

  // Seed/ambil state awal
  for(int i=0;i<4;i++){
    String p = baseRelaysPath + "/" + String(i) + "/state";
    if(Firebase.RTDB.getBool(&fbdo, p)){
      applyRelay(i, fbdo.boolData());
    } else {
      Firebase.RTDB.setBool(&fbdo, p, false);
      applyRelay(i, false);
    }
  }

  // Stream di folder base; kita hanya proses event boolean di ".../state"
  beginStreamWithRetry(baseRelaysPath);
}

// ====================== LOOP ======================
void loop(){
  if(WiFi.status()!=WL_CONNECTED){ Serial.println("WiFi putus, reconnect..."); waitForWifi(); }

  // 1) Stream: dengarkan perubahan {idx}/state
  if(Firebase.RTDB.readStream(&stream)){
    if(stream.dataAvailable()){
      if(stream.dataTypeEnum()==fb_esp_rtdb_data_type_boolean){
        String p = stream.dataPath(); // contoh: "/0/state"
        int idx = parseIndexFromStatePath(p);
        if(idx >= 0){
          bool v = stream.boolData();
          if(v != lastRelayState[idx]){
            applyRelay(idx, v);
            // tulis balik supaya Android & device konsisten (tidak masalah karena kita cek perubahan)
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

  // 2) Scheduler: jalan tiap menit (cek per detik agar ringan)
  unsigned long nowMs = millis();
  if(nowMs - lastScheduleTick >= TICK_MS){
    lastScheduleTick = nowMs;

    String hhmm = now_HHMM();
    if(hhmm.length() && hhmm != lastMinuteChecked){
      lastMinuteChecked = hhmm;

      String today = todayKey(); // "sun","mon",...
      // Android menandai hari dengan "mon..sun"; jika node tidak ada → kita anggap aktif (compatible)
      for(int i=0;i<4;i++){
        String chBase = baseRelaysPath + "/" + String(i);

        // Hari aktif?
        bool todayEnabled = true;
        if(Firebase.RTDB.getBool(&fbdo, chBase + "/days/" + today)){ todayEnabled = fbdo.boolData(); }
        if(!todayEnabled) continue;

        String onStr, offStr;
        if(Firebase.RTDB.getString(&fbdo, chBase + "/waktuON"))  onStr  = fbdo.stringData();
        if(Firebase.RTDB.getString(&fbdo, chBase + "/waktuOFF")) offStr = fbdo.stringData();

        if(onStr == hhmm && !lastRelayState[i]){
          applyRelay(i, true);
          writeStateToCloud(i, true);
          ackMeta(i, "schedule");
          Serial.printf("[AUTO] %s ON @ %s\n", RELAY_LABELS[i], hhmm.c_str());
        }
        if(offStr == hhmm && lastRelayState[i]){
          applyRelay(i, false);
          writeStateToCloud(i, false);
          ackMeta(i, "schedule");
          Serial.printf("[AUTO] %s OFF @ %s\n", RELAY_LABELS[i], hhmm.c_str());
        }
      }
    }
  }
}
