#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>

// Definisikan pin sensor dan LED
#define DHT22_PIN_1 14
#define DHT22_PIN_2 27
#define LED_WIFI_PIN 2      // LED Biru untuk status WiFi
#define LED_TEMP_PIN 4      // LED Merah untuk status suhu
#define RESET_BUTTON_PIN 18 // Tombol reset WiFi
#define RELAY_SPRAYER_PIN 15 // Relay untuk sprayer

#define DHT22_TYPE DHT22 

DHT dht1(DHT22_PIN_1, DHT22_TYPE);
DHT dht2(DHT22_PIN_2, DHT22_TYPE);

const String IMEI = "1000000005";
String firebaseHost = "https://jamur-iot-de497-default-rtdb.asia-southeast1.firebasedatabase.app/";

// Batas suhu dan kelembapan untuk sprayer
const float TEMP_MAX = 33.0;       // Suhu maksimum sebelum sprayer menyala (°C)
const float HUMIDITY_MIN = 60.0;   // Kelembapan minimum sebelum sprayer menyala (%)
const float TEMP_HYSTERESIS = 1.0; // 1°C hysteresis untuk suhu
const float HUMIDITY_HYSTERESIS = 5.0; // 5% hysteresis untuk kelembapan

// Status relay
bool sprayerActive = false;
bool lastSprayerStatus = false;

// Variabel untuk NTP
bool ntpSynced = false;

// Variabel untuk tombol reset
unsigned long buttonPressStartTime = 0;
const unsigned long resetHoldTime = 3000;
bool resetInProgress = false;

// WiFi Manager
WiFiManager wifiManager;

void setupTime() {
  Serial.print("⏳ Sinkronisasi waktu NTP...");
  configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    ntpSynced = true;
    Serial.println(" ✅ Berhasil!");
  } else {
    Serial.println(" ⚠️ Gagal");
  }
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "00/00/0000 00.00.00";
  }
  char buffer[30];
  snprintf(buffer, sizeof(buffer), "%d/%d/%d %02d.%02d.%02d",
           timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year+1900,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buffer);
}

void controlSprayer(float avgTemp, float avgHumidity) {
  lastSprayerStatus = sprayerActive;

  if (!sprayerActive) {
    if (avgTemp >= TEMP_MAX || avgHumidity < HUMIDITY_MIN) {
      sprayerActive = true;
      digitalWrite(RELAY_SPRAYER_PIN, HIGH);
      digitalWrite(LED_TEMP_PIN, HIGH);
      Serial.println("💦 SPRAYER ON (Kondisi kritis)");
      if (avgTemp >= TEMP_MAX) {
        Serial.println("🔥 PERINGATAN: Suhu terlalu tinggi!");
      }
      if (avgHumidity < HUMIDITY_MIN) {
        Serial.println("🏜️ PERINGATAN: Kelembapan terlalu rendah!");
      }
    }
  } else {
    if (avgTemp < TEMP_MAX && avgHumidity >= HUMIDITY_MIN) {
      sprayerActive = false;
      digitalWrite(RELAY_SPRAYER_PIN, LOW);
      digitalWrite(LED_TEMP_PIN, LOW);
      Serial.println("💦 SPRAYER OFF (Kondisi normal)");
    } else {
      Serial.println("💦 SPRAYER ON (Kondisi kritis)");
      if (avgTemp >= TEMP_MAX) {
        Serial.println("🔥 PERINGATAN: Suhu terlalu tinggi!");
      }
      if (avgHumidity < HUMIDITY_MIN) {
        Serial.println("🏜️ PERINGATAN: Kelembapan terlalu rendah!");
      }
    }
  }
}

void handleResetProcedure() {
  Serial.println("\n🔴 Tombol reset ditekan 3 detik - Reset WiFi dilakukan!");
  
  // Matikan relay dan reset status saat reset
  sprayerActive = false;
  digitalWrite(RELAY_SPRAYER_PIN, LOW);
  digitalWrite(LED_TEMP_PIN, LOW);
  Serial.println("🔒 Sprayer dimatikan untuk keamanan saat reset");
  
  // Indikator visual - kedipkan LED WiFi cepat
  for(int i = 0; i < 10; i++) {
    digitalWrite(LED_WIFI_PIN, !digitalRead(LED_WIFI_PIN));
    delay(200);
  }
  
  // Reset WiFi Manager
  wifiManager.resetSettings();
  // Buat SSID unik berdasarkan IMEI
  char ssid[32];
  sprintf(ssid, "JamurIoT_%s", IMEI.c_str());
  
  wifiManager.startConfigPortal(ssid,""); 
  Serial.println("♻️ Konfigurasi WiFi direset");
  
  // Juga hapus kredensial WiFi yang tersimpan
  WiFi.disconnect(true);
  delay(1000);
  
  // Indikator reset berhasil
  digitalWrite(LED_WIFI_PIN, HIGH);
  delay(1000);
  
  Serial.println("✅ Reset WiFi berhasil! ESP32 akan restart...");
  delay(1000);
  ESP.restart();
}

void checkResetButton() {
  // Tombol aktif LOW karena menggunakan INPUT_PULLUP
  if(digitalRead(RESET_BUTTON_PIN) == LOW) {
    if(!resetInProgress) {
      // Catat waktu pertama kali tombol ditekan
      if(buttonPressStartTime == 0) {
        buttonPressStartTime = millis();
      }
      
      // Cek jika sudah ditekan cukup lama (3 detik)
      if(millis() - buttonPressStartTime >= resetHoldTime) {
        resetInProgress = true;
        handleResetProcedure();
      }
    }
  } else {
    // Tombol dilepas sebelum 3 detik
    if(buttonPressStartTime > 0 && !resetInProgress) {
      unsigned long holdTime = millis() - buttonPressStartTime;
      if(holdTime > 1000) {
        Serial.printf("🟢 Tombol reset dilepas setelah %lu ms (butuh 3 detik untuk reset)\n", holdTime);
      }
    }
    buttonPressStartTime = 0;
    resetInProgress = false;
  }
}

void handleWiFiConnection() {
  static bool lastWiFiStatus = true;

  if (WiFi.status() != WL_CONNECTED) {
    if (lastWiFiStatus) {
      Serial.println("⚠️ WiFi terputus! Mencoba reconnect...");
      lastWiFiStatus = false;
    }

    digitalWrite(LED_WIFI_PIN, HIGH); // LED menyala saat koneksi putus

    WiFi.reconnect();

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      digitalWrite(LED_WIFI_PIN, (millis() % 500) < 250 ? HIGH : LOW); // LED berkedip
      delay(100);
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ WiFi terkoneksi kembali");
      Serial.print("📍 IP Baru: ");
      Serial.println(WiFi.localIP());
      digitalWrite(LED_WIFI_PIN, LOW); // Matikan LED WiFi saat sukses
      lastWiFiStatus = true;

      if (!ntpSynced) {
        setupTime();
      }
    }
  } else {
    if (!lastWiFiStatus) {
      Serial.println("✅ WiFi sudah kembali terhubung");
      Serial.print("📍 IP Saat ini: ");
      Serial.println(WiFi.localIP());
      digitalWrite(LED_WIFI_PIN, LOW); // Pastikan LED tetap mati saat reconnect sukses
    }
    lastWiFiStatus = true;
  }
}

void sendDataToFirebase(float suhu1, float kelembapan1, float suhu2, float kelembapan2, float rataRataSuhu, float rataRataKelembapan) {
  HTTPClient http;
  String url = firebaseHost + "devices/" + IMEI + ".json";
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  String jsonData = "{";
  jsonData += "\"lastUpdate\":\"" + getFormattedTime() + "\",";
  jsonData += "\"temperatureAverage\":" + String(rataRataSuhu, 1) + ",";
  jsonData += "\"humidityAverage\":" + String(rataRataKelembapan, 1) + ",";
  
  if(!isnan(suhu1)) {
    jsonData += "\"temperature1\":" + String(suhu1, 1) + ",";
    jsonData += "\"humidity1\":" + String(kelembapan1, 1) + ",";
  }
  
  if(!isnan(suhu2)) {
    jsonData += "\"temperature2\":" + String(suhu2, 1) + ",";
    jsonData += "\"humidity2\":" + String(kelembapan2, 1) + ",";
  }
  
  jsonData += "\"sprayerStatus\":" + String(sprayerActive ? "true" : "false") + ",";
  jsonData += "\"status\":\"" + String(ntpSynced ? "OK" : "NTP_PENDING") + "\"";
  jsonData += "}";
  
  int httpResponseCode = http.PATCH(jsonData);
  
  if(httpResponseCode > 0) {
    Serial.printf("✅ Data terkirim ke Firebase. Kode: %d\n", httpResponseCode);
  } else {
    Serial.printf("❌ Gagal kirim data ke Firebase. Kode error: %d\n", httpResponseCode);
  }
  
  http.end();
}

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_WIFI_PIN, OUTPUT);
  pinMode(LED_TEMP_PIN, OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RELAY_SPRAYER_PIN, OUTPUT);
  
  sprayerActive = false;
  digitalWrite(RELAY_SPRAYER_PIN, LOW);
  digitalWrite(LED_WIFI_PIN, HIGH); // LED WiFi nyala saat startup
  digitalWrite(LED_TEMP_PIN, LOW);

  Serial.println("\n🚀 Sistem Kontrol Sprayer Jamur IoT");

  // Konfigurasi WiFi Manager
  wifiManager.setTimeout(180); // Timeout 3 menit
  wifiManager.setDebugOutput(true);
  
  // Buat SSID unik berdasarkan IMEI
  char ssid[32];
  sprintf(ssid, "JamurIoT_%s", IMEI.c_str());
  
  Serial.printf("📡 Mencoba koneksi ke WiFi [%s]...\n", ssid);
  
  if(!wifiManager.autoConnect(ssid)) {
    Serial.println("❌ Gagal terhubung & timeout");
    digitalWrite(LED_WIFI_PIN, HIGH); // LED menyala tetap saat gagal
    delay(3000);
    ESP.restart();
  }

  Serial.println("✅ WiFi terhubung!");
  Serial.print("📍 IP: ");
  Serial.println(WiFi.localIP());
  digitalWrite(LED_WIFI_PIN, LOW); // Matikan LED WiFi setelah terkoneksi

  // Setup NTP
  setupTime();
  
  // Inisialisasi sensor
  dht1.begin();
  dht2.begin();
  Serial.println("🌡️ Sensor DHT22 siap");
}

void readSensorData() {
  Serial.printf("\n⏰ %s - Pembacaan sensor:\n", getFormattedTime().c_str());
  
  float suhu1 = dht1.readTemperature();
  float kelembapan1 = dht1.readHumidity();
  float suhu2 = dht2.readTemperature();
  float kelembapan2 = dht2.readHumidity();

  bool sensor1Valid = !isnan(suhu1) && !isnan(kelembapan1);
  bool sensor2Valid = !isnan(suhu2) && !isnan(kelembapan2);
  
  if(sensor1Valid) Serial.printf("🌡️ Sensor 1: %.1f°C, %.1f%%\n", suhu1, kelembapan1);
  else Serial.println("❌ Sensor 1 error");
  
  if(sensor2Valid) Serial.printf("🌡️ Sensor 2: %.1f°C, %.1f%%\n", suhu2, kelembapan2);
  else Serial.println("❌ Sensor 2 error");
  
  float rataRataSuhu = 0;
  float rataRataKelembapan = 0;
  int sensorValidCount = 0;
  
  if(sensor1Valid) {
    rataRataSuhu += suhu1;
    rataRataKelembapan += kelembapan1;
    sensorValidCount++;
  }
  if(sensor2Valid) {
    rataRataSuhu += suhu2;
    rataRataKelembapan += kelembapan2;
    sensorValidCount++;
  }
  
  if(sensorValidCount > 0) {
    rataRataSuhu /= sensorValidCount;
    rataRataKelembapan /= sensorValidCount;
    Serial.printf("📊 Rata-rata: %.1f°C, %.1f%%\n", rataRataSuhu, rataRataKelembapan);
    
    controlSprayer(rataRataSuhu, rataRataKelembapan);
    
    if(WiFi.status() == WL_CONNECTED) {
      sendDataToFirebase(suhu1, kelembapan1, suhu2, kelembapan2, rataRataSuhu, rataRataKelembapan);
    } else {
      Serial.println("⚠️ Tidak terhubung ke WiFi - Data tidak terkirim");
    }
  } else {
    Serial.println("❌ Tidak ada data sensor valid");
    if (sprayerActive) {
      sprayerActive = false;
      digitalWrite(RELAY_SPRAYER_PIN, LOW);
      digitalWrite(LED_TEMP_PIN, HIGH);
      Serial.println("🔒 Sprayer dimatikan");
    }
  }
}

void loop() {
  checkResetButton();
  handleWiFiConnection();
  readSensorData();
  delay(10000);
}
