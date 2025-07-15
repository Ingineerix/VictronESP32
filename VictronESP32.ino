#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "mbedtls/aes.h"

WiFiClient clientNode;
WiFiUDP wifiUDPServer;
Telnet LOG;
BLEScan *pBLEScan;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

const char* stssid = "<YOUR SSID>";
const char* stkey = "<YOUR PASS>";
const char* apssid = "ESP32-Victron";
const char* apkey = "localnet";

const char* otaPassword = "OTAPASS";

#define BATT_KWH 9.0           // Size of your battery in kWh
#define BATT_MAX_WATTS 1500    // Max expected battery charge/discharge for analog gauge
#define SOLAR_MAX_WATTS 1800   // Max Total Wattage you expect to get in most cases
#define DATA_MINS 840          // 14 hours of data - Needs to be a multiple of 120!
#define led         2          // GPIO for LED

#define STRING(x) #x
#define STRINGIFY(x) STRING(x)

// Daily solar data storage - using minutes since first light
uint16_t dailySolarData[DATA_MINS];
int currentDataIndex = 0;
unsigned long lastDataSave = 0;
unsigned long lastMinuteUpdate = 0;
const unsigned long SAVE_INTERVAL = 60000; // Save every minute
const unsigned long FLASH_SAVE_INTERVAL = 300000; // Save every 5 minutes (300,000ms)
const unsigned long MINUTE_INTERVAL = 60000; // Update data every minute
bool sessionActive = false;
bool allSolarOff = true;
bool printData = 0;
unsigned long sessionStartTime = 0;
unsigned long lastFlashSave = 0;
bool dataChangedSinceLastSave = false;
bool otaInProgress = false;
bool rebootRequested = false;

const uint8_t keys[4][16] = {
// LEFT:
    {0xba, 0xf1, 0x11, 0xd9, 0xcf, 0xab, 0x90, 0x05,
     0x65, 0x33, 0xe7, 0xdb, 0x9f, 0x15, 0xed, 0xc7},
// RIGHT:    
    {0x87, 0x93, 0xc2, 0x38, 0xf1, 0x5b, 0x81, 0x0d,
     0xca, 0x0d, 0xda, 0xd7, 0x2c, 0x63, 0x8b, 0x84},
// REAR:    
    {0xfd, 0xe3, 0x13, 0xd9, 0x65, 0xa8, 0x79, 0x75,
     0xe5, 0x04, 0x53, 0x16, 0x73, 0x07, 0x52, 0x8a},    
// BMV:
    {0x23, 0x84, 0x33, 0xa7, 0x66, 0x62, 0xf5, 0xa4,
     0x42, 0x5c, 0x7c, 0xd2, 0xa9, 0x59, 0xbe, 0xf1},
};

const char* deviceNames[] = {"LEFT", "RIGHT", "REAR", "BATT"};
const char* statTxt[] = {"OFF", " 1? ", " 2? ", "BULK", "ABSRB", "FLOAT", " 6? ", "EQLIZ", "INIT"};

// Device data structures
struct SolarData {
  String name;
  float voltage;
  float current;
  uint16_t power;
  float todayYield;
  uint8_t state;
  unsigned long lastUpdate;
  bool valid;
};

struct BatteryData {
  String name;
  float voltage;
  float current;
  float power;
  float ampHours;
  float soc;
  uint16_t timeToGo;
  unsigned long lastUpdate;
  bool valid;
};

// Storage for device data
SolarData solarDevices[3]; // LEFT, RIGHT, REAR
BatteryData batteryDevice;
unsigned long lastDisplayUpdate = 0;
unsigned long lastWebSocketUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000; // 1 second
const unsigned long WEBSOCKET_UPDATE_INTERVAL = 1000; // 1 second

int keyBits=128;
int scanTime = 1;
char savedDeviceName[32];
boolean isWifiConnected = false;
boolean isWifiActive = false;
boolean needServerInit = false;
uint8_t cnt = 0;
uint8_t noap = 0;
uint8_t wifiMode;
uint8_t SSID[32];
uint8_t WPA2Key[64];
uint32_t timer;
int32_t RSSI;

typedef struct {
  uint16_t vendorID;
  uint8_t beaconType;
  uint8_t unknownData1[3];
  uint8_t victronRecordType;
  uint16_t nonceDataCounter;
  uint8_t encryptKeyMatch;
  uint8_t victronEncryptedData[21];
  uint8_t nullPad;
} __attribute__((packed)) victronManufacturerData;

typedef struct {
   uint8_t deviceState;
   uint8_t errorCode;
   int16_t batteryVoltage;
   int16_t batteryCurrent;
   uint16_t todayYield;
   uint16_t inputPower;
   uint16_t last3[3];
} __attribute__((packed)) victronPanelData;

typedef struct {
   uint16_t timeToGo;
   int16_t batteryVoltage;
   uint16_t alarmReason;
   int16_t auxVoltage;
   uint64_t packed;
} __attribute__((packed)) victronBattData;

// Web page HTML
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>Solar System</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.ico">
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #1e3c72 0%, #2a5298 100%);
      color: #ffffff;
      margin: 0;
      padding: 20px;
      min-height: 100vh;
    }
    .container {
      max-width: 1200px;
      margin: 0 auto;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 15px;
      padding: 30px;
      backdrop-filter: blur(10px);
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
      gap: 20px;
      margin-bottom: 30px;
    }
    .card {
      background: rgba(255, 255, 255, 0.15);
      border-radius: 12px;
      padding: 25px;
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
      transition: transform 0.3s ease;
    }
    .card:hover {
      transform: translateY(-5px);
    }
    .card h3 {
      margin-top: 0;
      font-size: 1.4rem;
      color: #ffd700;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .metric {
      display: flex;
      justify-content: space-between;
      margin: 10px 0;
      font-size: 1.1rem;
    }
    .value {
      font-weight: bold;
      color: #00ff88;
    }
    .gauge-container {
      display: flex;
      justify-content: center;
      gap: 40px;
      margin: 30px 0 40px 0;
      flex-wrap: wrap;
    }
    .gauge-wrapper {
      display: flex;
      flex-direction: column;
      align-items: center;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 15px;
      padding: 20px;
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
      transition: transform 0.3s ease;
    }
    .gauge-wrapper:hover {
      transform: translateY(-5px);
    }
    .gauge-label {
      font-size: 1.1rem;
      font-weight: bold;
      color: #ffd700;
      margin-top: 10px;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .gauge-value {
      font-size: 1.3rem;
      font-weight: bold;
      color: #00ff88;
      margin-top: 5px;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .gauge-time {
      font-size: 1.0rem;
      font-weight: bold;
      color: #88ddff;
      margin-top: 3px;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .graph-container {
      background: rgba(255, 255, 255, 0.1);
      border-radius: 15px;
      padding: 25px;
      margin: 20px 0;
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
    }
    .graph-container h3 {
      color: #ffd700;
      font-size: 1.4rem;
      margin-bottom: 20px;
      text-align: center;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .graph-container canvas {
      width: 100%;
      height: auto;
      border-radius: 8px;
      background: rgba(0, 0, 0, 0.2);
    }
    .status-indicator {
      display: inline-block;
      padding: 5px 12px;
      border-radius: 20px;
      font-size: 0.9rem;
      font-weight: bold;
      background: rgba(0, 255, 136, 0.3);
      border: 1px solid #00ff88;
    }
    .connection-status {
      position: fixed;
      top: 10px;
      right: 10px;
      padding: 8px 12px;
      border-radius: 15px;
      font-size: 0.8rem;
      font-weight: bold;
      z-index: 1000;
    }
    .connected {
      background: rgba(0, 255, 136, 0.3);
      color: #00ff88;
      border: 1px solid #00ff88;
    }
    .disconnected {
      background: rgba(255, 0, 0, 0.3);
      color: #ff4444;
      border: 1px solid #ff4444;
    }
    @media (max-width: 768px) {
      .container {
        padding: 20px;
      }
      .grid {
        grid-template-columns: 1fr;
      }
      .gauge-container {
        gap: 20px;
      }
      .gauge-wrapper {
        padding: 15px;
      }
      .gauge-wrapper canvas {
        width: 150px !important;
        height: 150px !important;
      }
    }
  </style>
</head>
<body>
  <div class="connection-status" id="connectionStatus">Disconnected</div>
  
  <div class="container">
    <!-- Gauge Dashboard -->
    <div class="gauge-container">
      <div class="gauge-wrapper">
        <canvas id="solarGauge" width="200" height="200"></canvas>
        <div class="gauge-label">Solar Power</div>
        <div class="gauge-value" id="solarGaugeValue">0 W</div>
        <div class="gauge-time" id="solarGaugeToday">0.00 kWh</div>
      </div>
      <div class="gauge-wrapper">
        <canvas id="socGauge" width="200" height="200"></canvas>
        <div class="gauge-label">Battery SoC</div>
        <div class="gauge-value" id="socGaugeValue">0%</div>
        <div class="gauge-time" id="socGaugeKwh">0.00 kWh</div>
      </div>
      <div class="gauge-wrapper">
        <canvas id="batteryGauge" width="200" height="200"></canvas>
        <div class="gauge-label">Battery Power</div>
        <div class="gauge-value" id="batteryGaugeValue">0 W</div>
        <div class="gauge-time" id="batteryTimeToGo">---</div>
      </div>
    </div>
    
    <!-- Daily Solar Graph -->
    <div class="graph-container">
      <h3>Daily Solar Production</h3>
      <canvas id="solarChart" width="800" height="200"></canvas>
    </div>
    
    <div class="grid">
      <div class="card">
        <h3>Battery</h3>
        <div class="metric">
          <span>Voltage:</span>
          <span class="value" id="battVoltage">--</span>
        </div>
        <div class="metric">
          <span>Current:</span>
          <span class="value" id="battCurrent">--</span>
        </div>
        <div class="metric">
          <span>Power:</span>
          <span class="value" id="battPower">--</span>
        </div>
        <div class="metric">
          <span>Capacity used:</span>
          <span class="value" id="battCapacity">--</span>
        </div>
        <div class="metric">
          <span>State of Charge:</span>
          <span class="value" id="battSOC">--</span>
        </div>
        <div class="metric">
          <span>Time to Go:</span>
          <span class="value" id="battTimeToGo">--</span>
        </div>
      </div>
      
      <div class="card">
        <h3>Solar</h3>
        <div class="metric">
          <span>Total Today:</span>
          <span class="value" id="totalToday">-- kWh</span>
        </div>
        <div class="metric">
          <span>Session Time:</span>
          <span class="value" id="sessionTime">--</span>
        </div>
        <div class="metric">
          <span>Left:</span>
          <span class="value"><span id="leftPower">--</span> W <span class="status-indicator" id="leftStatus">--</span></span>
        </div>
        <div class="metric">
          <span>Right:</span>
          <span class="value"><span id="rightPower">--</span> W <span class="status-indicator" id="rightStatus">--</span></span>
        </div>
        <div class="metric">
          <span>Rear:</span>
          <span class="value"><span id="rearPower">--</span> W <span class="status-indicator" id="rearStatus">--</span></span>
        </div>
        <div class="metric">
          <span>Total Power:</span>
          <span class="value" id="totalPower">-- W</span>
        </div>
      </div>
    </div>
  </div>

  <script>
    const dataMins = )rawliteral"
    STRINGIFY(DATA_MINS)
    R"rawliteral(;
    const solarMaxWatts = )rawliteral"
    STRINGIFY(SOLAR_MAX_WATTS)
    R"rawliteral(;
    const battMaxWatts = )rawliteral"
    STRINGIFY(BATT_MAX_WATTS)
    R"rawliteral(;
    const batteryKWH = )rawliteral"
    STRINGIFY(BATT_KWH)
    R"rawliteral(;
    
    var gateway = 'ws://' + window.location.host + '/ws';
    var websocket;
    var reconnectInterval = 1000;
    var maxReconnectInterval = 5000;
    var solarData = [];
    var timeLabels = [];
    var dataChunks = [];
    var sessionActive = false;
    var sessionStartTime = 0;
    var lastMessageTime = 0;
    var connectionTimeout = 2500; // 2.5 seconds without data = assume disconnected
    var heartbeatInterval;
    var currentVersion = null;
    
    function loadInitialData() {
      fetch('/data')
        .then(response => {
          return response.json();
        })
        .then(data => {
          // Update all text fields
          document.getElementById('battVoltage').textContent = data.battVoltage;
          document.getElementById('battCurrent').textContent = data.battCurrent;
          document.getElementById('battPower').textContent = data.battPower;
          document.getElementById('battCapacity').textContent = data.battCapacity;
          document.getElementById('battSOC').textContent = data.battSOC;
      
          document.getElementById('leftPower').textContent = data.leftPower;
          document.getElementById('leftStatus').textContent = data.leftStatus;
          document.getElementById('rightPower').textContent = data.rightPower;
          document.getElementById('rightStatus').textContent = data.rightStatus;
          document.getElementById('rearPower').textContent = data.rearPower;
          document.getElementById('rearStatus').textContent = data.rearStatus;
      
          document.getElementById('totalPower').textContent = data.totalPower;
          document.getElementById('totalToday').textContent = data.totalToday;
          document.getElementById('sessionTime').textContent = data.sessionTime;
      
          // Update gauges manually since the data format is different
          // Extract numeric values from the formatted strings
          let solarPower = parseInt(data.totalPower.replace(' W', '')) || 0;
          let solarToday = parseFloat(data.totalToday.replace(' kWh', '')) || 0;
          let socValue = parseFloat(data.battSOC.replace(' %', '')) || 0;
          let battPowerValue = parseInt(data.battPower.replace(' W', '')) || 0;
      
          // Update Solar Power Gauge
          drawGauge(document.getElementById('solarGauge'), solarPower, 0, solarMaxWatts, 'W', '#00ff88');
          document.getElementById('solarGaugeValue').textContent = solarPower + ' W';
          document.getElementById('solarGaugeToday').textContent = solarToday.toFixed(2) + ' kWh';
      
          // Update Battery SoC Gauge
          let socColor = socValue > 50 ? '#00ff88' : socValue > 20 ? '#ffaa00' : '#ff4444';
          drawGauge(document.getElementById('socGauge'), socValue, 0, 100, '%', socColor);
          document.getElementById('socGaugeValue').textContent = socValue.toFixed(0) + '%';
          const remainingKwh = (socValue / 100) * batteryKWH;
          document.getElementById('socGaugeKwh').textContent = remainingKwh.toFixed(2) + ' kWh';
      
          // Update Battery Power Gauge
          let battColor = battPowerValue > 0 ? '#ff6666' : '#66ff66';
          drawGauge(document.getElementById('batteryGauge'), battPowerValue, -battMaxWatts, battMaxWatts, 'W', battColor, true);
          document.getElementById('batteryGaugeValue').textContent = battPowerValue.toFixed(0) + ' W';
        })
        .catch(error => {
          console.error('Initial data load failed:', error);
        });
    }    

    function initializeDailyData() {
      solarData = new Array(dataMins).fill(0);
      timeLabels = [];
      for (let i = 0; i < dataMins; i++) {
        timeLabels.push(i + 'min');
      }
    }
    
    function formatSessionTime(minutes) {
      if (minutes < 60) {
        return minutes + 'm';
      }
      const hours = Math.floor(minutes / 60);
      const mins = minutes % 60;
      return hours + 'h ' + mins + 'm';
    }
    
    function drawSolarChart() {
      const canvas = document.getElementById('solarChart');
      const ctx = canvas.getContext('2d');
      const width = canvas.width;
      const height = canvas.height;
      
      ctx.clearRect(0, 0, width, height);
      
      const padding = 40;
      const chartWidth = width - 2 * padding;
      const chartHeight = height - 2 * padding;
      
      // Find the last data point to determine chart range
      let lastDataIndex = 0;
      for (let i = solarData.length - 1; i >= 0; i--) {
        if (solarData[i] > 0) {
          lastDataIndex = i;
          break;
        }
      }
      
      // Use at least 900 minutes (15 hours) for scale, or actual data range
      const chartRange = Math.max(900, lastDataIndex + 10);
      const maxPower = Math.max(solarMaxWatts, Math.max(...solarData));
      
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.2)';
      ctx.lineWidth = 1;
      
      // Horizontal grid lines (power levels)
      for (let i = 0; i <= 4; i++) {
        const y = padding + (i * chartHeight / 4);
        ctx.beginPath();
        ctx.moveTo(padding, y);
        ctx.lineTo(width - padding, y);
        ctx.stroke();
        
        ctx.fillStyle = 'rgba(255, 255, 255, 0.8)';
        ctx.font = '12px Arial';
        ctx.textAlign = 'right';
        const powerLabel = ((4 - i) * maxPower / 4 / 1000).toFixed(1) + 'kW';
        ctx.fillText(powerLabel, padding - 5, y + 4);
      }
      
      // Draw solar data line
      ctx.strokeStyle = '#00ff88';
      ctx.lineWidth = 1;
      ctx.beginPath();
      
      let hasData = false;
      let firstPoint = true;
      
      for (let i = 0; i < chartRange; i++) {
        const x = padding + (i * chartWidth / chartRange);
        const y = padding + chartHeight - (solarData[i] / maxPower * chartHeight);
        
        if (firstPoint) {
          ctx.moveTo(x, y);
          firstPoint = false;
        } else {
          ctx.lineTo(x, y);
        }
        
        if (solarData[i] > 0) hasData = true;
      }
      
      if (hasData) {
        ctx.stroke();
        
        // Fill area under curve
        ctx.lineTo(padding + (chartRange * chartWidth / chartRange), height - padding);
        ctx.lineTo(padding, height - padding);
        ctx.closePath();
        ctx.fillStyle = 'rgba(0, 255, 136, 0.2)';
        ctx.fill();
      }
      
      // Current position indicator (if session active)
      if (sessionActive && lastDataIndex > 0) {
        const currentX = padding + (lastDataIndex * chartWidth / chartRange);
        
        ctx.strokeStyle = '#ff6666';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(currentX, padding);
        ctx.lineTo(currentX, height - padding);
        ctx.stroke();
      }
    }
    
    function drawGauge(canvas, value, min, max, unit, color, rotated = false) {
      const ctx = canvas.getContext('2d');
      const centerX = canvas.width / 2;
      const centerY = canvas.height / 2;
      const radius = canvas.width / 2 - 20;
      
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      
      ctx.beginPath();
      ctx.arc(centerX, centerY, radius, 0, 2 * Math.PI);
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.3)';
      ctx.lineWidth = 8;
      ctx.stroke();
      
      let angle;
      if (rotated) {
        const range = max - min;
        const normalizedValue = (value - min) / range;
        angle = (normalizedValue * 1.5 * Math.PI) - (1.25 * Math.PI);
      } else {
        const normalizedValue = Math.min(Math.max(value / max, 0), 1);
        angle = (normalizedValue * 1.5 * Math.PI) - (1.25 * Math.PI);
      }
      
      if (!rotated) {
        ctx.beginPath();
        ctx.arc(centerX, centerY, radius, -1.25 * Math.PI, angle);
        ctx.strokeStyle = color;
        ctx.lineWidth = 8;
        ctx.stroke();
      } else {
        const range = max - min;
        const zeroPosition = (-min) / range;
        const zeroAngle = (zeroPosition * 1.5 * Math.PI) - (1.25 * Math.PI);
        
        let arcColor;
        if (value < 0) {
          arcColor = '#ff6666'; // Red for charging (negative watts)
        } else {
          arcColor = '#66ff66'; // Green for discharging (positive watts)
        }
        
        ctx.beginPath();
        if (angle < zeroAngle) {
          ctx.arc(centerX, centerY, radius, angle, zeroAngle);
        } else {
          ctx.arc(centerX, centerY, radius, zeroAngle, angle);
        }
        ctx.strokeStyle = arcColor;
        ctx.lineWidth = 8;
        ctx.stroke();
      }
      
      ctx.beginPath();
      ctx.moveTo(centerX, centerY);
      const needleLength = radius - 20;
      const needleX = centerX + Math.cos(angle) * needleLength;
      const needleY = centerY + Math.sin(angle) * needleLength;
      ctx.lineTo(needleX, needleY);
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 4;
      ctx.stroke();
      
      ctx.beginPath();
      ctx.arc(centerX, centerY, 8, 0, 2 * Math.PI);
      ctx.fillStyle = '#ffffff';
      ctx.fill();
      
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';
      ctx.lineWidth = 2;
      
      for (let i = 0; i <= 6; i++) {
        const markAngle = (-1.25 * Math.PI) + (i * 1.5 * Math.PI / 6);
        const markValue = rotated ? (min + (i * (max - min) / 6)) : (i * max / 6);
        
        ctx.beginPath();
        const startRadius = radius - 15;
        const endRadius = radius - 5;
        const startX = centerX + Math.cos(markAngle) * startRadius;
        const startY = centerY + Math.sin(markAngle) * startRadius;
        const endX = centerX + Math.cos(markAngle) * endRadius;
        const endY = centerY + Math.sin(markAngle) * endRadius;
        
        ctx.moveTo(startX, startY);
        ctx.lineTo(endX, endY);
        ctx.stroke();
        
        ctx.fillStyle = 'rgba(255, 255, 255, 0.8)';
        ctx.font = '12px Arial';
        ctx.textAlign = 'center';
        const textX = centerX + Math.cos(markAngle) * (startRadius - 10);
        const textY = centerY + Math.sin(markAngle) * (startRadius - 10);
        ctx.fillText(Math.round(markValue), textX, textY + 4);
      }
    }
    
    function updateGauges(data) {
      // Solar Power Gauge (in watts)
      let solarPower = 0;
      let solarToday = 0;
      if (data.solar && data.solar.totalPower) {
        solarPower = parseInt(data.solar.totalPower); // Keep in watts as integer
      }
      if (data.solar && data.solar.totalToday) {
        solarToday = parseFloat(data.solar.totalToday) / 1000; // Convert Wh to kWh
      }
      drawGauge(document.getElementById('solarGauge'), solarPower, 0, solarMaxWatts, 'W', '#00ff88');
      document.getElementById('solarGaugeValue').textContent = solarPower + ' W';
      document.getElementById('solarGaugeToday').textContent = solarToday.toFixed(2) + ' kWh';
      
      // Battery SoC Gauge
      let soc = 0;
      if (data.battery && data.battery.soc) {
        soc = parseFloat(data.battery.soc);
      }
      let socColor = soc > 50 ? '#00ff88' : soc > 20 ? '#ffaa00' : '#ff4444';
      drawGauge(document.getElementById('socGauge'), soc, 0, 100, '%', socColor);
      document.getElementById('socGaugeValue').textContent = soc.toFixed(1) + '%';
      
      // Calculate remaining kWh (8.9kWh pack capacity)
      const remainingKwh = (soc / 100) * 8.9;
      document.getElementById('socGaugeKwh').textContent = remainingKwh.toFixed(2) + ' kWh';
      
      // Battery Power Gauge
      let battPower = 0;
      if (data.battery && data.battery.power) {
        battPower = parseFloat(data.battery.power);
      }
      let battColor = battPower > 0 ? '#ff6666' : '#66ff66';
      drawGauge(document.getElementById('batteryGauge'), battPower, -battMaxWatts, battMaxWatts, 'W', battColor, true);
      document.getElementById('batteryGaugeValue').textContent = battPower.toFixed(0) + ' W';
    }
    
    function initWebSocket() {
      try {
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
        websocket.onerror = onError;
      } catch (error) {
        console.log('Error creating WebSocket: ' + error.message);
      }
    }
    
    function onOpen(event) {
      document.getElementById('connectionStatus').textContent = 'Connected';
      document.getElementById('connectionStatus').className = 'connection-status connected';
      reconnectInterval = 1000;

      // Start monitoring connection health
      lastMessageTime = Date.now();
      if (heartbeatInterval) clearInterval(heartbeatInterval);
      heartbeatInterval = setInterval(checkConnectionHealth, 5000); // Check every 5 seconds
    }
    
    function onClose(event) {
      console.log('WebSocket connection closed. Code: ' + event.code + ', Reason: ' + event.reason);
      document.getElementById('connectionStatus').textContent = 'Disconnected';
      document.getElementById('connectionStatus').className = 'connection-status disconnected';

      if (heartbeatInterval) {
        clearInterval(heartbeatInterval);
        heartbeatInterval = null;
      }

      console.log('Reconnecting in ' + reconnectInterval + 'ms');
      setTimeout(initWebSocket, reconnectInterval);
      reconnectInterval = Math.min(reconnectInterval * 1.5, maxReconnectInterval);
    }
    
    function onError(event) {
      console.log('WebSocket error');
    }
    
    function onMessage(event) {
      lastMessageTime = Date.now();
      
      try {
        var data = JSON.parse(event.data);
        // Check for version changes first
        if (data.version) {
          if (currentVersion === null) {
            // First message - store current version
            currentVersion = data.version;
            console.log('Firmware version:', data.version);
          } else if (currentVersion !== data.version) {
            // New version detected!
            console.log('New firmware detected! Old:', currentVersion, 'New:', data.version);
            console.log('Reloading page...');
            setTimeout(() => {
              location.reload();
            }, 200); // Small delay to show the message
            return; // Don't process rest of the data
          }
        }
  
        if (data.dailyDataChunk) {
          dataChunks[data.chunkIndex] = data.dailyDataChunk;
          return;
        }
        
        if (data.dailyDataComplete) {
          solarData = new Array(dataMins).fill(0);
          
          for (let chunk = 0; chunk < dataChunks.length; chunk++) {
            if (dataChunks[chunk]) {
              const startIndex = chunk * 120;
              for (let i = 0; i < dataChunks[chunk].length && (startIndex + i) < dataMins; i++) {
                solarData[startIndex + i] = dataChunks[chunk][i];
              }
            }
          }
          drawSolarChart();
          return;
        }
        
        if (data.dailyData) {
          solarData = data.dailyData;
          drawSolarChart();
          return;
        }
        
        // Handle session status
        if (data.session) {
          sessionActive = data.session.active;
          if (sessionActive && data.session.startTime) {
            sessionStartTime = data.session.startTime;
          }
        }
        
        updateGauges(data);
        
        if (data.battery) {
          document.getElementById('battVoltage').textContent = data.battery.voltage + ' V';
          document.getElementById('battCurrent').textContent = data.battery.current + ' A';
          document.getElementById('battPower').textContent = data.battery.power + ' W';
          document.getElementById('battCapacity').textContent = data.battery.ampHours + ' Ah';
          document.getElementById('battSOC').textContent = data.battery.soc + ' %';
          
          let timeToGoText = data.battery.timeToGo;
          
          // Convert INF to infinity symbol for card display
          if (timeToGoText === 'INF') {
            document.getElementById('battTimeToGo').innerHTML = '&infin;';
          } else if (timeToGoText.includes('Days')) {
            const daysMatch = timeToGoText.match(/(\d+\.?\d*)\s*Days/);
            if (daysMatch && parseFloat(daysMatch[1]) >= 10.0) {
              document.getElementById('battTimeToGo').innerHTML = '&infin;';
            } else {
              document.getElementById('battTimeToGo').textContent = timeToGoText;
            }
          } else {
            document.getElementById('battTimeToGo').textContent = timeToGoText;
          }
          
          // Also update the gauge display
          let gaugeTimeToGoText = data.battery.timeToGo;
          if (gaugeTimeToGoText === 'INF') {
            document.getElementById('batteryTimeToGo').innerHTML = '&infin;';
          } else if (gaugeTimeToGoText.includes('Days')) {
            const daysMatch = gaugeTimeToGoText.match(/(\d+\.?\d*)\s*Days/);
            if (daysMatch && parseFloat(daysMatch[1]) >= 10.0) {
              document.getElementById('batteryTimeToGo').innerHTML = '&infin;';
            } else {
              document.getElementById('batteryTimeToGo').textContent = gaugeTimeToGoText;
            }
          } else {
            document.getElementById('batteryTimeToGo').textContent = gaugeTimeToGoText;
          }
        }
        
        if (data.panels) {
          document.getElementById('leftPower').textContent = data.panels.left.power || '--';
          document.getElementById('leftStatus').textContent = data.panels.left.status || '--';
          document.getElementById('rightPower').textContent = data.panels.right.power || '--';
          document.getElementById('rightStatus').textContent = data.panels.right.status || '--';
          document.getElementById('rearPower').textContent = data.panels.rear.power || '--';
          document.getElementById('rearStatus').textContent = data.panels.rear.status || '--';
        }
        
        if (data.solar) {
          document.getElementById('totalPower').textContent = data.solar.totalPower + ' W';
          
          // Convert Wh to kWh for display
          const todayKwh = (parseFloat(data.solar.totalToday) / 1000).toFixed(2);
          document.getElementById('totalToday').textContent = todayKwh + ' kWh';
          
          // Update session time display
          if (data.session && data.session.currentIndex >= 0) {
            document.getElementById('sessionTime').textContent = formatSessionTime(data.session.currentIndex);
          } else {
            document.getElementById('sessionTime').textContent = '--';
          }
          
          // Update live data
          if (data.session && data.session.currentIndex >= 0 && data.session.currentIndex < solarData.length) {
            solarData[data.session.currentIndex] = Math.max(solarData[data.session.currentIndex], data.solar.totalPower);
          }
          
          drawSolarChart();
        }
      } catch (error) {
        console.log('Error processing message: ' + error.message);
        console.error('Error parsing JSON:', error);
      }
    }
    
    window.addEventListener('load', onLoad);

    function checkConnectionHealth() {
      var now = Date.now();
  
      // If we haven't received data in a while, assume connection is dead
      if (lastMessageTime > 0 && (now - lastMessageTime) > connectionTimeout) {
        console.log('WebSocket appears dead - no data for', (now - lastMessageTime), 'ms');
    
        // Force close and reconnect
        if (websocket) {
          websocket.close();
        }
    
        // Reset connection status
        document.getElementById('connectionStatus').textContent = 'Reconnecting...';
        document.getElementById('connectionStatus').className = 'connection-status disconnected';
    
        // Restart connection
        setTimeout(initWebSocket, 100);
      }
    }
    
    function onLoad(event) {
      window.pageLoadTime = Date.now();

      loadInitialData();
      
      initializeDailyData();
      
      updateGauges({});
      
      drawSolarChart();
      
      if (typeof WebSocket === 'undefined') {
        console.log('ERROR: WebSocket not supported by this browser!');
        document.getElementById('connectionStatus').textContent = 'WebSocket Not Supported';
        document.getElementById('connectionStatus').className = 'connection-status disconnected';
        return;
      }
      
      initWebSocket();
      
      setInterval(function() {
        drawSolarChart();
      }, 60000);
    }
  </script>
</body>
</html>
)rawliteral";

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch(type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      Serial.printf("WebSocket connection established, sending daily data...\n");
      
      // Send daily data to new client
      sendDailySolarDataAsync(client);
      break;
      
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
      
    case WS_EVT_ERROR:
      Serial.printf("WebSocket client #%u error(%u): %s\n", client->id(), *((uint16_t*)arg), (char*)data);
      break;
      
    case WS_EVT_DATA:
      break;
      
    default:
      break;
  }
}

void handleFavicon(AsyncWebServerRequest *request) {
  String svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 48 48\">";
  
  // Sun rays (8 triangular rays around the circle)
  svg += "<polygon points=\"24,2 26,10 22,10\" fill=\"#ff8800\"/>";          // Top
  svg += "<polygon points=\"46,24 38,22 38,26\" fill=\"#ff8800\"/>";         // Right
  svg += "<polygon points=\"24,46 22,38 26,38\" fill=\"#ff8800\"/>";         // Bottom
  svg += "<polygon points=\"2,24 10,26 10,22\" fill=\"#ff8800\"/>";          // Left
  svg += "<polygon points=\"36.5,11.5 32,16 29,13\" fill=\"#ff8800\"/>";     // Top-right
  svg += "<polygon points=\"36.5,36.5 32,32 35,29\" fill=\"#ff8800\"/>";     // Bottom-right
  svg += "<polygon points=\"11.5,36.5 16,32 19,35\" fill=\"#ff8800\"/>";     // Bottom-left
  svg += "<polygon points=\"11.5,11.5 16,16 13,19\" fill=\"#ff8800\"/>";     // Top-left
  
  // Main sun circle (outer orange, inner yellow)
  svg += "<circle cx=\"24\" cy=\"24\" r=\"14\" fill=\"#ff8800\"/>";
  svg += "<circle cx=\"24\" cy=\"24\" r=\"11\" fill=\"#ffdd00\"/>";
  
  svg += "</svg>";
  
  request->send(200, "image/svg+xml", svg);
}

void sendDailySolarDataAsync(AsyncWebSocketClient *client) {
  const int chunkSize = 120;
  
  for (int chunk = 0; chunk < 7; chunk++) {
    DynamicJsonDocument doc(2048);
    JsonArray solarArray = doc.createNestedArray("dailyDataChunk");
    doc["chunkIndex"] = chunk;
    doc["totalChunks"] = 7;
    
    int startIndex = chunk * chunkSize;
    int endIndex = min((chunk + 1) * chunkSize, DATA_MINS);
    
    for (int i = startIndex; i < endIndex; i++) {
      solarArray.add(dailySolarData[i]);
    }
    
    String output;
    serializeJson(doc, output);
    
    client->text(output);
    delay(10);
  }
  
  DynamicJsonDocument completeDoc(256);
  completeDoc["dailyDataComplete"] = true;
  String completeOutput;
  serializeJson(completeDoc, completeOutput);
  client->text(completeOutput);
}

void initializeDevices() {
  for (int i = 0; i < 3; i++) {
    solarDevices[i].name = deviceNames[i];
    solarDevices[i].voltage = 0;
    solarDevices[i].current = 0;
    solarDevices[i].power = 0;
    solarDevices[i].todayYield = 0;
    solarDevices[i].state = 0;
    solarDevices[i].lastUpdate = 0;
    solarDevices[i].valid = false;
  }
  
  batteryDevice.name = "Battery";
  batteryDevice.voltage = 0;
  batteryDevice.current = 0;
  batteryDevice.power = 0;
  batteryDevice.ampHours = 0;
  batteryDevice.soc = 0;
  batteryDevice.timeToGo = 0;
  batteryDevice.lastUpdate = 0;
  batteryDevice.valid = false;
  
  for (int i = 0; i < DATA_MINS; i++) {
    dailySolarData[i] = 0;
  }
}

void initializeFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  Serial.println("SPIFFS Mounted Successfully");
  loadSessionData();
}

void loadSessionData() {
  File file = SPIFFS.open("/solarSession.bin", "r");
  if (file) {
    // Read session data
    file.read((uint8_t*)&sessionActive, sizeof(sessionActive));
    file.read((uint8_t*)&currentDataIndex, sizeof(currentDataIndex));
    file.read((uint8_t*)&sessionStartTime, sizeof(sessionStartTime));
    file.read((uint8_t*)dailySolarData, sizeof(dailySolarData));
    file.close();
  } else {
    Serial.println("No existing session data found, starting fresh");
    sessionActive = false;
    currentDataIndex = 0;
    sessionStartTime = millis();
  }
}

void saveSessionData() {
  File file = SPIFFS.open("/solarSession.bin", "w");
  if (file) {
    file.write((uint8_t*)&sessionActive, sizeof(sessionActive));
    file.write((uint8_t*)&currentDataIndex, sizeof(currentDataIndex));
    file.write((uint8_t*)&sessionStartTime, sizeof(sessionStartTime));
    file.write((uint8_t*)dailySolarData, sizeof(dailySolarData));
    file.close();
    
    dataChangedSinceLastSave = false;
    lastFlashSave = millis();
    
    Serial.println("Session data saved to flash");
  } else {
    Serial.println("Failed to save session data!");
  }
}

bool shouldSaveToFlash() {
  unsigned long now = millis();
  
  // Check various conditions that trigger a save
  if (otaInProgress) {
    Serial.println("OTA in progress - saving before update");
    return true;
  }
  
  if (rebootRequested) {
    Serial.println("Reboot requested - saving before restart");
    return true;
  }
  
  // Regular 5-minute interval save (only if data has changed)
  if (dataChangedSinceLastSave && (now - lastFlashSave >= FLASH_SAVE_INTERVAL)) {
    Serial.println("5-minute interval save");
    return true;
  }
  
  return false;
}

void updateDailyData(uint16_t totalPower) {
  unsigned long now = millis();
  
  // Check if all solar devices are OFF
  bool allOff = true;
  unsigned long currentTime = millis();
  for (int i = 0; i < 3; i++) {
    if (solarDevices[i].valid && (currentTime - solarDevices[i].lastUpdate < 10000)) {
      if (solarDevices[i].state != 0) { // Not OFF
        allOff = false;
        break;
      }
    }
  }
  
  // Session management logic
  if (!sessionActive && totalPower > 10) {
    // Start new session - first light detected
    Serial.println("Starting new solar session - first light detected");
    sessionActive = true;
    sessionStartTime = now;
    currentDataIndex = 0;
    lastMinuteUpdate = now;
    
    // Clear previous session data
    for (int i = 0; i < DATA_MINS; i++) {
      dailySolarData[i] = 0;
    }
    
    dataChangedSinceLastSave = true; // Mark that data has changed
  }
  
  if (sessionActive) {
    // Store previous value to check for changes
    uint16_t previousValue = (currentDataIndex < DATA_MINS) ? dailySolarData[currentDataIndex] : 0;
    
    // Update current minute's data
    if (currentDataIndex < DATA_MINS) {
      uint16_t newValue = max(dailySolarData[currentDataIndex], totalPower);
      if (newValue != previousValue) {
        dailySolarData[currentDataIndex] = newValue;
        dataChangedSinceLastSave = true; // Mark that data has changed
      }
    }
    
    // Check if it's time to move to next minute
    if (now - lastMinuteUpdate >= MINUTE_INTERVAL) {
      lastMinuteUpdate = now;
      if (currentDataIndex + 1 < DATA_MINS) { // Don't overflow array
        currentDataIndex++;
        dataChangedSinceLastSave = true; // New minute means data changed
      }
    }
    
    // End session if all solar devices are OFF
    if (allOff && totalPower <= 10) {
      Serial.println("Ending solar session - all panels OFF");
      sessionActive = false;
      dataChangedSinceLastSave = true; // Session state changed
    }
  }
}

void updateDisplayAndWebSocket() {
  unsigned long now = millis();
  
  if (now - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    lastDisplayUpdate = now;
    
    uint16_t totalPower = 0;
    float totalToday = 0;
    int activePanels = 0;
    String solarStatus = "OFF";
    
    for (int i = 0; i < 3; i++) {
      if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000)) {
        totalPower += solarDevices[i].power;
        totalToday += solarDevices[i].todayYield;
        activePanels++;
        if (solarDevices[i].state >= 3 && solarDevices[i].state <= 6) {
          solarStatus = statTxt[solarDevices[i].state];
        }
      }
    }
    
    updateDailyData(totalPower);
    
    // Check if we should save to flash (replaces the old every-minute save)
    if (shouldSaveToFlash()) {
      saveSessionData();
    }
        
    if (printData && batteryDevice.valid) {
      Serial.printf("Battery:          %6.2fV  %6.2fA  %6.0fW  %5.1fAh  %5.1f%%   %s\n",
        batteryDevice.voltage, batteryDevice.current, batteryDevice.power,
        batteryDevice.ampHours, batteryDevice.soc, 
        (batteryDevice.timeToGo == 65535) ? "---" : 
        (batteryDevice.timeToGo >= 1440) ? (String(batteryDevice.timeToGo / 1440.0, 1) + " Days").c_str() :
        (String(batteryDevice.timeToGo / 60) + ":" + (batteryDevice.timeToGo % 60 < 10 ? "0" : "") + String(batteryDevice.timeToGo % 60)).c_str());
      Serial.printf("Solar Total:      %6dW  %6.0fWh  %s  Session: %s (%dm)\n",
        totalPower, totalToday, solarStatus.c_str(), 
        sessionActive ? "Active" : "Inactive", currentDataIndex);
        printData = 0;
    }
  }
    
  if (now - lastWebSocketUpdate >= WEBSOCKET_UPDATE_INTERVAL) {
    lastWebSocketUpdate = now;
    
    DynamicJsonDocument doc(1024);

    // hash for auto reload if we update the code
    String buildTime = __TIMESTAMP__;
    uint32_t hash = 0;
    for (int i = 0; i < buildTime.length(); i++) {
      hash = hash * 31 + buildTime.charAt(i);
    }
    doc["version"] = String(hash);
    
    // Session information
    JsonObject session = doc.createNestedObject("session");
    session["active"] = sessionActive;
    session["currentIndex"] = currentDataIndex;
    session["startTime"] = sessionStartTime;
    
    if (batteryDevice.valid && (now - batteryDevice.lastUpdate < 10000)) {
      JsonObject battery = doc.createNestedObject("battery");
      battery["voltage"] = String(batteryDevice.voltage, 2);
      battery["current"] = String(batteryDevice.current, 2);
      battery["power"] = String(batteryDevice.power, 0);
      battery["ampHours"] = String(batteryDevice.ampHours, 1);
      battery["soc"] = String(batteryDevice.soc, 1);
      
      if (batteryDevice.timeToGo == 65535) {
        battery["timeToGo"] = "INF";
      } else if (batteryDevice.timeToGo >= 14400) {
        battery["timeToGo"] = "INF";
      } else if (batteryDevice.timeToGo >= 1440) {
        battery["timeToGo"] = String(batteryDevice.timeToGo / 1440.0, 1) + " Days";
      } else {
        int hours = batteryDevice.timeToGo / 60;
        int minutes = batteryDevice.timeToGo % 60;
        battery["timeToGo"] = String(hours) + ":" + (minutes < 10 ? "0" : "") + String(minutes);
      }
    }
    
    uint16_t totalPower = 0;
    float totalToday = 0;
    int activePanels = 0;
    String solarStatus = "OFF";
    
    JsonObject panels = doc.createNestedObject("panels");
    JsonObject leftPanel = panels.createNestedObject("left");
    JsonObject rightPanel = panels.createNestedObject("right");
    JsonObject rearPanel = panels.createNestedObject("rear");
    
    for (int i = 0; i < 3; i++) {
      JsonObject* currentPanel;
      if (i == 0) currentPanel = &leftPanel;
      else if (i == 1) currentPanel = &rightPanel;
      else currentPanel = &rearPanel;
      
      if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000)) {
        (*currentPanel)["power"] = solarDevices[i].power;
        (*currentPanel)["status"] = statTxt[solarDevices[i].state];
        totalPower += solarDevices[i].power;
        totalToday += solarDevices[i].todayYield;
        activePanels++;
        if (solarDevices[i].state >= 3 && solarDevices[i].state <= 6) {
          solarStatus = statTxt[solarDevices[i].state];
        }
      } else {
        (*currentPanel)["power"] = 0;
        (*currentPanel)["status"] = "OFF";
      }
    }
    
    JsonObject solar = doc.createNestedObject("solar");
    solar["totalPower"] = totalPower;
    solar["totalToday"] = String(totalToday, 0);
    solar["status"] = solarStatus;
    solar["activePanels"] = activePanels;
    
    String output;
    serializeJson(doc, output);
    ws.textAll(output);
  }
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      #define manDataSizeMax 31
      if (advertisedDevice.haveManufacturerData() == true) {
        uint8_t manCharBuf[manDataSizeMax+1];
        std::string manData = advertisedDevice.getManufacturerData();
        int manDataSize=manData.length();
        manData.copy((char *)manCharBuf, manDataSize);
        victronManufacturerData * vicData=(victronManufacturerData *)manCharBuf;
        
        if (vicData->vendorID!=0x02e1 || vicData->victronRecordType < 1 || vicData->victronRecordType > 2 ) {
          return;
        }

        digitalWrite(led, 1);

        if (advertisedDevice.haveName()) {
          strcpy(savedDeviceName,advertisedDevice.getName().c_str());
        }
        
        bool decryptionSuccessful = false;
        uint8_t outputData[16] = {0};
        int matchingKeyIndex = -1;

        for (int keyIndex = 0; keyIndex < 4 && !decryptionSuccessful; keyIndex++) {
          if (vicData->encryptKeyMatch != keys[keyIndex][0]) {
            continue;
          }

          uint8_t inputData[16];
          int encrDataSize = manDataSize - 10;
          for (int i = 0; i < encrDataSize; i++) {
            inputData[i] = vicData->victronEncryptedData[i];
          }

          mbedtls_aes_context ctx;
          mbedtls_aes_init(&ctx);

          auto status = mbedtls_aes_setkey_enc(&ctx, keys[keyIndex], keyBits);
          if (status != 0) {
            mbedtls_aes_free(&ctx);
            continue;
          }

          uint8_t data_counter_lsb = (vicData->nonceDataCounter) & 0xff;
          uint8_t data_counter_msb = ((vicData->nonceDataCounter) >> 8) & 0xff;
          uint8_t nonce_counter[16] = {data_counter_lsb, data_counter_msb, 0};
          uint8_t stream_block[16] = {0};

          size_t nonce_offset = 0;
          status = mbedtls_aes_crypt_ctr(&ctx, encrDataSize, &nonce_offset, nonce_counter, stream_block, inputData, outputData);
          mbedtls_aes_free(&ctx);

          if (status == 0) {
            decryptionSuccessful = true;
            matchingKeyIndex = keyIndex;
          }
        }

        if (!decryptionSuccessful) {
          return;
        }

        if (vicData->victronRecordType == 1) {
          victronPanelData * victronData = (victronPanelData *) outputData;
          uint8_t deviceState = victronData->deviceState;
          float batteryVoltage = float(victronData->batteryVoltage) * 0.01;
          float batteryCurrent = float(victronData->batteryCurrent) * 0.1;
          float todayYield = float(victronData->todayYield) * 0.01 * 1000;
          uint16_t inputPower = victronData->inputPower;
          
          if (deviceState == 245) deviceState = 8;
          if (deviceState > 8) deviceState = 1;
          
          if (matchingKeyIndex >= 0 && matchingKeyIndex < 3) {
            solarDevices[matchingKeyIndex].voltage = batteryVoltage;
            solarDevices[matchingKeyIndex].current = batteryCurrent;
            solarDevices[matchingKeyIndex].power = inputPower;
            solarDevices[matchingKeyIndex].todayYield = todayYield;
            solarDevices[matchingKeyIndex].state = deviceState;
            solarDevices[matchingKeyIndex].lastUpdate = millis();
            solarDevices[matchingKeyIndex].valid = true;
          }
        } else {
          victronBattData * victronData = (victronBattData *) outputData;
          float batteryVoltage = float(victronData->batteryVoltage) * 0.01;
          uint32_t current_raw = (victronData->packed >> 2) & 0x3FFFFF;
          uint32_t amp_hours_raw = (victronData->packed >> 24) & 0xFFFFF;
          uint32_t soc_raw = (victronData->packed >> 44) & 0x3FF;
          
          int32_t current_signed = 0;
          if (current_raw != 0x3FFFFF) {
            if (current_raw & 0x200000) {
              current_signed = (int32_t)current_raw - 0x400000;
            } else {
              current_signed = (int32_t)current_raw;
            }
          }
          
          if (amp_hours_raw == 0xFFFFF) amp_hours_raw = 0;
          if (soc_raw == 0x3FF) soc_raw = 0;
          
          batteryDevice.voltage = batteryVoltage;
          batteryDevice.current = current_signed * 0.001;
          batteryDevice.power = batteryVoltage * (current_signed * 0.001);
          batteryDevice.ampHours = amp_hours_raw * 0.1;
          batteryDevice.soc = soc_raw * 0.1;
          batteryDevice.timeToGo = victronData->timeToGo;
          batteryDevice.lastUpdate = millis();
          batteryDevice.valid = true;
        }
      }
    digitalWrite(led, 0);
    }
};

void handleDataEndpoint(AsyncWebServerRequest *request) {
  unsigned long now = millis();
  uint16_t totalPower = 0;
  float totalToday = 0;
  
  for (int i = 0; i < 3; i++) {
    if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000)) {
      totalPower += solarDevices[i].power;
      totalToday += solarDevices[i].todayYield;
    }
  }
  
  String sessionTimeStr = "--";
  if (sessionActive && currentDataIndex >= 0) {
    if (currentDataIndex < 60) {
      sessionTimeStr = String(currentDataIndex) + "m";
    } else {
      int hours = currentDataIndex / 60;
      int mins = currentDataIndex % 60;
      sessionTimeStr = String(hours) + "h " + String(mins) + "m";
    }
  }
  
  String response = "{";
  if (batteryDevice.valid && (now - batteryDevice.lastUpdate < 10000)) {
    response += "\"battVoltage\":\"" + String(batteryDevice.voltage, 2) + " V\",";
    response += "\"battCurrent\":\"" + String(batteryDevice.current, 2) + " A\",";
    response += "\"battPower\":\"" + String(batteryDevice.power, 0) + " W\",";
    response += "\"battCapacity\":\"" + String(batteryDevice.ampHours, 1) + " Ah\",";
    response += "\"battSOC\":\"" + String(batteryDevice.soc, 1) + " %\",";
    
    // Add individual panel data
    for (int i = 0; i < 3; i++) {
      String panelName = (i == 0) ? "left" : (i == 1) ? "right" : "rear";
      if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000)) {
        response += "\"" + panelName + "Power\":\"" + String(solarDevices[i].power) + "\",";
        response += "\"" + panelName + "Status\":\"" + String(statTxt[solarDevices[i].state]) + "\",";
      } else {
        response += "\"" + panelName + "Power\":\"--\",";
        response += "\"" + panelName + "Status\":\"--\",";
      }
    }
    
    response += "\"totalPower\":\"" + String(totalPower) + " W\",";
    response += "\"totalToday\":\"" + String(totalToday / 1000.0, 2) + " kWh\",";
    response += "\"sessionTime\":\"" + sessionTimeStr + "\"";
  } else {
    response += "\"battVoltage\":\"--\",\"battCurrent\":\"--\",\"battPower\":\"--\",";
    response += "\"battCapacity\":\"--\",\"battSOC\":\"--\",";
    response += "\"leftPower\":\"--\",\"leftStatus\":\"--\",";
    response += "\"rightPower\":\"--\",\"rightStatus\":\"--\",";
    response += "\"rearPower\":\"--\",\"rearStatus\":\"--\",";
    response += "\"totalPower\":\"-- W\",\"totalToday\":\"-- kWh\",\"sessionTime\":\"--\"";
  }
  response += "}";
  request->send(200, "application/json", response);
}
  
void setup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, 1);
  Serial.begin(115200);  
  Serial.println();
  Serial.printf("%s - Built %s\n",apssid,__TIMESTAMP__);
  Serial.println();
  
  initializeDevices();
  initializeFileSystem();
  // No NTP time setup needed for off-grid operation

  wifiMode = 1;
  WiFi.mode((wifi_mode_t)wifiMode);
  WiFi.begin(stssid, stkey);
  
  strcpy(savedDeviceName,"(unknown device name)");
  
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(20);
  pBLEScan->setWindow(19);
  
  Serial.println("setup() complete.");
  Serial.handle();
}

void loop() {
    ArduinoOTA.handle();
    Serial.handle();
    
    while (Serial.available()) {
        uint8_t gc = Serial.read();
        if (gc == 1) {  // Did client send CTRL-A [ENTER] (Update)?
            Serial.println("REBOOTING NOW...");
            Serial.handle();
            rebootRequested = true;
            saveSessionData(); // Save before reboot
            vTaskDelay(100);
            Serial.handle();
            vTaskDelay(100);
            Serial.handle();
            vTaskDelay(100);
            Serial.handle();
            vTaskDelay(100);
            ESP.restart();
        } else if (gc == 13) {
            printData = 1;
        }
    }
        
    if (wifiMode > 0) {
        if (!isWifiConnected) {
            if (WiFi.isConnected()) {
                noap = 0;
                Serial.print("Wifi now connected to SSID ");
                Serial.println(stssid);
                Serial.print("IP address: ");
                Serial.println(WiFi.localIP());
                Serial.print("RSSI: ");
                Serial.println(WiFi.RSSI());
                needServerInit = true;
                digitalWrite(led, 0);
            }
            if (wifiMode == 2) {
                Serial.print("Wifi AP setup as SSID: ");
                Serial.println(apssid);
                Serial.print("IP address: ");
                Serial.println(WiFi.softAPIP());
                needServerInit = true;
                digitalWrite(led, 0);
            }
            if (needServerInit) {
                isWifiConnected = true;
                
                server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
                  request->send_P(200, "text/html", index_html);
                });
                server.on("/favicon.ico", HTTP_GET, handleFavicon);
                server.on("/data", HTTP_GET, handleDataEndpoint);
                ws.onEvent(onWsEvent);
                server.addHandler(&ws);
                server.begin();
                                
                ArduinoOTA.setPort(3232);
                ArduinoOTA.setHostname(apssid);
                ArduinoOTA.setPassword(otaPassword);
                
                ArduinoOTA.onStart([]() {
                   otaInProgress = true;
                   saveSessionData(); // Save before OTA starts
                   String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
                   Serial.println("Start updating " + type);
                }).onEnd([]() {
                   otaInProgress = false;
                   Serial.println("\nEnd");
                }).onProgress([](unsigned int progress, unsigned int total) {
                   Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
                }).onError([](ota_error_t error) {
                   otaInProgress = false;
                   Serial.printf("Error[%u]: ", error);
                   if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
                   else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
                   else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
                   else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
                   else if (error == OTA_END_ERROR) Serial.println("End Failed");
                });
                ArduinoOTA.begin();
                
                Serial.println("Web server started on port 80");
                Serial.printf("Browse to: http://%s\n", WiFi.localIP().toString().c_str());
            }
        } else {
            if (!WiFi.isConnected()) {
                if (wifiMode == 1) {
                    Serial.println("WiFi disconnected. Bummer!");
                    isWifiConnected = false;
                    isWifiActive = false;
                }
            }
        }
    }

    if (millis() > (timer + 250)) {
        timer = millis();
        cnt++;
        if (wifiMode == 1 && WiFi.isConnected() == 0) {
          noap++;
          if (noap >= 60) {
            WiFi.mode(WIFI_AP);
            WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
            WiFi.softAP(apssid, apkey);
            wifiMode = 2;
            noap = 0;
          }
        }
    }

    updateDisplayAndWebSocket();

    BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
    pBLEScan->clearResults();
    
    yield();
}
