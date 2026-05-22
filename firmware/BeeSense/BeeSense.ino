// ============================================
//  BeeSense Firmware v5.0
//  I2S DMA audio | WiFi + Cloud | SD storage
// ============================================

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "DHT.h"
#include "WiFi.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "time.h"
#include <sys/time.h>
#include <driver/i2s.h>
#include <driver/adc.h>

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S (0x01)
#endif

// ============================================
//  CONFIGURATION
// ============================================

// Hardware pins
#define SD_CS 5
#define DHTPIN 4
#define DHTTYPE DHT11
#define MIC_ADC_CHANNEL ADC1_CHANNEL_6  // GPIO34

// WiFi (station mode — connects to your network)
const char* WIFI_SSID = "EACCESS";
const char* WIFI_PASS = "hostelnet";

// Cloud API (Cloudflare Worker endpoint for R2 storage)
const char* API_ENDPOINT = "https://beesense-api.beesense.workers.dev";
const char* API_KEY = "beesense-secret-2026";
const char* DEVICE_ID = "beesense-01";

// NTP
const long GMT_OFFSET_SEC = 19800;  // IST = UTC+5:30
const int DAYLIGHT_OFFSET_SEC = 0;

// Recording
#define SAMPLE_RATE 16000   // 16kHz (8kHz causes I2S clock overflow on ESP32)
#define RECORD_SECONDS 60
#define INTERVAL_MINUTES 10

// ============================================
//  CONSTANTS
// ============================================
#define EXPECTED_DATA_BYTES ((uint32_t)SAMPLE_RATE * 2 * RECORD_SECONDS)
#define I2S_READ_SAMPLES 1024
#define I2S_READ_BYTES (I2S_READ_SAMPLES * 2)
#define LOG_BUFFER_SIZE 60

// ============================================
//  GLOBALS
// ============================================
DHT dht(DHTPIN, DHTTYPE);

bool ntpSynced = false;
uint32_t fileCounter = 0;

uint16_t i2sRawBuf[I2S_READ_SAMPLES];
int16_t pcmBuf[I2S_READ_SAMPLES];

// Log ring buffer (viewable via WiFi)
String logBuffer[LOG_BUFFER_SIZE];
int logHead = 0;
int logCount = 0;

int lastWavHttpCode = 0;  // stored so summary can show it

// Status for web dashboard
String statusState = "BOOTING";
String lastRecFile = "-";
String lastRecTime = "-";
float lastTemp = 0, lastHum = 0;
int countdownMin = 0;
uint32_t recordingNum = 0;

// ============================================
//  LOG SYSTEM (Serial + WiFi buffer)
// ============================================
void logMsg(String msg) {
  Serial.println(msg);
  logBuffer[logHead] = msg;
  logHead = (logHead + 1) % LOG_BUFFER_SIZE;
  if (logCount < LOG_BUFFER_SIZE) logCount++;
}

// ============================================
//  WAV HEADER
// ============================================
void writeWAVHeader(File &f, uint32_t dataSize, uint32_t sampleRate) {
  uint8_t h[44];
  h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
  uint32_t cs = 36 + dataSize; memcpy(&h[4], &cs, 4);
  h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
  h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
  uint32_t s1 = 16; memcpy(&h[16], &s1, 4);
  uint16_t af = 1; memcpy(&h[20], &af, 2);
  uint16_t nc = 1; memcpy(&h[22], &nc, 2);
  uint32_t sr = sampleRate; memcpy(&h[24], &sr, 4);
  uint32_t br = sampleRate * 2; memcpy(&h[28], &br, 4);
  uint16_t ba = 2; memcpy(&h[32], &ba, 2);
  uint16_t bp = 16; memcpy(&h[34], &bp, 2);
  h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
  memcpy(&h[40], &dataSize, 4);
  f.write(h, 44);
}

// ============================================
//  TIME
// ============================================
void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    logMsg("[TIME] No WiFi — skipping NTP.");
    return;
  }

  logMsg("[TIME] Syncing NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

  struct tm ti;
  if (getLocalTime(&ti, 10000)) {
    ntpSynced = true;
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    logMsg("[TIME] Synced: " + String(buf));
  } else {
    logMsg("[TIME] NTP timeout.");
  }
}

String getTimestamp() {
  struct tm ti;
  if (ntpSynced && getLocalTime(&ti)) {
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    return String(buf);
  }
  unsigned long s = millis() / 1000;
  char buf[20];
  sprintf(buf, "boot+%lus", s);
  return String(buf);
}

String makeWAVPath() {
  struct tm ti;
  if (ntpSynced && getLocalTime(&ti)) {
    char buf[32];
    strftime(buf, sizeof(buf), "/rec_%Y%m%d_%H%M%S.wav", &ti);
    return String(buf);
  }
  fileCounter++;
  char buf[32];
  sprintf(buf, "/rec_%06lu.wav", (unsigned long)fileCounter);
  return String(buf);
}

// ============================================
//  WIFI (station mode)
// ============================================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  logMsg("[WIFI] Connecting to " + String(WIFI_SSID) + "...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logMsg("[WIFI] Connected. IP: " + WiFi.localIP().toString());
    return true;
  }

  logMsg("[WIFI] Connection failed.");
  return false;
}

// ============================================
//  CLOUD UPLOAD (Cloudflare R2 via Worker)
// ============================================
bool uploadWAVToCloud(String filePath) {
  lastWavHttpCode = 0;

  if (WiFi.status() != WL_CONNECTED) {
    lastWavHttpCode = -100;
    logMsg("[CLOUD] WAV skip: no WiFi");
    return false;
  }

  File wavFile = SD.open(filePath.c_str(), FILE_READ);
  if (!wavFile) {
    lastWavHttpCode = -101;
    logMsg("[CLOUD] WAV skip: can't open file");
    return false;
  }

  size_t fileSize = wavFile.size();
  if (fileSize <= 44) {
    lastWavHttpCode = -102;
    wavFile.close();
    return false;
  }

  uint32_t heap = ESP.getFreeHeap();
  if (heap < 60000) {
    lastWavHttpCode = -103;
    logMsg("[CLOUD] WAV skip: heap " + String(heap));
    wavFile.close();
    return false;
  }

  String filename = filePath;
  if (filename.startsWith("/")) filename = filename.substring(1);
  String url = String(API_ENDPOINT) + "/api/upload/" + filename;

  logMsg("[CLOUD] WAV upload: " + String(fileSize) + "B heap:" +
         String(heap) + " RSSI:" + String(WiFi.RSSI()));

  WiFiClientSecure *client = new WiFiClientSecure();
  if (!client) {
    lastWavHttpCode = -104;
    wavFile.close();
    return false;
  }
  client->setInsecure();
  client->setTimeout(120);

  HTTPClient http;
  http.begin(*client, url);
  http.addHeader("Authorization", "Bearer " + String(API_KEY));
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Device-Id", String(DEVICE_ID));
  http.setTimeout(120000);

  int httpCode = http.sendRequest("PUT", &wavFile, fileSize);
  lastWavHttpCode = httpCode;

  logMsg("[CLOUD] WAV HTTP " + String(httpCode));

  wavFile.close();
  http.end();
  client->stop();
  delete client;

  return (httpCode == 200 || httpCode == 201);
}

bool uploadSensorData(String timestamp, float temp, float humidity,
                      String wavFile, uint32_t sampleRate) {
  logMsg("[CLOUD] === Sensor Upload Start ===");
  logMsg("[CLOUD] WiFi: " + String(WiFi.status()) +
         " | RSSI: " + String(WiFi.RSSI()) + " dBm" +
         " | Heap: " + String(ESP.getFreeHeap()));

  if (WiFi.status() != WL_CONNECTED) {
    logMsg("[CLOUD] No WiFi, skipping sensor upload.");
    return false;
  }

  if (ESP.getFreeHeap() < 50000) {
    logMsg("[CLOUD] Low heap, skipping sensor upload.");
    return false;
  }

  WiFiClientSecure *client = new WiFiClientSecure();
  if (!client) {
    logMsg("[CLOUD] TLS client alloc failed.");
    return false;
  }
  client->setInsecure();
  client->setTimeout(30);

  HTTPClient http;
  http.begin(*client, String(API_ENDPOINT) + "/api/sensor-data");
  http.addHeader("Authorization", "Bearer " + String(API_KEY));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(30000);

  String json = "{";
  json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  json += "\"timestamp\":\"" + timestamp + "\",";
  json += "\"temperature\":" + String(temp, 2) + ",";
  json += "\"humidity\":" + String(humidity, 2) + ",";
  json += "\"wav_file\":\"" + wavFile + "\",";
  json += "\"sample_rate\":" + String(sampleRate);
  json += "}";

  logMsg("[CLOUD] POST sensor data...");
  unsigned long start = millis();

  int httpCode = http.POST(json);

  unsigned long elapsed = millis() - start;
  logMsg("[CLOUD] Response: HTTP " + String(httpCode) + " in " + String(elapsed / 1000) + "s");

  if (httpCode > 0) {
    String body = http.getString();
    logMsg("[CLOUD] Body: " + body.substring(0, 200));
  }

  http.end();
  client->stop();
  delete client;

  if (httpCode == 200 || httpCode == 201) {
    logMsg("[CLOUD] Sensor data OK.");
    return true;
  }

  logMsg("[CLOUD] Sensor upload FAILED (HTTP " + String(httpCode) + ")");
  return false;
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 10);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) out += ' ';
        else out += c;
    }
  }
  return out;
}

void uploadLogs() {
  if (WiFi.status() != WL_CONNECTED || logCount == 0) return;
  if (ESP.getFreeHeap() < 50000) return;

  int start = (logCount < LOG_BUFFER_SIZE) ? 0 : logHead;
  int count = min(logCount, LOG_BUFFER_SIZE);

  // Send only the last 30 lines to keep payload small
  int sendCount = min(count, 30);
  int sendStart = (start + count - sendCount) % LOG_BUFFER_SIZE;

  String json = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"lines\":[";
  for (int i = 0; i < sendCount; i++) {
    int idx = (sendStart + i) % LOG_BUFFER_SIZE;
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(logBuffer[idx]) + "\"";
  }
  json += "]}";

  Serial.println("[LOGS] Sending " + String(sendCount) + " lines (" + String(json.length()) + " bytes)...");

  WiFiClientSecure *client = new WiFiClientSecure();
  if (!client) {
    Serial.println("[LOGS] TLS alloc failed");
    return;
  }
  client->setInsecure();
  client->setTimeout(15);

  HTTPClient http;
  http.begin(*client, String(API_ENDPOINT) + "/api/logs");
  http.addHeader("Authorization", "Bearer " + String(API_KEY));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int code = http.POST(json);
  Serial.println("[LOGS] HTTP " + String(code));

  http.end();
  client->stop();
  delete client;
}

// ============================================
//  I2S ADC (DMA audio capture)
// ============================================
bool i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 16;
  cfg.dma_buf_len = 1024;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(MIC_ADC_CHANNEL, ADC_ATTEN_DB_11);

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err != ESP_OK) {
    logMsg("[I2S] Driver install failed: " + String(err));
    return false;
  }

  err = i2s_set_adc_mode(ADC_UNIT_1, MIC_ADC_CHANNEL);
  if (err != ESP_OK) {
    logMsg("[I2S] ADC mode failed: " + String(err));
    return false;
  }

  logMsg("[I2S] DMA audio ready.");
  return true;
}

void i2sStart() {
  i2s_adc_enable(I2S_NUM_0);
  delay(100);
  size_t discard;
  i2s_read(I2S_NUM_0, i2sRawBuf, I2S_READ_BYTES, &discard, pdMS_TO_TICKS(500));
}

void i2sStop() {
  i2s_adc_disable(I2S_NUM_0);
}

// ============================================
//  SD CARD (with retry logic)
// ============================================
bool sdInitialized = false;

void sdClockPulses() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(200);

  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 40; i++) {
    SPI.transfer(0xFF);
  }
  SPI.endTransaction();
  delay(100);
}

bool initSD() {
  logMsg("[SD] ========== INIT START ==========");
  logMsg("[SD] Heap: " + String(ESP.getFreeHeap()));

  // Try each speed with multiple clean attempts
  // No manual CMD0 — let SD.begin() handle the full init sequence cleanly
  uint32_t speeds[] = {4000000, 2000000, 1000000, 400000};
  const char* speedLabels[] = {"4 MHz", "2 MHz", "1 MHz", "400 kHz"};

  for (int s = 0; s < 4; s++) {
    logMsg("[SD] --- Speed: " + String(speedLabels[s]) + " ---");

    for (int a = 1; a <= 5; a++) {
      // Full teardown
      SD.end();
      SPI.end();

      // Force card state machine reset via CS toggle
      pinMode(SD_CS, OUTPUT);
      digitalWrite(SD_CS, LOW);
      delay(50);
      digitalWrite(SD_CS, HIGH);
      delay(300 + a * 200);

      // Set MISO pull-up before SPI takes the pin
      pinMode(19, INPUT_PULLUP);

      SPI.begin(18, 19, 23, SD_CS);
      sdClockPulses();

      logMsg("[SD] Attempt " + String(a) + "/5 at " + String(speedLabels[s]));

      if (SD.begin(SD_CS, SPI, speeds[s])) {
        uint8_t cardType = SD.cardType();
        String typeStr = "UNKNOWN";
        if (cardType == CARD_MMC) typeStr = "MMC";
        else if (cardType == CARD_SD) typeStr = "SD";
        else if (cardType == CARD_SDHC) typeStr = "SDHC";
        else if (cardType == CARD_NONE) typeStr = "NONE";
        logMsg("[SD] OK at " + String(speedLabels[s]) +
               " attempt " + String(a) + " | Type: " + typeStr +
               " | Size: " + String((uint32_t)(SD.cardSize() / (1024 * 1024))) + " MB");
        sdInitialized = true;
        logMsg("[SD] ========== INIT SUCCESS ==========");
        return true;
      }

      logMsg("[SD] SD.begin() failed at " + String(speedLabels[s]));
    }
  }

  logMsg("[SD] ========== INIT FAILED ==========");
  sdInitialized = false;
  return false;
}

bool reinitSD() {
  logMsg("[SD] Reinitializing...");
  SD.end();
  SPI.end();
  delay(1000);
  return initSD();
}

size_t sdWriteWithRetry(File &f, uint8_t* data, size_t len) {
  size_t written = f.write(data, len);
  if (written == len) return written;

  // Retry once
  delay(10);
  written = f.write(data, len);
  return written;
}

uint32_t scanExistingFiles() {
  uint32_t maxNum = 0;
  File root = SD.open("/");
  if (!root) return 0;
  File entry = root.openNextFile();
  while (entry) {
    String name = String(entry.name());
    int idx = name.lastIndexOf("rec_");
    if (idx >= 0 && name.endsWith(".wav")) {
      String numPart = name.substring(idx + 4, idx + 10);
      uint32_t num = (uint32_t)numPart.toInt();
      if (num > maxNum) maxNum = num;
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  return maxNum;
}

bool testSDWrite() {
  logMsg("[SD] Write test...");
  File tf = SD.open("/test.tmp", FILE_WRITE);
  if (!tf) {
    logMsg("[SD] FAIL - cannot create file.");
    return false;
  }
  uint8_t buf[512];
  memset(buf, 0xAA, 512);
  for (int i = 0; i < 4; i++) {
    if (tf.write(buf, 512) != 512) {
      logMsg("[SD] FAIL - write error.");
      tf.close();
      SD.remove("/test.tmp");
      return false;
    }
  }
  tf.flush();
  tf.close();

  File rf = SD.open("/test.tmp", FILE_READ);
  if (!rf || rf.size() != 2048) {
    logMsg("[SD] FAIL - readback error.");
    if (rf) rf.close();
    SD.remove("/test.tmp");
    return false;
  }
  rf.close();
  SD.remove("/test.tmp");
  logMsg("[SD] Write test PASSED.");
  return true;
}

// ============================================
//  RECORDING SESSION
// ============================================
void recordSession() {
  recordingNum++;
  statusState = "RECORDING";

  String wavPath = makeWAVPath();
  String startTime = getTimestamp();

  logMsg("=========================================");
  logMsg("[REC] #" + String(recordingNum) + " | " + startTime);
  logMsg("[REC] File: " + wavPath);
  logMsg("=========================================");

  // Read DHT BEFORE recording — these calls block ~500ms total
  // and would cause I2S DMA overflow if done during audio capture
  float tempSum = 0, humSum = 0;
  int dhtCount = 0;
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    tempSum += t; humSum += h; dhtCount++;
    lastTemp = t; lastHum = h;
    logMsg("[DHT] " + String(t, 1) + "C, " + String(h, 1) + "%");
  }

  // Check SD is working
  if (!sdInitialized) {
    if (!reinitSD()) {
      logMsg("[ERROR] SD not available. Skipping recording.");
      return;
    }
  }

  // Record raw audio to temp file (no WAV header — avoids seek/rewrite)
  File raw = SD.open("/temp.raw", FILE_WRITE);
  if (!raw) {
    logMsg("[ERROR] Cannot create temp file. Trying SD reinit...");
    if (reinitSD()) {
      raw = SD.open("/temp.raw", FILE_WRITE);
    }
    if (!raw) {
      logMsg("[ERROR] Temp file creation failed.");
      return;
    }
  }

  i2sStart();

  uint32_t totalBytes = 0;
  uint32_t writeCount = 0;
  uint32_t writeErrors = 0;
  uint32_t i2sErrors = 0;
  uint32_t i2sZeroReads = 0;

  unsigned long recordStart = millis();
  unsigned long lastLog = recordStart;

  logMsg("[REC] Capturing " + String(RECORD_SECONDS) + " seconds of audio...");
  logMsg("[REC] Free heap: " + String(ESP.getFreeHeap()));

  while ((millis() - recordStart) < ((unsigned long)RECORD_SECONDS * 1000UL)) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, i2sRawBuf, I2S_READ_BYTES, &bytesRead, pdMS_TO_TICKS(500));

    if (err != ESP_OK) { i2sErrors++; continue; }
    if (bytesRead == 0) { i2sZeroReads++; continue; }

    int samplesRead = bytesRead / 2;
    for (int i = 0; i < samplesRead; i++) {
      uint16_t adcVal = i2sRawBuf[i] & 0x0FFF;
      pcmBuf[i] = (int16_t)((adcVal - 2048) << 4);
    }

    size_t pcmBytes = samplesRead * 2;
    size_t written = sdWriteWithRetry(raw, (uint8_t*)pcmBuf, pcmBytes);
    totalBytes += written;
    writeCount++;
    if (written != pcmBytes) {
      writeErrors++;
      if (writeErrors <= 3) {
        logMsg("[ERROR] SD write: " + String(pcmBytes) + " -> " + String(written));
      }
    }

    unsigned long now = millis();
    if ((now - lastLog) >= 15000) {
      unsigned long sec = (now - recordStart) / 1000;
      logMsg("[REC] " + String(sec) + "s/" + String(RECORD_SECONDS) +
             "s | " + String(totalBytes) + " bytes | WrErr:" + String(writeErrors));
      lastLog = now;
    }
  }

  i2sStop();

  unsigned long recordDuration = millis() - recordStart;

  raw.flush();
  raw.close();

  logMsg("[REC] Done. Bytes: " + String(totalBytes) + " | Writes: " + String(writeCount) +
         " | WrErr: " + String(writeErrors) + " | I2SErr: " + String(i2sErrors) +
         " | Time: " + String(recordDuration / 1000) + "s");

  if (totalBytes == 0) {
    logMsg("[ERROR] No audio data captured!");
    SD.remove("/temp.raw");
    return;
  }

  // Compute actual sample rate so WAV plays back at exactly 60 seconds
  uint32_t actualSampleRate = totalBytes / 2 / RECORD_SECONDS;
  if (actualSampleRate < 1000) actualSampleRate = SAMPLE_RATE;

  logMsg("[REC] Actual sample rate: " + String(actualSampleRate) + " Hz");

  // Build final WAV: correct header + copy raw data (no seek needed)
  File wav = SD.open(wavPath.c_str(), FILE_WRITE);
  if (!wav) {
    logMsg("[ERROR] Cannot create WAV file.");
    SD.remove("/temp.raw");
    return;
  }

  writeWAVHeader(wav, totalBytes, actualSampleRate);
  if (wav.position() != 44) {
    logMsg("[ERROR] Header write failed!");
    wav.close();
    SD.remove("/temp.raw");
    return;
  }

  raw = SD.open("/temp.raw", FILE_READ);
  if (!raw) {
    logMsg("[ERROR] Cannot reopen temp file for copy.");
    wav.close();
    SD.remove("/temp.raw");
    return;
  }

  uint32_t copied = 0;
  while (raw.available()) {
    size_t n = raw.read((uint8_t*)i2sRawBuf, I2S_READ_BYTES);
    if (n > 0) {
      wav.write((uint8_t*)i2sRawBuf, n);
      copied += n;
    }
  }
  raw.close();
  wav.flush();
  wav.close();
  SD.remove("/temp.raw");

  logMsg("[WAV] Built: header(44) + data(" + String(copied) + ") = " +
         String(copied + 44) + " bytes");

  // Verify
  File verify = SD.open(wavPath.c_str(), FILE_READ);
  if (verify) {
    size_t fsize = verify.size();
    verify.close();
    size_t expected = totalBytes + 44;
    if (fsize >= expected) {
      logMsg("[VERIFY] OK! " + String(fsize) + " bytes (" +
             String(RECORD_SECONDS) + "s at " + String(actualSampleRate) + " Hz).");
    } else {
      logMsg("[VERIFY] Size mismatch: " + String(fsize) + " / " + String(expected));
    }
  }

  // Read DHT AFTER recording
  t = dht.readTemperature();
  h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    tempSum += t; humSum += h; dhtCount++;
    lastTemp = t; lastHum = h;
  }

  float avgTemp = (dhtCount > 0) ? (tempSum / dhtCount) : 0.0;
  float avgHum = (dhtCount > 0) ? (humSum / dhtCount) : 0.0;

  // CSV
  String ts = getTimestamp();
  File csv = SD.open("/beesense.csv", FILE_APPEND);
  if (csv) {
    csv.print(ts); csv.print(",");
    csv.print(avgTemp, 2); csv.print(",");
    csv.print(avgHum, 2); csv.print(",");
    csv.println(wavPath);
    csv.flush();
    csv.close();
    logMsg("[CSV] Logged.");
  } else {
    logMsg("[CSV] Write failed!");
  }

  lastRecFile = wavPath;
  lastRecTime = ts;

  // Upload to cloud
  bool wavUploaded = false;
  bool dataUploaded = false;
  if (WiFi.status() != WL_CONNECTED) {
    logMsg("[CLOUD] WiFi disconnected. Reconnecting...");
    connectWiFi();
  }
  if (WiFi.status() == WL_CONNECTED) {
    // WAV upload first (large file, needs clean heap)
    for (int attempt = 1; attempt <= 2 && !wavUploaded; attempt++) {
      if (attempt > 1) {
        logMsg("[CLOUD] WAV retry...");
        delay(3000);
        if (WiFi.status() != WL_CONNECTED) connectWiFi();
      }
      wavUploaded = uploadWAVToCloud(wavPath);
    }

    // Sensor data (small JSON, always works)
    dataUploaded = uploadSensorData(ts, avgTemp, avgHum, wavPath, actualSampleRate);
  } else {
    logMsg("[CLOUD] No WiFi. Data saved to SD card only.");
  }

  logMsg("========== SUMMARY ==========");
  logMsg("  Time:     " + ts);
  logMsg("  File:     " + wavPath);
  logMsg("  Rate:     " + String(actualSampleRate) + " Hz");
  logMsg("  Duration: " + String(recordDuration / 1000) + "s wall-clock");
  logMsg("  Writes:   " + String(writeCount) + " | Errors: " + String(writeErrors));
  logMsg("  Size:     " + String(totalBytes) + " bytes (" + String(totalBytes / 1024) + " KB)");
  logMsg("  Temp:     " + String(avgTemp, 2) + " C");
  logMsg("  Humid:    " + String(avgHum, 2) + " %");
  logMsg("  Cloud:    WAV " + String(wavUploaded ? "OK" : ("FAIL(" + String(lastWavHttpCode) + ")")) +
         " | Data " + String(dataUploaded ? "OK" : "FAIL"));
  logMsg("  Heap:     " + String(ESP.getFreeHeap()) + " bytes free");
  logMsg("=============================");

  // Upload logs to cloud so dashboard can see them
  uploadLogs();
}

// ============================================
//  SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  logMsg("====================================");
  logMsg("  BeeSense v5.0 — Cloud Edition");
  logMsg("  Device: " + String(DEVICE_ID));
  logMsg("====================================");

  // --- DHT ---
  dht.begin();
  logMsg("[OK] DHT ready.");

  // --- SD Card FIRST (before WiFi — WiFi can interfere with SPI) ---
  logMsg("[SD] Initializing...");
  if (!initSD()) {
    logMsg("[WARN] SD failed on boot. Will retry before recording.");
    statusState = "SD ERROR";
  } else {
    logMsg("[SD] Card: " + String((uint32_t)(SD.cardSize() / (1024 * 1024))) + " MB");
    logMsg("[SD] Used: " + String((uint32_t)(SD.usedBytes() / (1024 * 1024))) +
           " / " + String((uint32_t)(SD.totalBytes() / (1024 * 1024))) + " MB");

    if (!testSDWrite()) {
      logMsg("[WARN] SD write test failed. Will retry.");
      sdInitialized = false;
    }

    if (!SD.exists("/beesense.csv")) {
      File f = SD.open("/beesense.csv", FILE_WRITE);
      if (f) {
        f.println("Datetime,Avg_Temperature_C,Avg_Humidity_percent,WAV_File");
        f.flush(); f.close();
        logMsg("[OK] CSV created.");
      }
    }
  }

  // --- I2S audio ---
  if (!i2sInit()) {
    logMsg("[ERROR] I2S failed!");
  }

  // --- WiFi (station mode) ---
  if (connectWiFi()) {
    syncNTP();
  }

  // --- Scan existing files if no NTP ---
  if (!ntpSynced && sdInitialized) {
    fileCounter = scanExistingFiles();
    logMsg("[LOG] Existing files: " + String(fileCounter));
  }

  logMsg("");
  logMsg("====================================");
  logMsg("  READY | " + getTimestamp());
  logMsg("  WAV: " + String((EXPECTED_DATA_BYTES + 44) / 1024) + " KB each");
  logMsg("  Sample rate: " + String(SAMPLE_RATE) + " Hz");
  logMsg("  Cloud: " + String(API_ENDPOINT));
  logMsg("  First recording starts NOW.");
  logMsg("====================================");
  logMsg("");

  statusState = "STARTING";
}

// ============================================
//  LOOP
// ============================================
void loop() {
  unsigned long cycleStart = millis();

  // --- SD retry if it failed during boot ---
  if (!sdInitialized) {
    logMsg("[SD] Retrying initialization...");
    if (initSD() && testSDWrite()) {
      if (!SD.exists("/beesense.csv")) {
        File f = SD.open("/beesense.csv", FILE_WRITE);
        if (f) {
          f.println("Datetime,Avg_Temperature_C,Avg_Humidity_percent,WAV_File");
          f.flush(); f.close();
        }
      }
    } else {
      logMsg("[SD] Still failing. Will retry next cycle.");
      statusState = "SD ERROR";
      delay(60000);
      return;
    }
  }

  // --- Ensure WiFi + NTP before recording ---
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!ntpSynced && WiFi.status() == WL_CONNECTED) {
    syncNTP();
  }

  recordSession();

  // --- Wait for next interval ---
  unsigned long elapsed = millis() - cycleStart;
  unsigned long intervalMs = (unsigned long)INTERVAL_MINUTES * 60UL * 1000UL;

  if (elapsed < intervalMs) {
    statusState = "WAITING";
    unsigned long waitMs = intervalMs - elapsed;
    unsigned long waitMin = waitMs / 60000UL;

    logMsg("");
    logMsg("=========================================");
    if (ntpSynced) {
      struct tm ti;
      if (getLocalTime(&ti)) {
        time_t nextTime = mktime(&ti) + (waitMs / 1000);
        struct tm* nextTm = localtime(&nextTime);
        char buf[25];
        strftime(buf, sizeof(buf), "%H:%M:%S", nextTm);
        logMsg("[WAIT] Next recording at " + String(buf));
      }
    } else {
      logMsg("[WAIT] Next recording in " + String(waitMin) + " minutes.");
    }
    logMsg("=========================================");

    unsigned long lastCountdown = waitMin + 1;
    unsigned long lastLogUpload = 0;

    while ((millis() - cycleStart) < intervalMs) {
      unsigned long remaining = intervalMs - (millis() - cycleStart);
      unsigned long remainMin = remaining / 60000UL;

      if (remainMin < lastCountdown) {
        lastCountdown = remainMin;
        if (remainMin > 0) {
          logMsg("[COUNTDOWN] " + String(remainMin) + " min | " + getTimestamp());
        } else {
          logMsg("[COUNTDOWN] < 1 min | Starting soon...");
        }
      }

      // Reconnect WiFi if it dropped during the wait
      if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
      }

      // Upload logs to cloud every 30 seconds for live dashboard view
      if (WiFi.status() == WL_CONNECTED && (millis() - lastLogUpload) >= 30000) {
        uploadLogs();
        lastLogUpload = millis();
      }

      delay(5000);
    }
    logMsg("");
  }
}
