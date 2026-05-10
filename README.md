# BeeSense

[![Platform](https://img.shields.io/badge/platform-ESP32-blue)](https://www.espressif.com/en/products/socs/esp32)
[![Cloud](https://img.shields.io/badge/cloud-Cloudflare%20R2-F38020)](https://developers.cloudflare.com/r2/)
[![Dashboard](https://img.shields.io/badge/dashboard-Next.js%2016-000000)](https://nextjs.org/)
[![License](https://img.shields.io/badge/license-Capstone%20Project-green)]()

An IoT-based beehive monitoring system that records audio from beehives, measures temperature and humidity, stores everything locally on an SD card **and** uploads to Cloudflare R2 cloud storage. A web dashboard provides real-time monitoring and data export for ML model training.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Hardware](#hardware)
  - [Components](#components)
  - [Pin Connections](#pin-connections)
  - [Wiring Diagram](#wiring-diagram)
- [Project Structure](#project-structure)
- [Firmware](#firmware)
  - [Configuration Constants](#configuration-constants)
  - [Recording Behavior](#recording-behavior)
- [Cloud API](#cloud-api)
  - [Endpoints](#endpoints)
  - [Sensor Data Schema](#sensor-data-schema)
- [Dashboard](#dashboard)
- [Setup Guide](#setup-guide)
  - [1. Cloudflare R2 and Worker](#1-cloudflare-r2-and-worker)
  - [2. Dashboard](#2-dashboard)
  - [3. Firmware](#3-firmware)
- [Environment Variables](#environment-variables)
- [Data Flow](#data-flow)

---

## Overview

BeeSense is a capstone project designed to continuously monitor beehive conditions. The system captures 60-second audio recordings at regular intervals alongside temperature and humidity readings. All data is stored on a local SD card for resilience and simultaneously uploaded to Cloudflare R2 cloud storage for remote access. A Next.js web dashboard displays sensor trends in real time and supports CSV/JSON export for machine learning model training.

---

## Architecture

```
+----------+        WiFi         +--------------------+       R2        +-----------------+
|          | ----- WAV upload -> |                    | --- store --->  |                 |
|  ESP32   |                     |  Cloudflare Worker |                 |  R2 Bucket      |
|          | -- sensor JSON -->  |                    | <-- read -----  | "beesense-data" |
+----+-----+                     +--------+-----------+                 +-----------------+
     |                                    |
     | SD card                            | API
     | (local backup)                     |
     v                                    v
+----------+                     +------------------+
| MicroSD  |                     | Next.js          |
| Card     |                     | Dashboard        |
+----------+                     | (auto-refresh    |
                                 |  every 30s)      |
                                 +------------------+
```

**Flow:** ESP32 records 60 seconds of audio every 10 minutes, saves the WAV file to the SD card, then uploads the WAV and a sensor data JSON payload to the Cloudflare Worker. The Worker stores both in an R2 bucket. The dashboard reads from the same Worker API and auto-refreshes every 30 seconds.

---

## Hardware

### Components

| Component | Purpose |
|---|---|
| ESP32 microcontroller | Main processing unit, WiFi connectivity |
| INMP441 / MAX9814 microphone module | Beehive audio capture via I2S ADC |
| DHT11 sensor | Temperature and humidity measurement |
| MicroSD card module (SPI) | Local data storage and backup |
| USB cable or 5V power supply | Power |

### Pin Connections

| Signal | GPIO Pin | Notes |
|---|---|---|
| Microphone ADC input | GPIO34 | ADC1_CHANNEL_6, I2S DMA capture |
| DHT11 data | GPIO4 | Digital temperature/humidity |
| SD card CS | GPIO5 | SPI chip select |
| SD card SCK | GPIO18 | SPI clock |
| SD card MISO | GPIO19 | SPI data out |
| SD card MOSI | GPIO23 | SPI data in |

### Wiring Diagram

```
ESP32 DevKit
 +-----------+
 | GPIO34    |<--- Microphone OUT (INMP441 / MAX9814)
 | GPIO4     |<--- DHT11 DATA (with 10k pull-up to 3.3V)
 |           |
 | GPIO5     |---> SD Module CS
 | GPIO18    |---> SD Module SCK
 | GPIO19    |<--- SD Module MISO
 | GPIO23    |---> SD Module MOSI
 |           |
 | 3.3V      |---> DHT11 VCC, Mic VCC
 | 5V        |---> SD Module VCC
 | GND       |---> All GND
 +-----------+
```

---

## Project Structure

```
BeeSense/
|-- firmware/
|   +-- BeeSense.ino              ESP32 Arduino firmware (v5.0)
|
|-- cloudflare-worker/
|   |-- worker.js                 Cloudflare Worker API (R2 storage backend)
|   +-- wrangler.toml             Worker deployment configuration
|
+-- dashboard/                    Next.js 16 web dashboard
    |-- app/
    |   |-- layout.tsx            Root layout
    |   |-- page.tsx              Main monitoring dashboard
    |   +-- recordings/
    |       +-- page.tsx          Recordings browser
    |-- package.json
    +-- ...
```

---

## Firmware

The firmware (`firmware/BeeSense.ino`) runs on the ESP32 and handles the full data acquisition pipeline: WiFi connection, NTP time sync, I2S DMA audio capture, DHT sensor reads, SD card storage with retry logic, and cloud upload via HTTPS.

### Configuration Constants

| Constant | Default Value | Description |
|---|---|---|
| `SAMPLE_RATE` | 16000 Hz | Audio sample rate (16 kHz) |
| `RECORD_SECONDS` | 60 | Duration of each recording |
| `INTERVAL_MINUTES` | 10 | Minutes between recording sessions |
| `DEVICE_ID` | `"beesense-01"` | Unique identifier for this device |
| `WIFI_SSID` | *(must be set)* | WiFi network name |
| `WIFI_PASS` | *(must be set)* | WiFi password |
| `API_ENDPOINT` | *(must be set)* | Cloudflare Worker URL |
| `API_KEY` | *(must be set)* | Shared secret for API auth |
| `GMT_OFFSET_SEC` | 19800 | Timezone offset in seconds (IST = UTC+5:30) |

### Recording Behavior

1. ESP32 boots, initializes SD card, connects to WiFi, and syncs time via NTP.
2. Every 10 minutes, a recording session begins:
   - DHT11 sensor is read **before** audio capture (the DHT blocks for ~500ms and would cause I2S DMA overflow during recording).
   - I2S DMA captures 60 seconds of 16-bit mono PCM audio at 16 kHz.
   - Audio is written to the SD card as a standard WAV file.
   - DHT11 is read again **after** recording; the two readings are averaged.
   - A CSV log entry is appended to `/beesense.csv` on the SD card.
   - The WAV file is uploaded to the cloud via HTTPS PUT.
   - A sensor data JSON payload is uploaded via HTTPS POST.
3. If WiFi is unavailable, data is preserved on the SD card.
4. If the SD card fails, the firmware retries initialization with multiple SPI speeds (4 MHz, 2 MHz, 1 MHz) and up to 7 attempts per speed.

---

## Cloud API

The Cloudflare Worker (`cloudflare-worker/worker.js`) acts as the API layer between the ESP32, the R2 storage bucket, and the dashboard. Write endpoints (PUT/POST) require Bearer token authentication. Read endpoints (GET) are open for the dashboard.

### Endpoints

| Method | Path | Auth | Used By | Description |
|---|---|---|---|---|
| `PUT` | `/api/upload/:filename` | Bearer token | ESP32 | Upload a WAV audio file |
| `POST` | `/api/sensor-data` | Bearer token | ESP32 | Store a sensor reading (JSON) |
| `GET` | `/api/files?device=:id` | None | Dashboard | List WAV files for a device |
| `GET` | `/api/sensor-data?device=:id&limit=:n` | None | Dashboard | Get recent sensor readings |
| `GET` | `/api/download/:key` | None | Dashboard | Download a file from R2 |
| `GET` | `/api/export/csv?device=:id` | None | Dashboard | Export all sensor data as CSV |
| `GET` | `/api/export/json?device=:id` | None | Dashboard | Export all sensor data as JSON |
| `GET` | `/api/health` | None | Dashboard | Health check |

### Sensor Data Schema

Each sensor reading is stored as a JSON object in R2:

```json
{
  "device_id": "beesense-01",
  "timestamp": "2025-01-15 14:30:45",
  "temperature": 28.50,
  "humidity": 65.20,
  "wav_file": "/rec_20250115_143045.wav",
  "sample_rate": 16000
}
```

---

## Dashboard

The web dashboard is built with Next.js 16, shadcn/ui components, and Recharts for data visualization. It provides:

- **Real-time sensor monitoring** -- temperature and humidity trends displayed as interactive charts.
- **Recordings browser** -- browse and download WAV files stored in the cloud.
- **Auto-refresh** -- the dashboard polls the API every 30 seconds for new data.
- **Data export** -- export sensor history as CSV or JSON for ML model training.

Key dependencies: `next@16`, `react@19`, `recharts`, `shadcn`, `lucide-react`, `date-fns`.

---

## Setup Guide

### 1. Cloudflare R2 and Worker

1. **Create the R2 bucket.** Log in to the [Cloudflare dashboard](https://dash.cloudflare.com/), navigate to R2, and create a bucket named `beesense-data`. If you choose a different name, update `bucket_name` in `cloudflare-worker/wrangler.toml`.

2. **Choose an API key.** Pick a strong secret string. This same key will be used in the Worker secret and in the ESP32 firmware.

3. **Deploy the Worker:**

   ```bash
   cd cloudflare-worker
   npx wrangler deploy
   ```

4. **Set the API_KEY secret:**

   ```bash
   npx wrangler secret put API_KEY
   ```

   Enter your chosen secret when prompted. Alternatively, set it via the Cloudflare dashboard under **Workers & Pages > beesense-api > Settings > Variables and Secrets**.

5. **Note the Worker URL.** After deployment, Wrangler will print the URL (e.g., `https://beesense-api.your-subdomain.workers.dev`). You will need this for both the dashboard and the firmware.

### 2. Dashboard

1. **Install dependencies:**

   ```bash
   cd dashboard
   npm install
   ```

2. **Configure environment variables.** Copy `.env.example` to `.env.local` and fill in the values:

   ```env
   NEXT_PUBLIC_API_URL=https://beesense-api.your-subdomain.workers.dev
   NEXT_PUBLIC_DEVICE_ID=beesense-01
   ```

3. **Run locally:**

   ```bash
   npm run dev
   ```

   The dashboard will be available at `http://localhost:3000`.

4. **Production deployment.** Deploy to [Vercel](https://vercel.com/) or any hosting platform that supports Next.js. Set the same environment variables in your hosting provider's dashboard.

### 3. Firmware

1. **Open** `firmware/BeeSense.ino` in the [Arduino IDE](https://www.arduino.cc/en/software).

2. **Install the ESP32 board package.** In Arduino IDE, go to **Tools > Board > Boards Manager**, search for **"ESP32 by Espressif Systems"**, and install it.

3. **Install required libraries.** In Arduino IDE, go to **Tools > Manage Libraries** and install:
   - **DHT sensor library** (by Adafruit)

4. **Update configuration constants** at the top of `BeeSense.ino`:

   ```cpp
   const char* WIFI_SSID     = "YourWiFiNetwork";
   const char* WIFI_PASS     = "YourWiFiPassword";
   const char* API_ENDPOINT  = "https://beesense-api.your-subdomain.workers.dev";
   const char* API_KEY       = "your-secret-api-key";
   const char* DEVICE_ID     = "beesense-01";
   ```

   Make sure `API_KEY` matches the secret you set in the Cloudflare Worker.

5. **Select the board.** In Arduino IDE, go to **Tools > Board** and select **ESP32 Dev Module** (or your specific ESP32 variant).

6. **Flash.** Connect the ESP32 via USB and click **Upload**. Open the Serial Monitor at 115200 baud to verify boot messages.

---

## Environment Variables

A summary of all configurable values across the three components:

| Component | Variable | Where to Set | Description |
|---|---|---|---|
| Cloudflare Worker | `API_KEY` | `wrangler secret put` or Cloudflare dashboard | Shared secret for ESP32 auth |
| Dashboard | `NEXT_PUBLIC_API_URL` | `.env.local` | Full URL of the Cloudflare Worker |
| Dashboard | `NEXT_PUBLIC_DEVICE_ID` | `.env.local` | Device ID to query (default: `beesense-01`) |
| Firmware | `WIFI_SSID` | `BeeSense.ino` | WiFi network name |
| Firmware | `WIFI_PASS` | `BeeSense.ino` | WiFi password |
| Firmware | `API_ENDPOINT` | `BeeSense.ino` | Full URL of the Cloudflare Worker |
| Firmware | `API_KEY` | `BeeSense.ino` | Must match the Worker secret |
| Firmware | `DEVICE_ID` | `BeeSense.ino` | Unique device identifier |

---

## Data Flow

```
1. ESP32 powers on
   |
   +-> Initializes SD card (SPI: CS=5, SCK=18, MISO=19, MOSI=23)
   +-> Connects to WiFi
   +-> Syncs time via NTP
   |
2. Every 10 minutes:
   |
   +-> Reads DHT11 (temperature + humidity)
   +-> Captures 60s audio via I2S ADC at 16 kHz (GPIO34)
   +-> Writes WAV file to SD card
   +-> Reads DHT11 again, averages both readings
   +-> Appends CSV log to /beesense.csv on SD
   |
   +-> Uploads WAV to Worker:  PUT /api/upload/<filename>
   +-> Uploads sensor JSON:    POST /api/sensor-data
   |
3. Cloudflare Worker stores to R2 bucket "beesense-data"
   |
   +-> WAV files at:    wav/<device_id>/<filename>
   +-> Sensor data at:  data/<device_id>/<timestamp>.json
   |
4. Dashboard polls Worker API every 30 seconds
   |
   +-> GET /api/sensor-data  -> renders charts
   +-> GET /api/files        -> lists recordings
   +-> GET /api/download     -> plays/downloads WAV
   +-> GET /api/export/csv   -> exports for ML training
```
