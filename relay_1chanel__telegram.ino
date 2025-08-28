#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>  // NTP WIB

// ====== WiFi ======
const char *ssid     = "GEORGIA";
const char *password = "Georgia12345";

// ====== Telegram ======
const String BOT_TOKEN = "7833050640:AAFbapxzmp_RE_fNnmDssqByh7Ank19prKY";
const String CHAT_ID   = "1995225615";

// ====== Relay ======
const int  RELAY_PIN        = 12;   // D6
const bool RELAY_ACTIVE_LOW = true; // LOW=ON, HIGH=OFF
bool relayOn = false;

// ====== Telegram objs ======
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);
long lastUpdateId = 0;

// ====== Waktu (WIB) ======
const long GMT_OFFSET_SEC = 7 * 3600;
const int  DST_OFFSET_SEC = 0;
bool timeReady = false;

// ====== BMP280 ======
Adafruit_BMP280 bmp; // I2C
bool  bmpReady     = false;
float lastTempC    = NAN;
float lastPressPa  = NAN;  // dari BMP280 (Pascal)

// ====== Jadwal (timer) — simpan epoch absolut ======
bool   timerActive   = false;
time_t timerEndEpoch = 0;        // jam OFF dalam epoch
unsigned long timerCheckTick = 0;

// ====== Aturan Suhu (histeresis) ======
bool  tempRuleActive = false;
float tempOnThreshC  = 32.0;
float tempOffThreshC = 25.0;

// ====== Persist ======
const char *STATE_FILE = "/state.ini";

// ---------------- UTIL ----------------
void writeRelay(bool on) {
  relayOn = on;
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH)
                                           : (on ? HIGH : LOW));
}

void setRelay(bool on, bool notify=true) {
  writeRelay(on);
  if (notify) bot.sendMessage(CHAT_ID, String("Saklar sekarang ") + (on ? "✅ ON" : "❌ OFF"), "Markdown");
}

bool getLocal(struct tm &out) {
  if (!timeReady) return false;
  return getLocalTime(&out, 200);
}

String fmtNow() {
  struct tm t;
  if (!getLocal(t)) return "Waktu belum sinkron (NTP)";
  char buf[32]; strftime(buf, sizeof(buf), "%H:%M:%S %d-%m-%Y", &t);
  return String(buf);
}

String fmtHMS(unsigned long s) {
  unsigned long h = s / 3600, m = (s % 3600) / 60, d = s % 60;
  char buf[16]; snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, d);
  return String(buf);
}

// ---------- Persist (LittleFS) ----------
void saveState() {
  if (!LittleFS.begin()) return;
  File f = LittleFS.open(STATE_FILE, "w");
  if (!f) return;
  f.printf("relayOn=%d\n", relayOn?1:0);
  f.printf("timerActive=%d\n", timerActive?1:0);
  f.printf("timerEndEpoch=%ld\n", (long)timerEndEpoch);
  f.printf("tempRuleActive=%d\n", tempRuleActive?1:0);
  f.printf("tempOn=%.2f\n", tempOnThreshC);
  f.printf("tempOff=%.2f\n", tempOffThreshC);
  f.close();
}

void loadState() {
  if (!LittleFS.begin()) return;
  if (!LittleFS.exists(STATE_FILE)) return;
  File f = LittleFS.open(STATE_FILE, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    if      (k=="relayOn")        relayOn        = (v.toInt()!=0);
    else if (k=="timerActive")    timerActive    = (v.toInt()!=0);
    else if (k=="timerEndEpoch")  timerEndEpoch  = (time_t)v.toInt();
    else if (k=="tempRuleActive") tempRuleActive = (v.toInt()!=0);
    else if (k=="tempOn")         tempOnThreshC  = v.toFloat();
    else if (k=="tempOff")        tempOffThreshC = v.toFloat();
  }
  f.close();
}

void touchSaveSoon() {
  static unsigned long last=0;
  if (millis()-last > 300) { saveState(); last = millis(); }
}

// ---------- Baca BMP ----------
bool readEnv() {
  if (!bmpReady) { lastTempC = NAN; lastPressPa = NAN; return false; }
  float t = bmp.readTemperature(); // °C
  float p = bmp.readPressure();    // Pa
  if (isnan(t) || isnan(p)) { lastTempC = NAN; lastPressPa = NAN; return false; }
  lastTempC = t; lastPressPa = p; return true;
}

void enforceTempRule() {
  if (!tempRuleActive || !bmpReady || isnan(lastTempC)) return;
  if (lastTempC >= tempOnThreshC && !relayOn) {
    setRelay(true, false); touchSaveSoon();
    bot.sendMessage(CHAT_ID, "🌡️ Mencapai ≥ *"+String(tempOnThreshC,1)+"°C* → relay *ON*.", "Markdown");
  } else if (lastTempC <= tempOffThreshC && relayOn) {
    setRelay(false, false); touchSaveSoon();
    bot.sendMessage(CHAT_ID, "🌡️ Turun ≤ *"+String(tempOffThreshC,1)+"°C* → relay *OFF*.", "Markdown");
  }
}

// ---------- Parser teks ----------
long extractNumberBefore(const String& text, const String& key) {
  int kpos = text.indexOf(key); if (kpos < 0) return -1;
  int i = kpos - 1; while (i >= 0 && text[i] == ' ') i--; int e = i;
  while (i >= 0 && isDigit(text[i])) i--; int s = i + 1;
  if (s <= e && isDigit(text[e])) return text.substring(s, e + 1).toInt();
  return -1;
}

// "nyalakan X jam Y menit Z detik" (angka tunggal => menit)
bool parseDurationSec(const String& in, unsigned long &secOut) {
  String t=in; t.toLowerCase();
  long j=extractNumberBefore(t,"jam");
  long m=extractNumberBefore(t,"menit");
  long d=extractNumberBefore(t,"detik");
  if (j<0 && m<0 && d<0) {
    int ns=-1, ne=-1;
    for (int i=0;i<(int)t.length();i++){ if (isDigit(t[i])) { ns=i; break; } }
    if (ns>=0){ ne=ns; while(ne+1<(int)t.length() && isDigit(t[ne+1])) ne++; m=t.substring(ns,ne+1).toInt(); }
  }
  if (j<0) j=0; if (m<0) m=0; if (d<0) d=0;
  unsigned long s = (unsigned long)j*3600UL + (unsigned long)m*60UL + (unsigned long)d;
  if (!s) return false; secOut = s; return true;
}

bool parseUntilClock(const String& in, time_t &epochOut) {
  String t=in; t.toLowerCase();
  int pos=t.indexOf("sampai"); if(pos<0) pos=t.indexOf("hingga"); if(pos<0) return false;
  int c=-1; for(int i=pos;i<(int)t.length();i++){ if(isDigit(t[i])){ c=i; break; } }
  if (c<0) return false;
  int e=c; while(e<(int)t.length() && !isspace(t[e])) e++;
  String clock = t.substring(c,e);
  int a=clock.indexOf(':'); if(a<0) return false;
  int b=clock.indexOf(':', a+1);
  int hh=clock.substring(0,a).toInt();
  int mm=(b<0)? clock.substring(a+1).toInt() : clock.substring(a+1,b).toInt();
  int ss=(b<0)? 0 : clock.substring(b+1).toInt();
  if (hh<0||hh>23||mm<0||mm>59||ss<0||ss>59) return false;

  struct tm now;
  if (!getLocal(now)) return false;
  struct tm target = now; target.tm_hour=hh; target.tm_min=mm; target.tm_sec=ss;
  time_t nowE = mktime(&now), tgt = mktime(&target);
  if (difftime(tgt, nowE) <= 0) { target.tm_mday += 1; tgt = mktime(&target); }
  epochOut = tgt; return true;
}

// kalimat: "nyalakan setiap suhu 32 derajat dan matikan setiap suhu 25 derajat"
bool parseTempRuleSentence(const String& in, float &onTh, float &offTh) {
  String x=in; x.toLowerCase(); int pos=0; float f[2]; int n=0;
  while(n<2) {
    int p=-1,q=-1; for(int i=pos;i<(int)x.length();i++){ if(isDigit(x[i])){ p=i; break; } }
    if (p<0) break; q=p; while(q+1<(int)x.length() && (isDigit(x[q+1])||x[q+1]=='.')) q++;
    int dpos = x.indexOf("derajat", q);
    if (dpos>=0 && dpos-q<=8){ f[n++]=x.substring(p,q+1).toFloat(); pos=dpos+7; }
    else pos=q+1;
  }
  if (n==2){ onTh=f[0]; offTh=f[1]; return true; }
  return false;
}

// ---------- Keyboard ----------
String commandKeyboardJson() {
  return F("["
           "[\"status\",\"waktu\"],"
           "[\"suhu\",\"status suhu\"],"
           "[\"nyala\",\"mati\"],"
           "[\"batal timer\",\"hapus aturan suhu\"],"
           "[\"nyalakan 10 menit\",\"nyalakan sampai 21:30\"]"
           "]");
}
void showKb(const String& msg){ bot.sendMessageWithReplyKeyboard(CHAT_ID, msg, "Markdown", commandKeyboardJson(), true); }
void hideKb (const String& msg){ bot.sendMessageWithReplyKeyboard(CHAT_ID, msg, "Markdown", "[]", true); }

// ---------- Status (sesuai requestmu) ----------
String scheduleExamplesBlock() {
  String s;
  s += "• Durasi: *nyalakan 10 menit*, *nyalakan 2 jam*\n";
  s += "• Sampai jam: *nyalakan sampai 21:30*\n";
  s += "• Suhu: *atur suhu on 32 off 25*, *nyalakan setiap suhu 30 derajat dan matikan setiap suhu 27 derajat*\n";
  s += "• Batalkan jadwal: *batal timer* / *batalkan jadwal*\n";
  return s;
}

void sendStatus() {
  String s = "⏰ Sekarang: *"+fmtNow()+"*\n";

  // Suhu & Tekanan
  String tStr = isnan(lastTempC)  ? "-" : String(lastTempC,1)+"°C";
  String pStr = isnan(lastPressPa)? "-" : String(lastPressPa/100.0,1)+" hPa"; // Pa -> hPa
  s += "🌡️ Suhu: *"+tStr+"*   ⛅ Tekanan: *"+pStr+"*\n";

  // Relay
  s += "💡 Relay: " + String(relayOn ? "✅ ON" : "❌ OFF") + "\n";

  // Jadwal
  s += "🗓️ Jadwal relay:\n";
  if (timerActive && timeReady) {
    struct tm now; getLocal(now);
    time_t nowE = mktime(&now);
    long diff = (long)difftime(timerEndEpoch, nowE);
    if (diff > 0) {
      char tbuf[16]; struct tm tend; localtime_r(&timerEndEpoch, &tend);
      strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tend);
      s += "• Terjadwal: mati *"+String(tbuf)+"* (sisa *"+fmtHMS(diff)+"*)\n";
    } else {
      s += "• Terjadwal: *kedaluwarsa*, menunggu OFF…\n";
    }
  } else {
    s += "• Terjadwal: tidak ada\n";
  }

  // Contoh perintah ditempatkan DI BAGIAN JADWAL (sesuai permintaan)
  s += "\n📌 Contoh perintah cepat:\n";
  s += scheduleExamplesBlock();

  // Catatan kunci jadwal
  s += "\n🔒 Saat ada jadwal aktif, perintah *nyala/mati/toggle/nyalakan ...* ditolak sampai jadwal *dibatalkan*.\n";

  showKb(s);
}

// ---------- Handler pesan ----------
void handleNewMessages() {
  int n = bot.getUpdates(lastUpdateId);
  if (n <= 0) return;

  for (int i=0;i<n;i++) {
    auto &m = bot.messages[i];
    lastUpdateId = m.update_id + 1;

    String chat_id = m.chat_id; if (chat_id != CHAT_ID) { bot.sendMessage(chat_id, "Maaf, akses ditolak.", ""); continue; }
    String text = m.text; text.trim(); String low=text; low.toLowerCase();

    if (low=="/start") {
      showKb("Halo! Ketik *status* untuk ringkasan.\n\n"+scheduleExamplesBlock());

    } else if (low=="perintah" || low=="/help") {
      showKb(scheduleExamplesBlock());

    } else if (low=="hide keyboard") {
      hideKb("Keyboard disembunyikan. Ketik *perintah* untuk memunculkan lagi.");

    } else if (low=="status") {
      sendStatus();

    } else if (low=="waktu") {
      bot.sendMessage(CHAT_ID, "⏰ *"+fmtNow()+"*", "Markdown");

    } else if (low=="suhu") {
      if (readEnv())
        bot.sendMessage(CHAT_ID, "🌡️ Suhu: *"+String(lastTempC,1)+"°C*   ⛅ Tekanan: *"+String(lastPressPa/100.0,1)+" hPa*", "Markdown");
      else
        bot.sendMessage(CHAT_ID, "🌡️/⛅ BMP280 belum siap / gagal baca.", "Markdown");

    } else if (low=="status suhu") {
      String s = "Aturan suhu: "+String(tempRuleActive?"AKTIF":"NONAKTIF")+"\n";
      s += "• ON ≥ *"+String(tempOnThreshC,1)+"°C*, OFF ≤ *"+String(tempOffThreshC,1)+"°C*\n";
      if (readEnv())
        s += "Suhu: *"+String(lastTempC,1)+"°C*, Tekanan: *"+String(lastPressPa/100.0,1)+" hPa*";
      else
        s += "Suhu/Tekanan: *-*\n";
      showKb(s);

    // ---- Lock manual saat jadwal aktif ----
    } else if (low=="nyala" || low=="on" || low=="mati" || low=="off" || low=="toggle") {
      if (timerActive) {
        showKb("🔒 Ada jadwal aktif. Batalkan dulu dengan *batal timer* / *batalkan jadwal*.");
        continue;
      }
      if (low=="nyala" || low=="on")       { setRelay(true);  touchSaveSoon(); }
      else if (low=="mati" || low=="off")  { setRelay(false); touchSaveSoon(); }
      else                                 { setRelay(!relayOn); touchSaveSoon(); }

    } else if (low=="batal timer" || low=="batalkan jadwal") {
      timerActive = false; timerEndEpoch = 0; touchSaveSoon();
      bot.sendMessage(CHAT_ID, "⏹️ Jadwal dibatalkan. Status saklar: "+String(relayOn?"✅ ON":"❌ OFF"), "Markdown");

    } else if (low=="hapus aturan suhu" || low=="matikan aturan suhu") {
      tempRuleActive = false; touchSaveSoon();
      bot.sendMessage(CHAT_ID, "🛑 Aturan suhu *dinonaktifkan*.", "Markdown");

    } else if (low.startsWith("atur suhu on ") || low.startsWith("atur suhu off ") || low.startsWith("atur suhu")) {
      long onN  = extractNumberBefore(low,"on");
      long offN = extractNumberBefore(low,"off");
      if (onN>=0)  tempOnThreshC  = (float)onN;
      if (offN>=0) tempOffThreshC = (float)offN;
      tempRuleActive = true; touchSaveSoon();
      showKb("✅ Aturan suhu *AKTIF*.\n• ON ≥ *"+String(tempOnThreshC,1)+"°C*, OFF ≤ *"+String(tempOffThreshC,1)+"°C*.");

    } else if (low.startsWith("nyalakan setiap suhu")) {
      float onT, offT;
      if (parseTempRuleSentence(low, onT, offT)) {
        tempOnThreshC = onT; tempOffThreshC = offT; tempRuleActive = true; touchSaveSoon();
        showKb("✅ Aturan suhu *AKTIF*.\n• ON ≥ *"+String(onT,1)+"°C*, OFF ≤ *"+String(offT,1)+"°C*.");
      } else {
        showKb("Format tidak dikenali.\nContoh:\n*nyalakan setiap suhu 32 derajat dan matikan setiap suhu 25 derajat*\n*atur suhu on 32 off 25*");
      }

    } else if (low.startsWith("nyalakan")) {
      // Jika sudah ada jadwal, tolak dulu
      if (timerActive) {
        showKb("🔒 Ada jadwal aktif. Batalkan dengan *batal timer* / *batalkan jadwal* sebelum membuat jadwal baru.");
        continue;
      }
      // a) sampai jam
      time_t tgt;
      bool okTime = parseUntilClock(low, tgt);
      if (okTime && timeReady) {
        setRelay(true);
        timerActive   = true;
        timerEndEpoch = tgt; touchSaveSoon();
        struct tm tend; localtime_r(&tgt, &tend); char buf[16]; strftime(buf, sizeof(buf), "%H:%M:%S", &tend);
        showKb("✅ ON sampai *"+String(buf)+"* (WIB).");
      } else {
        // b) durasi
        unsigned long sec;
        if (parseDurationSec(low, sec) && timeReady) {
          struct tm now; getLocal(now); time_t nowE = mktime(&now);
          setRelay(true);
          timerActive   = true;
          timerEndEpoch = nowE + (time_t)sec; touchSaveSoon();
          showKb("✅ ON selama *"+fmtHMS(sec)+"*. Akan mati otomatis.");
        } else {
          showKb("Format tidak dikenali / waktu belum sinkron.\nContoh:\n*nyalakan 10 menit*\n*nyalakan 2 jam*\n*nyalakan sampai 21:30*");
        }
      }

    } else {
      showKb("Perintah tidak dikenali.\n\n"+scheduleExamplesBlock());
    }
  }
}

// ---------- setup & loop ----------
void setup() {
  Serial.begin(115200); Serial.println();
  pinMode(RELAY_PIN, OUTPUT);
  writeRelay(false); // start OFF

  // WiFi
  Serial.printf("Menghubungkan ke WiFi %s", ssid);
  WiFi.begin(ssid, password);
  secureClient.setInsecure();
  while (WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected! IP: "+WiFi.localIP().toString());

  // Filesystem & pulihkan state
  LittleFS.begin();
  loadState();
  writeRelay(relayOn);

  // NTP
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  for (int i=0;i<20;i++){ struct tm t; if(getLocalTime(&t,200)){ timeReady=true; break; } delay(200); }

  // BMP280
  Wire.begin(); // Wemos D1 mini: D2=SDA, D1=SCL
  bmpReady = bmp.begin(0x76) || bmp.begin(0x77);
  if (bmpReady) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
  }

  // Jika ada jadwal tersimpan & waktu siap, evaluasi
  if (timerActive && timeReady) {
    struct tm now; getLocal(now); time_t nowE = mktime(&now);
    if (difftime(timerEndEpoch, nowE) <= 0) {
      timerActive=false; timerEndEpoch=0; setRelay(false,false); touchSaveSoon();
    }
  }

  bot.sendMessage(CHAT_ID, "Bot online. Ketik *status* untuk ringkasan.", "Markdown");
}

void loop() {
  handleNewMessages();

  // Baca lingkungan & enforce suhu tiap ~1s
  static unsigned long lastEnv=0;
  if (millis()-lastEnv >= 1000) {
    lastEnv = millis();
    readEnv();
    enforceTempRule();
  }

  // Cek jadwal tiap ~1s
  if (millis()-timerCheckTick >= 1000) {
    timerCheckTick = millis();
    if (timerActive && timeReady) {
      struct tm now; if (getLocal(now)) {
        time_t nowE = mktime(&now);
        if (difftime(timerEndEpoch, nowE) <= 0) {
          timerActive=false; timerEndEpoch=0; touchSaveSoon();
          if (relayOn) { setRelay(false,false); bot.sendMessage(CHAT_ID, "⏳ Waktu habis. Saklar otomatis *OFF*.", "Markdown"); }
        }
      }
    }
  }

  delay(100);
}
