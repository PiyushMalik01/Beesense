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
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Cloud API (Cloudflare Worker endpoint for R2 storage)
const char* API_ENDPOINT = "https://beesense-api.YOUR_SUBDOMAIN.workers.dev";
const char* API_KEY = "YOUR_SECRET_API_KEY";
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
WiFiClientSecure secureClient;

bool ntpSynced = false;
uint32_t fileCounter = 0;

uint16_t i2sRawBuf[I2S_READ_SAMPLES];
int16_t pcmBuf[I2S_READ_SAMPLES];

// Log ring buffer (viewable via WiFi)
String logBuffer[LOG_BUFFER_SIZE];
int logHead = 0;
int logCount = 0;

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
  if (WiFi.status() != WL_CONNECTED) {
    logMsg("[CLOUD] No WiFi, skipping WAV upload.");
    return false;
  }

  File wavFile = SD.open(filePath.c_str(), FILE_READ);
  if (!wavFile) {
    logMsg("[CLOUD] Cannot open " + filePath);
    return false;
  }

  size_t fileSize = wavFile.size();
  String filename = filePath;
  if (filename.startsWith("/")) filename = filename.substring(1);

  String url = String(API_ENDPOINT) + "/api/upload/" + filename;

  HTTPClient http;
  http.begin(secureClient, url);
  http.addHeader("Authorization", "Bearer " + String(API_KEY));
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Device-Id", String(DEVICE_ID));
  http.setTimeout(120000);

  logMsg("[CLOUD] Uploading " + filename + " (" + String(fileSize / 1024) + " KB)...");

  int httpCode = http.sendRequest("PUT", &wavFile, fileSize);
  wavFile.close();
  http.end();

  if (httpCode == 200 || httpCode == 201) {
    logMsg("[CLOUD] WAV upload OK.");
    return true;
  }

  logMsg("[CLOUD] WAV upload failed: HTTP " + String(httpCode));
  return false;
}

bool uploadSensorData(String timestamp, float temp, float humidity,
                      String wavFile, uint32_t sampleRate) {
  if (WiFi.status() != WL_CONNECTED) {
    logMsg("[CLOUD] No WiFi, skipping data upload.");
    return false;
  }

  HTTPClient http;
  http.begin(secureClient, String(API_ENDPOINT) + "/api/sensor-data");
  http.addHeader("Authorization", "Bearer " + String(API_KEY));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  String json = "{";
  json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  json += "\"timestamp\":\"" + timestamp + "\",";
  json += "\"temperature\":" + String(temp, 2) + ",";
  json += "\"humidity\":" + String(humidity, 2) + ",";
  json += "\"wav_file\":\"" + wavFile + "\",";
  json += "\"sample_rate\":" + String(sampleRate);
  json += "}";

  int httpCode = http.POST(json);
  http.end();

  if (httpCode == 200 || httpCode == 201) {
    logMsg("[CLOUD] Sensor data uploaded.");
    return true;
  }

  logMsg("[CLOUD] Sensor data failed: HTTP " + String(httpCode));
  return false;
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

// Send 80+ dummy clock pulses with CS HIGH — required by SD spec
// to bring card from any unknown state to SPI mode ready
void sdClockReset() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 16; i++) {
    SPI.transfer(0xFF);
  }
  SPI.endTransaction();
  delay(50);
}

bool initSD() {
  logMsg("[SD] Starting init sequence...");

  uint32_t speeds[] = {4000000, 2000000, 1000000};

  for (int s = 0; s < 3; s++) {
    logMsg("[SD] Speed: " + String(speeds[s] / 1000000) + " MHz");

    for (int a = 1; a <= 7; a++) {
      // Full reset: tear down SPI, rebuild, send clock pulses
      SD.end();
      SPI.end();
      delay(200);

      SPI.begin(18, 19, 23, SD_CS);
      sdClockReset();

      logMsg("[SD]   Attempt " + String(a) + "...");

      if (SD.begin(SD_CS, SPI, speeds[s])) {
        logMsg("[SD] OK at " + String(speeds[s] / 1000000) + " MHz (attempt " + String(a) + ").");
        sdInitialized = true;
        return true;
      }

      delay(300 + a * 200);  // increasing delay between retries
    }
  }

  logMsg("[SD] All attempts failed.");
  sdInitialized = false;
  return false;
}

bool reinitSD() {
  logMsg("[SD] Reinitializing...");
  SD.end();
  SPI.end();
  delay(500);
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

  File wav = SD.open(wavPath.c_str(), FILE_WRITE);
  if (!wav) {
    logMsg("[ERROR] Cannot create WAV. Trying SD reinit...");
    if (reinitSD()) {
      wav = SD.open(wavPath.c_str(), FILE_WRITE);
    }
    if (!wav) {
      logMsg("[ERROR] WAV creation failed after reinit.");
      return;
    }
  }

  // Write placeholder header (will be updated after recording with actual values)
  writeWAVHeader(wav, EXPECTED_DATA_BYTES, SAMPLE_RATE);
  if (wav.position() != 44) {
    logMsg("[ERROR] Header write failed!");
    wav.close();
    return;
  }
  logMsg("[OK] WAV file created.");

  // Start I2S
  i2sStart();

  uint32_t totalBytes = 0;
  uint32_t writeCount = 0;
  uint32_t writeErrors = 0;

  unsigned long recordStart = millis();
  unsigned long lastLog = recordStart;

  logMsg("[REC] Capturing " + String(RECORD_SECONDS) + " seconds of audio...");

  // ========================
  //  MAIN RECORDING LOOP
  //  Pure audio capture — no DHT, no WiFi, no flush.
  //  These operations block for hundreds of milliseconds and cause
  //  I2S DMA buffer overflow, losing audio samples.
  // ========================
  while ((millis() - recordStart) < ((unsigned long)RECORD_SECONDS * 1000UL)) {

    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, i2sRawBuf, I2S_READ_BYTES, &bytesRead, pdMS_TO_TICKS(500));

    if (err != ESP_OK || bytesRead == 0) {
      continue;
    }

    // Convert raw ADC to signed 16-bit PCM
    int samplesRead = bytesRead / 2;
    for (int i = 0; i < samplesRead; i++) {
      uint16_t adcVal = i2sRawBuf[i] & 0x0FFF;
      pcmBuf[i] = (int16_t)((adcVal - 2048) << 4);
    }

    // Write to SD
    size_t pcmBytes = samplesRead * 2;
    size_t written = sdWriteWithRetry(wav, (uint8_t*)pcmBuf, pcmBytes);
    totalBytes += written;
    writeCount++;
    if (written != pcmBytes) {
      writeErrors++;
      if (writeErrors <= 3) {
        logMsg("[ERROR] SD write: " + String(pcmBytes) + " -> " + String(written));
      }
    }

    // Brief progress every ~15 seconds (Serial.println is fast, no DMA risk)
    unsigned long now = millis();
    if ((now - lastLog) >= 15000) {
      unsigned long sec = (now - recordStart) / 1000;
      logMsg("[REC] " + String(sec) + "s/" + String(RECORD_SECONDS) +
             "s | " + String(totalBytes / 1024) + " KB | Err: " + String(writeErrors));
      lastLog = now;
    }
  }

  // Stop I2S
  i2sStop();

  unsigned long recordDuration = millis() - recordStart;

  if (totalBytes == 0) {
    logMsg("[ERROR] No audio data captured!");
    wav.close();
    SD.remove(wavPath.c_str());
    return;
  }

  // Compute actual sample rate from the data we captured.
  // If I2S delivered more or fewer samples than expected, this ensures
  // the WAV plays back at real-time speed (60s of audio = 60s playback).
  uint32_t actualSampleRate = totalBytes / 2 / RECORD_SECONDS;
  if (actualSampleRate < 1000) actualSampleRate = SAMPLE_RATE;

  logMsg("[REC] Captured " + String(totalBytes) + " bytes in " +
         String(recordDuration / 1000) + "s");
  logMsg("[REC] Effective sample rate: " + String(actualSampleRate) +
         " Hz (configured: " + String(SAMPLE_RATE) + " Hz)");

  // Rewrite WAV header with actual data size and computed sample rate
  wav.seek(0);
  writeWAVHeader(wav, totalBytes, actualSampleRate);

  wav.flush();
  wav.close();

  // Verify
  File verify = SD.open(wavPath.c_str(), FILE_READ);
  if (verify) {
    size_t fsize = verify.size();
    verify.close();
    size_t expected = totalBytes + 44;
    if (fsize >= expected) {
      logMsg("[VERIFY] OK! " + String(fsize) + " bytes (" +
             String(RECORD_SECONDS) + "s audio at " +
             String(actualSampleRate) + " Hz).");
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

  // Upload to cloud (file stays on SD card regardless of upload result)
  bool wavUploaded = false;
  bool dataUploaded = false;
  if (WiFi.status() != WL_CONNECTED) {
    logMsg("[CLOUD] WiFi disconnected. Reconnecting...");
    connectWiFi();
  }
  if (WiFi.status() == WL_CONNECTED) {
    wavUploaded = uploadWAVToCloud(wavPath);
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
  logMsg("  Size:     " + String(totalBytes / 1024) + " KB");
  logMsg("  Temp:     " + String(avgTemp, 2) + " C");
  logMsg("  Humid:    " + String(avgHum, 2) + " %");
  logMsg("  Cloud:    WAV " + String(wavUploaded ? "OK" : "FAIL") +
         " | Data " + String(dataUploaded ? "OK" : "FAIL"));
  logMsg("=============================");
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
  secureClient.setInsecure();
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
    if (!ntpSynced && WiFi.status() == WL_CONNECTED) {
      syncNTP();
    }
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

      delay(5000);
    }
    logMsg("");
  }
}
