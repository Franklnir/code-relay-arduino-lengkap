#include <ESP8266WiFi.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>

// Ganti dengan kredensial WiFi Anda
const char *ssid = "GEORGIA";
const char *password = "Georgia12345";

// Kredensial Bot Telegram Anda
const String BOT_TOKEN = "7833050640:AAFbapxzmp_RE_fNnmDssqByh7Ank19prKY";
const String CHAT_ID = "1995225615";

// Deklarasi relay pins dan status
const int relayPins[] = {14, 12, 0, 2}; // D5, D6, D3, D4 (GPIO)
const int NUM_RELAYS = 4;
bool relayStatus[NUM_RELAYS] = {false, false, false, false}; // Status awal semua off
long lastMessageOffset = 0; // Untuk melacak pesan terakhir yang diproses

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

void setup() {
  Serial.begin(115200);
  Serial.println();

  // Inisialisasi pin relay
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // Relays are active low, so HIGH turns them off
  }

  // Koneksi WiFi
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  client.setInsecure(); // Disable certificate validation for simpler connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected!");
  Serial.println(WiFi.localIP());

  // Kirim pesan ke Telegram bahwa bot sudah online
  bot.sendMessage(CHAT_ID, "Bot is online!", "");
}

void loop() {
  handleNewMessages();
  delay(1000); // Tunggu sebentar sebelum memeriksa pesan baru lagi
}

void handleNewMessages() {
  // Panggil getUpdates() dengan offset terakhir yang diketahui
  int numNewMessages = bot.getUpdates(lastMessageOffset);

  if (numNewMessages > 0) {
    for (int i = 0; i < numNewMessages; i++) {
      String text = bot.messages[i].text;
      String chat_id = bot.messages[i].chat_id;
      
      // Update offset untuk menghindari pemrosesan pesan yang sama berulang kali
      lastMessageOffset = bot.messages[i].update_id + 1;

      if (chat_id == CHAT_ID) {
        Serial.print("Received command: ");
        Serial.println(text);

        if (text == "/start") {
          sendWelcomeMessage();
        } else if (text == "status") {
          sendStatusMessage();
        } else if (text.startsWith("1 nyala") || text.startsWith("1 on")) {
          controlRelay(0, true);
        } else if (text.startsWith("1 mati") || text.startsWith("1 off")) {
          controlRelay(0, false);
        } else if (text.startsWith("2 nyala") || text.startsWith("2 on")) {
          controlRelay(1, true);
        } else if (text.startsWith("2 mati") || text.startsWith("2 off")) {
          controlRelay(1, false);
        } else if (text.startsWith("3 nyala") || text.startsWith("3 on")) {
          controlRelay(2, true);
        } else if (text.startsWith("3 mati") || text.startsWith("3 off")) {
          controlRelay(2, false);
        } else if (text.startsWith("4 nyala") || text.startsWith("4 on")) {
          controlRelay(3, true);
        } else if (text.startsWith("4 mati") || text.startsWith("4 off")) {
          controlRelay(3, false);
        } else {
          bot.sendMessage(CHAT_ID, "Command not recognized. Please use 'status', '1 nyala', '1 mati', etc.", "");
        }
      }
    }
  }
}

void sendWelcomeMessage() {
  String welcomeMsg = "Halo! Saya adalah bot kontrol saklar. Gunakan perintah berikut:\n";
  welcomeMsg += "- **status**: Menampilkan status semua saklar.\n";
  welcomeMsg += "- **1 nyala**: Menyalakan saklar 1.\n";
  welcomeMsg += "- **1 mati**: Mematikan saklar 1.\n";
  welcomeMsg += "Dan seterusnya untuk saklar 2, 3, dan 4.";
  bot.sendMessage(CHAT_ID, welcomeMsg, "Markdown");
}

void sendStatusMessage() {
  String statusMsg = "Status Saklar:\n";
  for (int i = 0; i < NUM_RELAYS; i++) {
    statusMsg += "Saklar " + String(i + 1) + ": ";
    statusMsg += (relayStatus[i] ? String("✅ ON") : String("❌ OFF")) + "\n";
  }
  bot.sendMessage(CHAT_ID, statusMsg, "Markdown");
}

void controlRelay(int relayIndex, bool turnOn) {
  if (relayIndex >= 0 && relayIndex < NUM_RELAYS) {
    relayStatus[relayIndex] = turnOn;
    
    // Relays are active low, so LOW turns them on
    digitalWrite(relayPins[relayIndex], turnOn ? LOW : HIGH); 

    String msg = "Saklar " + String(relayIndex + 1) + " sekarang ";
    msg += (turnOn ? "✅ ON" : "❌ OFF");
    bot.sendMessage(CHAT_ID, msg, "Markdown");
  }
}
