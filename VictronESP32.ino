#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <SPIFFS.h>
#include "mbedtls/aes.h"

// WiFi Configuration - Primary and Secondary SSIDs
const char* stssid = "YOUR-SSID";         // Primary WiFi network
const char* stkey = "PASSWORD";           // Primary WiFi password
const char* stssid2 = "YOUR-SSID2";       // Secondary WiFi network
const char* stkey2 = "PASSWORD2";         // Secondary WiFi password

const char* otaPassword = "PASS";         // OTA Password

const char* hostname = "ESP32-Victron";

const char* mpptNames[] = {"left", "right", "rear"};
const char* deviceNames[] = {"LEFT", "RIGHT", "REAR", "BATT"};

#define BATT_KWH 9.0           // Size of battery in kWh
#define BATT_MAX_WATTS 1500    // Max expected battery charge/discharge for analog gauge
#define SOLAR_MAX_WATTS 2000   // Max Total Solar Wattage you expect to get in most cases
#define DATA_MINS 840          // 14 hours of data - Needs to be a multiple of 120!
#define UPDATE_INTERVAL 1000   // 1 second
#define led         2          // GPIO for LED
#define button      0          // GPIO for Button

WiFiClient clientNode;
WiFiUDP wifiUDPServer;
BLEScan *pBLEScan;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

struct SessionData {
    bool active;                       // 1 byte  
    bool newDay;                       // 1 byte
    uint16_t index;                    // 2 bytes
    uint16_t runTime;                  // 2 bytes
    uint16_t daily[DATA_MINS];         // 840 * 2 = 1680 bytes
    uint16_t checksum;                 // 2 bytes
};                                     // 1688 Total bytes

struct SystemState {
    // Battery data
    bool batteryValid;
    float battVoltage;
    float battCurrent;
    float battPower;
    float battAmpHours;
    float battSoc;
    uint16_t battTimeToGo;
    
    // MPPT data
    uint16_t mpptPowers[3];
    const char* mpptStatuses[3];
    bool mpptValid[3];
    
    // Totals
    uint16_t totalPower;
    float totalToday;
        
    // Data freshness
    unsigned long lastUpdate;
    
    // Constructor to initialize
    SystemState() : batteryValid(false), battVoltage(0), battCurrent(0), battPower(0),
                   battAmpHours(0), battSoc(0), battTimeToGo(0), totalPower(0), 
                   totalToday(0), lastUpdate(0) {
        for (int i = 0; i < 3; i++) {
            mpptPowers[i] = 0;
            mpptStatuses[i] = "?";
            mpptValid[i] = false;
        }
    }
};

#define STRING(x) #x
#define STRINGIFY(x) STRING(x)

const uint8_t keys[4][16] = {
// LEFT:
    {0xb1, 0xf1, 0x11, 0xd9, 0xcf, 0xab, 0x90, 0x05,
     0x65, 0x33, 0xe7, 0xdb, 0x9f, 0x15, 0xed, 0xc7},
// RIGHT:    
    {0x81, 0x93, 0xc2, 0x38, 0xf1, 0x5b, 0x81, 0x0d,
     0xca, 0x0d, 0xda, 0xd7, 0x2c, 0x63, 0x8b, 0x84},
// REAR:    
    {0xf1, 0xe3, 0x13, 0xd9, 0x65, 0xa8, 0x79, 0x75,
     0xe5, 0x04, 0x53, 0x16, 0x73, 0x07, 0x52, 0x8a},    
// BMV:
    {0x21, 0x84, 0x33, 0xa7, 0x66, 0x62, 0xf5, 0xa4,
     0x42, 0x5c, 0x7c, 0xd2, 0xa9, 0x59, 0xbe, 0xf1},
};

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
} __attribute__((packed)) victronMpptData;

typedef struct {
   uint16_t timeToGo;
   int16_t batteryVoltage;
   uint16_t alarmReason;
   int16_t auxVoltage;
   uint64_t packed;
} __attribute__((packed)) victronBattData;

// WiFi network credentials structure
struct WiFiNetwork {
  const char* ssid;
  const char* password;
};

WiFiNetwork wifiNetworks[] = {
  {stssid, stkey},    // Primary network
  {stssid2, stkey2}   // Secondary network  
};

TaskHandle_t bleScanTaskHandle = NULL;
SemaphoreHandle_t deviceDataMutex = NULL; // For thread-safe access to device data

size_t minFreeHeap = SIZE_MAX;
extern const char index_html[] PROGMEM;

SolarData solarDevices[3]; // LEFT, RIGHT, REAR
BatteryData batteryDevice;
SystemState state;
SessionData session = {0};

const char* getResetReasonString(esp_reset_reason_t reason) {
    switch(reason) {
        case ESP_RST_POWERON: return "Power-on reset";
        case ESP_RST_EXT: return "External reset";
        case ESP_RST_SW: return "Software reset";
        case ESP_RST_PANIC: return "Exception/panic reset";
        case ESP_RST_INT_WDT: return "Interrupt watchdog reset";
        case ESP_RST_TASK_WDT: return "Task watchdog reset";
        case ESP_RST_WDT: return "Other watchdog reset";
        case ESP_RST_DEEPSLEEP: return "Deep sleep reset";
        case ESP_RST_BROWNOUT: return "Brown-out reset";
        case ESP_RST_SDIO: return "SDIO reset";
        default: return "Unknown reset";
    }
}

const unsigned long WIFI_SCAN_INTERVAL = 30000; // Scan every 30 seconds when in AP mode
const unsigned long WIFI_CONNECT_TIMEOUT = 15000; // 15 seconds to connect
const int numWifiNetworks = sizeof(wifiNetworks) / sizeof(wifiNetworks[0]);
const char* mpptStatuses[3] = {"OFF", "OFF", "OFF"};

static uint8_t lowPowerCount = 0;
static float lastTodayYield[3] = {-1, -1, -1}; // -1 = not initialized
static unsigned long minTimer = 0;

bool bleDataUpdated = false;
bool allSolarOff = true;
bool printData = 0;
bool dataChangedSinceLastSave = false;
bool dataChangedSinceLastBackup = false;
bool otaInProgress = false;
bool otaEnabled = false;
bool batteryValid = false;
bool wifiScanInProgress = false;
bool isWifiConnected = false;
char savedDeviceName[32];
uint8_t backupTimer = 0;
uint8_t cnt = 0;
uint8_t SSID[32];
uint8_t WPA2Key[64];
uint16_t battTimeToGo = 0;
uint16_t mpptPowers[3] = {0, 0, 0};
uint16_t loopCount = 0;
uint32_t timer;
int32_t RSSI;
int currentSSIDIndex = -1; // -1 = none, 0 = primary, 1 = secondary
int keyBits = 128;
int scanTime = 1;
int lastClientCount = 0;
unsigned long lastWifiScan = 0;
unsigned long wifiConnectStartTime = 0;
unsigned long lastUpdate = 0;
float battVoltage = 0, battCurrent = 0, battPower = 0, battAmpHours = 0, battSoc = 0;

void reboot() {
  Serial.println("REBOOTING NOW...");
  saveSessionDataToSPIFFS();
            
  // Stop BLE task before reboot
  if (bleScanTaskHandle != NULL) {
    vTaskDelete(bleScanTaskHandle);
    bleScanTaskHandle = NULL;
  }
  vTaskDelay(100 / portTICK_PERIOD_MS);
  ESP.restart();
}

void checkMemoryHealth() {
    static unsigned long lastMemCheck = 0;
    static size_t lastFreeHeap = 0;
    static uint8_t memoryDropCount = 0;
    static unsigned long lowMemoryStart = 0;
    
    unsigned long now = millis();
    
    if (now - lastMemCheck < 5000) return;
    lastMemCheck = now;
    
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minFreeHeap) {
        minFreeHeap = freeHeap;
    }

    if (!heap_caps_check_integrity_all(false)) {
        Serial.println("############# HEAP CORRUPTION DETECTED!!! #############");
        reboot();
    }
    
    // Track extended low memory periods
    if (freeHeap < 20000) {
        if (lowMemoryStart == 0) {
            lowMemoryStart = now;
        } else if (now - lowMemoryStart > 300000) { // 5 minutes of low memory
            Serial.printf("WARNING: 5+ minutes of low memory condition (current: %d bytes)\r\n", freeHeap);
            reboot();
        }
    } else {
        lowMemoryStart = 0;
    }
    
    // Check for sudden memory drops (possible corruption)
    if (lastFreeHeap > 0) {
        int32_t memoryDrop = lastFreeHeap - freeHeap;
        if (memoryDrop > 10000) { // More than 10KB drop
            memoryDropCount++;
            Serial.printf("WARNING: Large memory drop: %d bytes (total drops: %d)\r\n", memoryDrop, memoryDropCount);
            
            if (memoryDropCount > 3) {
                Serial.println("CRITICAL: Multiple large memory drops - possible corruption");
            }
        } else if (memoryDrop < -5000) { // Memory suddenly increased (also suspicious)
            Serial.printf("INFO: Sudden memory increase: %d bytes\r\n", -memoryDrop);
        }
    }
    lastFreeHeap = freeHeap;
    
    // Critical memory threshold
    if (freeHeap < 8192) {
        Serial.printf("CRITICAL: Low memory (%d bytes), forcing reset!\r\n", freeHeap);
        reboot();
    }
    // Warning threshold with cleanup
    else if (freeHeap < 12288) {
        Serial.printf("WARNING: Low memory (%d bytes) - cleaning up\r\n", freeHeap);
        
        // Aggressive cleanup
        ws.cleanupClients();
        
        // Force garbage collection
        void* ptr = malloc(1024);
        if (ptr) free(ptr);
    }
}

void monitorBLETask() {
    static unsigned long lastBLECheck = 0;
    unsigned long now = millis();
    
    if (now - lastBLECheck < 60000) return; // Check every minute
    lastBLECheck = now;
    
    if (bleScanTaskHandle) {
        UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(bleScanTaskHandle);
        if (stackLeft < 500) {
            Serial.printf("WARNING: BLE task low on stack (%d bytes left)\r\n", stackLeft);
        }
    } else {
        Serial.println("ERROR: BLE task is dead - attempting restart");
        
        // Attempt to restart BLE task
        BaseType_t taskCreated = xTaskCreatePinnedToCore(
            bleScanTask,
            "BLE_Scan_Task",
            8192,
            NULL,
            1,
            &bleScanTaskHandle,
            1
        );
        
        if (taskCreated == pdPASS) {
            Serial.println("BLE task restarted successfully");
        }
    }
}

void setupWiFi() {
  Serial.println("Initializing WiFi...");
  
  WiFi.mode(WIFI_STA);
  delay(100);
  
  scanForWiFiNetworks();
  
  initializeWebServer();
}

void scanForWiFiNetworks() {
  if (wifiScanInProgress) return;
  
  Serial.println("Scanning for WiFi networks...");
  wifiScanInProgress = true;
  lastWifiScan = millis();
  
  int n = WiFi.scanNetworks();
  wifiScanInProgress = false;
  
  if (n == 0) {
    Serial.println("No networks found - will retry in 30 seconds");
    return;
  }
  
  Serial.printf("Found %d networks:\r\n", n);
  
  int foundNetworkIndex = -1;
  int bestRSSI = -999;
  
  for (int i = 0; i < n; i++) {
    String foundSSID = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    
    Serial.printf("  %d: %s (%d dBm) %s\r\n", i, foundSSID.c_str(), rssi, 
               WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " " : "*");
    
    for (int j = 0; j < numWifiNetworks; j++) {
      if (foundSSID == wifiNetworks[j].ssid) {
        if (foundNetworkIndex == -1 || 
            (j == 0 && foundNetworkIndex != 0) || // Primary network found
            (j == foundNetworkIndex && rssi > bestRSSI)) { // Same network, better signal
          foundNetworkIndex = j;
          bestRSSI = rssi;
          Serial.printf("  ---> Found configured network: %s (index %d, RSSI: %d)\r\n", 
                     wifiNetworks[j].ssid, j, rssi);
        }
      }
    }
  }
  
  // If we found a preferred network, connect to it
  if (foundNetworkIndex != -1) {
    connectToWiFi(foundNetworkIndex);
  } else {
    Serial.println("No configured networks found - will retry in 30 seconds");
  }
}

void connectToWiFi(int networkIndex) {
  if (networkIndex < 0 || networkIndex >= numWifiNetworks) return;
  
  Serial.printf("Attempting to connect to: %s\r\n", wifiNetworks[networkIndex].ssid);
  
  if (WiFi.isConnected()) {
    WiFi.disconnect();
    delay(100);
  }
  
  WiFi.begin(wifiNetworks[networkIndex].ssid, wifiNetworks[networkIndex].password);
  
  wifiConnectStartTime = millis();
  currentSSIDIndex = networkIndex;
}

void manageWiFi() {
    static bool lastConnectionState = false;
    unsigned long now = millis();
    bool currentlyConnected = WiFi.isConnected();
    
    // Handle connection state changes
    if (currentlyConnected != lastConnectionState) {
        lastConnectionState = currentlyConnected;
        
        if (currentlyConnected && !isWifiConnected) {
            // Just connected
            isWifiConnected = true;
            
            Serial.printf("\r\nSuccessfully connected to: %s\r\n", wifiNetworks[currentSSIDIndex].ssid);
            Serial.printf("IP address: %s\r\n", WiFi.localIP().toString().c_str());
            Serial.printf("RSSI: %d dBm\r\n", WiFi.RSSI());
            
            digitalWrite(led, 0); // Turn off LED when connected
            
        } else if (!currentlyConnected && isWifiConnected) {
            // Just disconnected
            isWifiConnected = false;
            currentSSIDIndex = -1;
            
            Serial.println("WiFi connection lost - will start scanning");
            digitalWrite(led, 1); // Turn on LED when disconnected
            
            // Start scanning for networks again
            scanForWiFiNetworks();
        }
    }
    
    // Handle connection timeout (only when trying to connect)
    if (!currentlyConnected && currentSSIDIndex >= 0) {
        if (now - wifiConnectStartTime > WIFI_CONNECT_TIMEOUT) {
            Serial.printf("Connection to %s timed out\r\n", wifiNetworks[currentSSIDIndex].ssid);
            currentSSIDIndex = -1;
            
            // Try scanning again
            scanForWiFiNetworks();
        }
    }
    
    // Periodic scanning when disconnected (backup in case connection drops silently)
    if (!currentlyConnected && currentSSIDIndex == -1) {
        if (now - lastWifiScan > WIFI_SCAN_INTERVAL) {
            scanForWiFiNetworks();
        }
    }
}

void initializeWebServer() {
    Serial.println("Initializing web server...");
    
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send_P(200, "text/html", index_html);
    });
    server.on("/favicon.ico", HTTP_GET, handleFavicon);
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();
    
    Serial.println("Web server initialized");
}

void otaProgressCallback(unsigned int progress, unsigned int total) {
   Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
}

void enableOTA() {
    ArduinoOTA.setPort(3232);
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(otaPassword);
    
    ArduinoOTA.onStart([]() {
       otaInProgress = true;
       
       // Kick off all WebSocket clients before OTA starts
       Serial.printf("OTA Starting - Disconnecting %d WebSocket clients\r\n", ws.count());
       server.end();
       
       saveSessionDataToSPIFFS(); // Backup before OTA starts
       String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
       Serial.println("Start updating " + type);
    }).onEnd([]() {
       reboot();
    }).onProgress(otaProgressCallback)
    .onError([](ota_error_t error) {
       Serial.printf("Error[%u]: ", error);
       if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
       else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
       else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
       else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
       else if (error == OTA_END_ERROR) Serial.println("End Failed");
       reboot();
    });
    ArduinoOTA.begin();
    otaEnabled = true;
    Serial.println("OTA Enabled!");
}

void checkForNewDay(int deviceIndex, float currentYield) {
  // Only check if we have a previous valid yield value
  if (lastTodayYield[deviceIndex] >= 0) {
    // Detect new day: yield went from non-zero to zero
    if (lastTodayYield[deviceIndex] > 0 && currentYield == 0) {
      Serial.printf("%s MPPT yield reset: %.1f -> %.1f\r\n", 
                 deviceNames[deviceIndex], lastTodayYield[deviceIndex], currentYield);
      
      // Mark that we detected a new day
      if (!session.newDay) {
        session.newDay = true;
        Serial.println("NEW DAY: Flagged for reset - waiting for all MPPTs to confirm");
      }
    }
  }
  
  // Update the tracked value
  lastTodayYield[deviceIndex] = currentYield;
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>Solar System</title>
  <meta name="viewport" content="width=device-width, initial-scale=0.65, user-scalable=yes">
  <link rel="icon" type="image/svg+xml" href="/favicon.ico">
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #1e3c72 0%, #2a5298 100%);
      color: #ffffff;
      margin: 0;
      padding: 4px;
      min-height: 100vh;
      zoom: 0.95;
    }
    .container {
      max-width: 1080px;
      margin: 0 auto;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 11px;
      padding: 5px;
      backdrop-filter: blur(10px);
      box-shadow: 0 7px 29px rgba(0, 0, 0, 0.3);
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(270px, 1fr));
      gap: 4px;
      margin-bottom: 5px;
    }
    .card {
      background: rgba(255, 255, 255, 0.15);
      border-radius: 11px;
      padding: 18px;
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
    }
    .card h3 {
      margin-top: 0;
      font-size: 1.26rem;
      color: #ffd700;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .metric {
      display: flex;
      justify-content: space-between;
      margin: 9px 0;
      font-size: 1rem;
    }
    .value {
      font-weight: bold;
      color: #00ff88;
    }
    .gauge-container {
      display: flex;
      justify-content: stretch;
      gap: 5px;
      margin: 3px 0 5px 0;
      flex-wrap: wrap;
    }
    .gauge-wrapper {
      display: flex;
      flex-direction: column;
      align-items: center;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 9px;
      padding: 7px 4px;
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
      flex: 1 1 0;
      min-width: 0;
      min-height: 243px;
    }
    .gauge-label {
      font-size: 0.81rem;
      font-weight: bold;
      color: #ffd700;
      margin-top: 5px;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .gauge-value {
      font-size: 0.9rem;
      font-weight: bold;
      color: #00ff88;
      margin-top: 3px;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .gauge-time {
      font-size: 0.72rem;
      font-weight: bold;
      color: #88ddff;
      margin-top: 2px;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .graph-container {
      background: rgba(255, 255, 255, 0.1);
      border-radius: 7px;
      padding: 4px;
      margin: 4px 0;
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
    }
    .graph-container h3 {
      color: #ffd700;
      font-size: 0.9rem;
      margin-bottom: 4px;
      text-align: center;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .graph-container canvas {
      width: 100%;
      height: auto;
      border-radius: 7px;
      background: rgba(0, 0, 0, 0.2);
    }
    .status-indicator {
      display: inline-block;
      padding: 4px 11px;
      border-radius: 18px;
      font-size: 0.81rem;
      font-weight: bold;
      background: rgba(0, 255, 136, 0.3);
      border: 1px solid #00ff88;
    }
    .connection-status {
      position: fixed;
      top: 9px;
      right: 9px;
      padding: 7px 11px;
      border-radius: 14px;
      font-size: 0.72rem;
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
      body {
        padding: 4px;
        zoom: 1;
      }
      .container {
        padding: 6px;
        max-width: 100%;
      }
      .grid {
        grid-template-columns: 1fr;
        gap: 8px;
      }
      .gauge-container {
        gap: 8px;
        flex-direction: column;
        align-items: center;
      }
      .gauge-wrapper {
        padding: 12px 8px;
        width: 95%;
        max-width: none;
        min-width: auto;
        min-height: 320px;
      }
      .gauge-wrapper canvas {
        width: 240px !important;
        height: 240px !important;
      }
      .gauge-label {
        font-size: 0.9rem;
      }
      .gauge-value {
        font-size: 1.0rem;
      }
      .gauge-time {
        font-size: 0.8rem;
      }
      .card {
        padding: 8px;
      }
      .card h3 {
        font-size: 0.9rem;
      }
      .metric {
        font-size: 0.75rem;
      }
      .graph-container {
        padding: 6px;
      }
      .graph-container h3 {
        font-size: 0.9rem;
      }
    }
    @media (max-width: 480px) {
      body {
        padding: 3px;
      }
      .container {
        padding: 5px;
      }
      .gauge-container {
        gap: 6px;
      }
      .gauge-wrapper {
        padding: 10px 6px;
        width: 98%;
        min-height: 280px;
      }
      .gauge-wrapper canvas {
        width: 200px !important;
        height: 200px !important;
      }
      .gauge-label {
        font-size: 0.85rem;
      }
      .gauge-value {
        font-size: 0.95rem;
      }
      .gauge-time {
        font-size: 0.75rem;
      }
      .card {
        padding: 6px;
      }
      .metric {
        font-size: 0.7rem;
        margin: 2px 0;
      }
      .connection-status {
        top: 3px;
        right: 3px;
        padding: 4px 6px;
        font-size: 0.65rem;
      }
    }
  </style>
</head>
<body>
  <div class="connection-status" id="connectionStatus">Disconnected</div>
  
  <div class="container">
    <div class="gauge-container">
      <div class="gauge-wrapper">
        <canvas id="solarGauge" width="210" height="210"></canvas>
        <div class="gauge-label">Solar Power</div>
        <div class="gauge-value" id="solarGaugeValue">-- W</div>
        <div class="gauge-value" id="solarGaugeToday">-- kWh</div>
      </div>
      <div class="gauge-wrapper">
        <canvas id="socGauge" width="210" height="210"></canvas>
        <div class="gauge-label">Battery SoC</div>
        <div class="gauge-value" id="socGaugeValue">--%</div>
        <div class="gauge-value" id="socGaugeKwh">-- kWh</div>
      </div>
      <div class="gauge-wrapper">
        <canvas id="batteryGauge" width="210" height="210"></canvas>
        <div class="gauge-label">Battery Power</div>
        <div class="gauge-value" id="batteryGaugeValue">-- W</div>
        <div class="gauge-value" id="batteryTimeToGo">--</div>
      </div>
    </div>
    <div class="graph-container">
      <h3>Daily Solar Production</h3>
      <canvas id="solarChart" width="785" height="205"></canvas>
    </div>
    <div class="grid">
      <div class="card">
        <h3><center>Solar</center></h3>
        <div class="metric">
          <span>Total Today:</span>
          <span class="value" id="totalToday">-- kWh</span>
        </div>
        <div class="metric">
          <span>Session Time:</span>
          <span class="value" id="sessionTime">--</span>
        </div>
        <div class="metric">
          <span>Left Array:</span>
          <span class="value"><span id="leftPower">--</span> W <span class="status-indicator" id="leftStatus">--</span></span>
        </div>
        <div class="metric">
          <span>Right Array:</span>
          <span class="value"><span id="rightPower">--</span> W <span class="status-indicator" id="rightStatus">--</span></span>
        </div>
        <div class="metric">
          <span>Rear Array:</span>
          <span class="value"><span id="rearPower">--</span> W <span class="status-indicator" id="rearStatus">--</span></span>
        </div>
        <div class="metric">
          <span>Total Power:</span>
          <span class="value" id="totalPower">-- W</span>
        </div>
      </div>
      <div class="card">
        <h3><center>Battery</center></h3>
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
          <span>Capacity Used:</span>
          <span class="value" id="battCapacity">--</span>
        </div>
        <div class="metric">
          <span>Energy Remaining:</span>
          <span class="value" id="battKwh">--</span>
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
    
    var solarData = [];
    var timeLabels = [];
    var dataChunks = [];
    var active = false;
    var currentVersion = null;
    
    class RobustWebSocket {
        constructor() {
            this.ws = null;
            this.reconnectTimer = null;
            this.reconnectInterval = 1000;
            this.maxReconnectInterval = 30000;
            this.reconnectAttempts = 0;
            this.maxReconnectAttempts = 10;
            this.isIntentionallyClosed = false;
            this.lastDataTime = Date.now();
            this.connectionCheckTimer = null;
            this.heartbeatInterval = null;
            
            this.onOpen = this.onOpen.bind(this);
            this.onMessage = this.onMessage.bind(this);
            this.onClose = this.onClose.bind(this);
            this.onError = this.onError.bind(this);
            this.checkConnection = this.checkConnection.bind(this);
            
            this.initializeDailyData();
            
            document.addEventListener('visibilitychange', () => {
                if (document.hidden) {
                    this.onPageHidden();
                } else {
                    this.onPageVisible();
                }
            });
            
            window.addEventListener('beforeunload', () => {
                this.cleanup();
            });
            
            setTimeout(() => {
                this.connect();
                this.startConnectionCheck();
            }, 100);
        }
        
        initializeDailyData() {
            solarData = new Array(dataMins).fill(0);
            dataChunks = new Array(7).fill(null);
            timeLabels = [];
            for (let i = 0; i < dataMins; i++) {
                timeLabels.push(i + 'min');
            }
        }
        
        connect() {
            this.cleanup(false);
            
            if (this.isIntentionallyClosed) {
                console.log('WebSocket intentionally closed, not reconnecting');
                return;
            }
            
            if (this.reconnectAttempts >= this.maxReconnectAttempts) {
                console.error('Max reconnection attempts reached. Giving up.');
                this.updateConnectionStatus('Failed - Too many attempts');
                return;
            }
            
            try {
                console.log(`Attempting WebSocket connection (attempt ${this.reconnectAttempts + 1})`);
                this.ws = new WebSocket(`ws://${window.location.hostname}/ws`);
                
                this.ws.addEventListener('open', this.onOpen);
                this.ws.addEventListener('message', this.onMessage);
                this.ws.addEventListener('close', this.onClose);
                this.ws.addEventListener('error', this.onError);
                
            } catch (error) {
                console.error('Failed to create WebSocket:', error);
                this.scheduleReconnect();
            }
        }
        
        onOpen(event) {
            console.log('WebSocket connected successfully');
            this.reconnectAttempts = 0;
            this.reconnectInterval = 1000;
            this.lastDataTime = Date.now();
            
            this.clearReconnectTimer();
            
            this.updateConnectionStatus('Connected');
        }
        
        onMessage(event) {
            this.lastDataTime = Date.now();
            
            try {
                const data = JSON.parse(event.data);
                if (data.version) {
                    if (currentVersion === null) {
                        currentVersion = data.version;
                        console.log('Firmware version:', data.version);
                    } else if (currentVersion !== data.version) {
                        console.log('New firmware detected! Reloading page...');
                        setTimeout(() => location.reload(), 200);
                        return;
                    }
                }
                
                if (data.dailyDataChunk !== undefined) {
                    this.handleDailyDataChunk(data);
                } else if (data.dailyDataComplete) {
                    this.handleDailyDataComplete();
                } else {
                    this.handleLiveData(data);
                }
                
            } catch (error) {
                console.error('Error processing WebSocket message:', error);
                console.log('Raw message:', event.data);
            }
        }
        
        onClose(event) {
            console.log(`WebSocket connection closed. Code: ${event.code}, Reason: ${event.reason}`);            
            this.ws = null;
            this.updateConnectionStatus('Disconnected');
            if (!this.isIntentionallyClosed) {
                this.scheduleReconnect();
            }
        }
        
        onError(event) {
            console.error('WebSocket error', event);
        }
        
        scheduleReconnect() {
            this.clearReconnectTimer();
            
            if (this.isIntentionallyClosed) {
                return;
            }
            
            this.reconnectAttempts++;
            
            if (this.reconnectAttempts > this.maxReconnectAttempts) {
                console.error('Max reconnection attempts reached');
                this.updateConnectionStatus('Failed - Too many attempts');
                return;
            }
            
            const baseDelay = Math.min(this.reconnectInterval * Math.pow(1.5, this.reconnectAttempts - 1), this.maxReconnectInterval);
            const jitter = Math.random() * 1000;
            const delay = baseDelay + jitter;
            
            console.log(`Reconnecting in ${Math.round(delay)}ms (attempt ${this.reconnectAttempts})`);
            this.updateConnectionStatus(`Reconnecting in ${Math.round(delay/1000)}s`);
            
            this.reconnectTimer = setTimeout(() => {
                this.connect();
            }, delay);
        }
        
        clearReconnectTimer() {
            if (this.reconnectTimer) {
                clearTimeout(this.reconnectTimer);
                this.reconnectTimer = null;
            }
        }
        
        startConnectionCheck() {
            if (this.connectionCheckTimer) {
                clearInterval(this.connectionCheckTimer);
            }
            this.connectionCheckTimer = setInterval(this.checkConnection, 5000);
        }
        
        checkConnection() {
            if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
                return;
            }
            
            const timeSinceLastData = Date.now() - this.lastDataTime;
            
            if (timeSinceLastData > 15000) {
                console.log(`WebSocket appears dead - no data for ${timeSinceLastData} ms`);
                this.forceReconnect();
            }
        }
        
        forceReconnect() {
            console.log('Forcing WebSocket reconnection');
            this.isIntentionallyClosed = false;
            
            if (this.ws) {
                this.ws.close(1000, 'Forced reconnection');
            } else {
                this.connect();
            }
        }
        
        onPageHidden() {
            console.log('Page hidden - pausing reconnection attempts');
            this.clearReconnectTimer();
            
            if (this.connectionCheckTimer) {
                clearInterval(this.connectionCheckTimer);
                this.connectionCheckTimer = null;
            }
        }
        
        onPageVisible() {
            console.log('Page visible - resuming connection');
            this.startConnectionCheck();
            
            if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
                this.connect();
            }
        }
        
        cleanup(intentional = true) {
            this.isIntentionallyClosed = intentional;
            
            this.clearReconnectTimer();
            
            if (this.connectionCheckTimer) {
                clearInterval(this.connectionCheckTimer);
                this.connectionCheckTimer = null;
            }
            
            if (this.heartbeatInterval) {
                clearInterval(this.heartbeatInterval);
                this.heartbeatInterval = null;
            }
            
            if (this.ws) {
                this.ws.removeEventListener('open', this.onOpen);
                this.ws.removeEventListener('message', this.onMessage);
                this.ws.removeEventListener('close', this.onClose);
                this.ws.removeEventListener('error', this.onError);
                
                if (this.ws.readyState === WebSocket.OPEN) {
                    this.ws.close(1000, intentional ? 'Page unloading' : 'Reconnecting');
                }
                
                this.ws = null;
            }
        }
        
        handleDailyDataChunk(data) {
            if (data.chunkIndex !== undefined && data.chunkIndex < dataChunks.length) {
                dataChunks[data.chunkIndex] = data.dailyDataChunk;
                console.log(`Received chunk ${data.chunkIndex} with ${data.dailyDataChunk.length} items`);
            }
        }
        
        handleDailyDataComplete() {
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
        }
        
        handleLiveData(data) {
            if (data.session) {
                active = data.session.active;
            }
            
            updateGauges(data);
            
            if (data.battery) {
                document.getElementById('battVoltage').textContent = data.battery.voltage + ' V';
                document.getElementById('battCurrent').textContent = data.battery.current + ' A';
                document.getElementById('battPower').textContent = data.battery.power + ' W';
                document.getElementById('battCapacity').textContent = data.battery.ampHours + ' Ah';
                document.getElementById('battSOC').textContent = data.battery.soc + ' %';
                document.getElementById('battKwh').textContent = ((data.battery.soc / 100) * batteryKWH).toFixed(2) + ' kWh';
                
                const timeToGoFormatted = formatTimeToGo(data.battery.timeToGo);
                document.getElementById('battTimeToGo').innerHTML = timeToGoFormatted;
                document.getElementById('batteryTimeToGo').innerHTML = timeToGoFormatted;
            }
            
            if (data.Mppts) {
                const todayKwh = (parseFloat(data.solar.totalToday) / 1000).toFixed(2);
                document.getElementById('leftPower').textContent = data.Mppts.left.power || '--';
                document.getElementById('leftStatus').textContent = data.Mppts.left.status || '--';
                document.getElementById('rightPower').textContent = data.Mppts.right.power || '--';
                document.getElementById('rightStatus').textContent = data.Mppts.right.status || '--';
                document.getElementById('rearPower').textContent = data.Mppts.rear.power || '--';
                document.getElementById('rearStatus').textContent = data.Mppts.rear.status || '--';
                
                let totalPower = 0;
                if (data.Mppts.left && data.Mppts.left.power) totalPower += data.Mppts.left.power;
                if (data.Mppts.right && data.Mppts.right.power) totalPower += data.Mppts.right.power;
                if (data.Mppts.rear && data.Mppts.rear.power) totalPower += data.Mppts.rear.power;
                document.getElementById('totalPower').textContent = totalPower + ' W';
                document.getElementById('totalToday').textContent = todayKwh + ' kWh';
                
                if (data.session && data.session.currentIndex >= 0) {
                    document.getElementById('sessionTime').textContent = formatTimeToGo(data.session.currentIndex);
                } else {
                    document.getElementById('sessionTime').textContent = '--';
                }
                
                if (data.session && data.session.currentIndex >= 0 && data.session.currentIndex < solarData.length) {
                    if (totalPower > 0) {
                        solarData[data.session.currentIndex] = Math.max(solarData[data.session.currentIndex], totalPower);
                    }
                drawSolarChart();
                }
            }
        }
        
        updateConnectionStatus(status) {
            const statusElement = document.getElementById('connectionStatus');
            if (statusElement) {
                statusElement.textContent = status;
                statusElement.className = status.includes('Connected') ? 'connection-status connected' : 
                                        status.includes('Failed') ? 'connection-status disconnected' : 'connection-status disconnected';
            }
        }
        
        disconnect() {
            this.cleanup(true);
        }
        
        reconnect() {
            this.isIntentionallyClosed = false;
            this.reconnectAttempts = 0;
            this.connect();
        }
    }
    
    function formatTimeToGo(minutes) {
        if (minutes === 65535 || minutes >= 14400) {
            return '&infin;';
        }
        
        if (minutes >= 1440) {
            const days = Math.floor(minutes / 1440);
            const remainingHours = Math.floor((minutes % 1440) / 60);
            
            if (days === 1) {
                if (remainingHours === 0) return '1 day';
                if (remainingHours === 1) return '1 day 1 hour';
                return '1 day ' + remainingHours + ' hours';
            } else {
                if (remainingHours === 0) return days + ' days';
                if (remainingHours === 1) return days + ' days 1 hour';
                return days + ' days ' + remainingHours + ' hours';
            }
        } else if (minutes >= 60) {
            const hours = Math.floor(minutes / 60);
            const mins = minutes % 60;
            
            if (hours === 1) {
                if (mins === 0) return '1 hour';
                if (mins === 1) return '1 hour 1 min';
                return '1 hour ' + mins + ' mins';
            } else {
                if (mins === 0) return hours + ' hours';
                if (mins === 1) return hours + ' hours 1 min';
                return hours + ' hours ' + mins + ' mins';
            }
        } else {
            if (minutes === 1) return '1 min';
            return minutes + ' mins';
        }
    }
    
    function drawSolarChart() {
        const canvas = document.getElementById('solarChart');
        const ctx = canvas.getContext('2d');
        const width = canvas.width;
        const height = canvas.height;
        
        ctx.clearRect(0, 0, width, height);
        
        const xpadding = 44;
        const ypadding = 18;
        const chartWidth = width - 2 * xpadding;
        const chartHeight = height - 2 * ypadding;
        
        let lastDataIndex = 0;
        for (let i = solarData.length - 1; i >= 0; i--) {
            if (solarData[i] > 0) {
                lastDataIndex = i;
                break;
            }
        }
        
        const chartRange = dataMins;
        const maxPower = solarMaxWatts;
        
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.2)';
        ctx.lineWidth = 1;
        
        for (let i = 0; i <= 4; i++) {
            const y = ypadding + (i * chartHeight / 4);
            ctx.beginPath();
            ctx.moveTo(xpadding, y);
            ctx.lineTo(width - xpadding, y);
            ctx.stroke();
            
            ctx.fillStyle = 'rgba(255, 255, 255, 0.95)';
            ctx.font = '12px Arial';
            ctx.textAlign = 'right';
            const powerLabel = ((4 - i) * maxPower / 4 / 1000).toFixed(1) + 'kW';
            ctx.fillText(powerLabel, xpadding - 5, y + 4);
        }
        
        ctx.strokeStyle = '#00ff88';
        ctx.lineWidth = 1;
        ctx.beginPath();
        
        let hasData = false;
        let firstPoint = true;
        
        for (let i = 0; i < chartRange; i++) {
            const x = xpadding + (i * chartWidth / chartRange);
            const y = ypadding + chartHeight - (solarData[i] / maxPower * chartHeight);
            
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
            
            ctx.lineTo(xpadding + (chartRange * chartWidth / chartRange), height - ypadding);
            ctx.lineTo(xpadding, height - ypadding);
            ctx.closePath();
            ctx.fillStyle = 'rgba(0, 255, 136, 0.2)';
            ctx.fill();
        }
        
        if (active && lastDataIndex > 0) {
            const currentX = xpadding + (lastDataIndex * chartWidth / chartRange);
            
            ctx.strokeStyle = '#ff6666';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(currentX, ypadding);
            ctx.lineTo(currentX, height - ypadding);
            ctx.stroke();
        }
    }
    
    function drawGauge(canvas, value, min, max, unit, color, rotated = false) {
        const ctx = canvas.getContext('2d');
        const centerX = canvas.width / 2;
        const centerY = canvas.height / 2;
        const radius = canvas.width / 2 - 10;
        
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
            angle = (normalizedValue * 1.7 * Math.PI) - (1.35 * Math.PI);
        } else {
            const normalizedValue = Math.min(Math.max(value / max, 0), 1);
            angle = (normalizedValue * 1.7 * Math.PI) - (1.35 * Math.PI);
        }
        
        if (!rotated) {
            ctx.beginPath();
            ctx.arc(centerX, centerY, radius, -1.35 * Math.PI, angle);
            ctx.strokeStyle = color;
            ctx.lineWidth = 8;
            ctx.stroke();
        } else {
            const range = max - min;
            const zeroPosition = (-min) / range;
            const zeroAngle = (zeroPosition * 1.7 * Math.PI) - (1.35 * Math.PI);
            
            let arcColor;
            if (value < 0) {
                arcColor = '#ff6666';
            } else {
                arcColor = '#66ff66';
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
        const needleLength = radius - 35;
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
        
        for (let i = 0; i <= 10; i++) {
            const markAngle = (-1.35 * Math.PI) + (i * 1.7 * Math.PI / 10);
            const markValue = rotated ? (min + (i * (max - min) / 10)) : (i * max / 10);
            
            ctx.beginPath();
            const startRadius = radius - 12;
            const endRadius = radius - 4;
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
        let solarPower = 0;
        let solarToday = 0;
        
        if (data.Mppts) {
            if (data.Mppts.left && data.Mppts.left.power) solarPower += data.Mppts.left.power;
            if (data.Mppts.right && data.Mppts.right.power) solarPower += data.Mppts.right.power;
            if (data.Mppts.rear && data.Mppts.rear.power) solarPower += data.Mppts.rear.power;
        }
        
        if (data.solar && data.solar.totalToday !== undefined) {
            solarToday = parseFloat(data.solar.totalToday) / 1000;
        } else {
            const totalTodayText = document.getElementById('totalToday').textContent;
            if (totalTodayText && totalTodayText !== '-- kWh' && totalTodayText !== '--') {
                solarToday = parseFloat(totalTodayText.replace(' kWh', '')) || 0;
            }
        }
        
        drawGauge(document.getElementById('solarGauge'), solarPower, 0, solarMaxWatts, 'W', '#00ff88');
        document.getElementById('solarGaugeValue').textContent = solarPower + ' W';
        document.getElementById('solarGaugeToday').textContent = solarToday.toFixed(2) + ' kWh';
        
        let soc = 0;
        if (data.battery && data.battery.soc) {
            soc = parseFloat(data.battery.soc);
        }
        let socColor = soc > 50 ? '#00ff88' : soc > 20 ? '#ffaa00' : '#ff4444';
        drawGauge(document.getElementById('socGauge'), soc, 0, 100, '%', socColor);
        document.getElementById('socGaugeValue').textContent = soc.toFixed(1) + '%';
        document.getElementById('socGaugeKwh').textContent = ((soc / 100) * batteryKWH).toFixed(2) + ' kWh';
        
        let battPower = 0;
        if (data.battery && data.battery.power) {
            battPower = parseFloat(data.battery.power);
        }
        let battColor = battPower > 0 ? '#ff6666' : '#66ff66';
        drawGauge(document.getElementById('batteryGauge'), battPower, -battMaxWatts, battMaxWatts, 'W', battColor, true);
        document.getElementById('batteryGaugeValue').textContent = battPower.toFixed(0) + ' W';
    }
    
    let robustWS = null;
    
    window.addEventListener('load', function() {
        window.pageLoadTime = Date.now();
        
        drawSolarChart();
        
        if (typeof WebSocket === 'undefined') {
            console.log('ERROR: WebSocket not supported by this browser!');
            document.getElementById('connectionStatus').textContent = 'WebSocket Not Supported';
            document.getElementById('connectionStatus').className = 'connection-status disconnected';
            return;
        }
        
        robustWS = new RobustWebSocket();
        
        setInterval(function() {
            drawSolarChart();
        }, 60000);
    });
  </script>
</body>
</html>
)rawliteral";

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch(type) {
    case WS_EVT_CONNECT:
      // Deny new connections during OTA
      if (otaInProgress) {
        client->close(1012, "Service Restarting");
        return;
      }
      
      // Deny connections if memory is low
      if (ESP.getFreeHeap() < 30000) {
        Serial.printf("Rejecting WebSocket client due to low memory (%d bytes)\r\n", ESP.getFreeHeap());
        client->close(1013, "Low Memory");
        return;
      }
            
      Serial.printf("WebSocket client #%u connected from %s (Free heap: %d)\r\n", 
                 client->id(), client->remoteIP().toString().c_str(), ESP.getFreeHeap());
      
      // Send daily data to new client only if we have good memory
      if (ESP.getFreeHeap() > 30000) {
        sendDailySolarDataAsync(client);
      } else {
        Serial.println("Skipping daily data send due to low memory");
      }
      break;
      
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected (Free heap: %d)\r\n", 
                 client->id(), ESP.getFreeHeap());
      // Force cleanup after disconnect to reclaim memory immediately
      ws.cleanupClients();
      break;
      
    case WS_EVT_ERROR:
      Serial.printf("WebSocket client #%u error(%u): %s\r\n", client->id(), *((uint16_t*)arg), (char*)data);
      break;
      
    case WS_EVT_DATA:
      // Ignore any data during OTA or low memory
      if (otaInProgress || ESP.getFreeHeap() < 20000) {
        return;
      }
      break;
      
    default:
      break;
  }
}

void handleFavicon(AsyncWebServerRequest *request) {
  static const char svg[] PROGMEM = 
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 48 48\">"
    "<polygon points=\"24,2 26,10 22,10\" fill=\"#ff8800\"/>"
    "<polygon points=\"46,24 38,22 38,26\" fill=\"#ff8800\"/>"
    "<polygon points=\"24,46 22,38 26,38\" fill=\"#ff8800\"/>"
    "<polygon points=\"2,24 10,26 10,22\" fill=\"#ff8800\"/>"
    "<polygon points=\"36.5,11.5 32,16 29,13\" fill=\"#ff8800\"/>"
    "<polygon points=\"36.5,36.5 32,32 35,29\" fill=\"#ff8800\"/>"
    "<polygon points=\"11.5,36.5 16,32 19,35\" fill=\"#ff8800\"/>"
    "<polygon points=\"11.5,11.5 16,16 13,19\" fill=\"#ff8800\"/>"
    "<circle cx=\"24\" cy=\"24\" r=\"14\" fill=\"#ff8800\"/>"
    "<circle cx=\"24\" cy=\"24\" r=\"11\" fill=\"#ffdd00\"/>"
    "</svg>";
  
  request->send_P(200, "image/svg+xml", svg);
}

void sendDailySolarDataAsync(AsyncWebSocketClient *client) {
    const int chunkSize = 120;
    
    if (!client || !client->canSend()) {
        return;
    }
    
    if (ESP.getFreeHeap() < 30000) {
        Serial.printf("Insufficient memory for daily data send (%d bytes)\r\n", ESP.getFreeHeap());
        return;
    }
    
    static char dataBuffer[2000];
    
    for (int chunk = 0; chunk < 7; chunk++) {
        int pos = snprintf(dataBuffer, sizeof(dataBuffer),
            "{\"dailyDataChunk\":[");
        
        int startIndex = chunk * chunkSize;
        int endIndex = min((chunk + 1) * chunkSize, DATA_MINS);
        
        for (int i = startIndex; i < endIndex && pos < sizeof(dataBuffer) - 50; i++) {
            if (i > startIndex) {
                pos += snprintf(dataBuffer + pos, sizeof(dataBuffer) - pos, ",");
            }
            pos += snprintf(dataBuffer + pos, sizeof(dataBuffer) - pos, "%d", session.daily[i]);
        }
        
        snprintf(dataBuffer + pos, sizeof(dataBuffer) - pos,
            "],\"chunkIndex\":%d,\"totalChunks\":7}", chunk);

        if (client->canSend()) {
            client->text(dataBuffer);
            delay(10);
        } else {
            Serial.printf("Skipping chunk %d due to client unavailable\r\n", chunk);
            break;
        }
    }
    
    if (client && client->canSend()) {
        client->text("{\"dailyDataComplete\":true}");
    }
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
    
    loadSessionDataFromSPIFFS();
}

void initializeFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  Serial.println("SPIFFS Mounted Successfully");
}

void saveSessionDataToSPIFFS() {
    session.checksum = session.index ^ session.runTime ^ 0xa5;
    
    File file = SPIFFS.open("/solarSession.bin", "w");
    if (file) {
        file.write((uint8_t*)&session, sizeof(SessionData));
        file.flush(); // Force write
        file.close();
        
        Serial.printf("Session Data saved to SPIFFS: Active=%s, Index=%d\r\n", session.active ? "true" : "false", session.index);
        backupTimer = 0;
    } else {
        Serial.println("Failed to save session backup to SPIFFS!");
    }
}

void loadSessionDataFromSPIFFS() {
    File file = SPIFFS.open("/solarSession.bin", "r");
    if (!file) {
        Serial.println("No SPIFFS session backup found, starting fresh");
        initializeSessionData();
        return;
    }
    
    if (file.size() != sizeof(SessionData)) {
        Serial.printf("Invalid session backup size: %d (expected %d)\r\n", file.size(), sizeof(SessionData));
        file.close();
        initializeSessionData();
        return;
    }
    
    file.readBytes((char*)&session, sizeof(SessionData));
    file.close();
    
    // Verify checksum
    if (session.checksum != (session.index ^ session.runTime ^ 0xa5)) {
        Serial.println("Corrupted session backup, starting fresh");
        initializeSessionData();
        return;
    }
    
    Serial.printf("Valid Session Data in SPIFFS Restored: Active=%s, Index=%d\r\n", session.active ? "true" : "false", session.index);
    
    Serial.printf("Last boot ran for %u minutes!\r\n", session.runTime);
}

void initializeSessionData() {
    Serial.println("Initializing fresh session data!");
    
    // Clear all day data
    for (int i = 0; i < DATA_MINS; i++) {
        session.daily[i] = 0;
    }
    
    session.active = false;
    session.index = 0;
    session.newDay = false;
}

void updateData() {
    static unsigned long lastUpdateTime = 0;
    
    if (deviceDataMutex == NULL || millis() - lastUpdateTime < UPDATE_INTERVAL) {
        return;
    }

    // Take mutex once for entire operation
    if (xSemaphoreTake(deviceDataMutex, 50 / portTICK_PERIOD_MS) != pdTRUE) {
        return;  // Couldn't get lock
    }
        
    // Collect battery data
    state.batteryValid = batteryDevice.valid && (millis() - batteryDevice.lastUpdate < 10000);
    if (state.batteryValid) {
        state.battVoltage = batteryDevice.voltage;
        state.battCurrent = batteryDevice.current;
        state.battPower = batteryDevice.power;
        state.battAmpHours = batteryDevice.ampHours;
        state.battSoc = batteryDevice.soc;
        state.battTimeToGo = batteryDevice.timeToGo;
    } else {
        // Clear battery data if not valid
        state.battVoltage = 0;
        state.battCurrent = 0;
        state.battPower = 0;
        state.battAmpHours = 0;
        state.battSoc = 0;
        state.battTimeToGo = 0;
    }
    
    // Collect MPPT data
    uint16_t newTotalPower = 0;
    float newTotalToday = 0;
    
    for (int i = 0; i < 3; i++) {
        state.mpptValid[i] = solarDevices[i].valid && (millis() - solarDevices[i].lastUpdate < 10000);
        
        if (state.mpptValid[i]) {
            state.mpptPowers[i] = solarDevices[i].power;
            state.mpptStatuses[i] = statTxt[solarDevices[i].state];
            newTotalPower += solarDevices[i].power;
            newTotalToday += solarDevices[i].todayYield;
        } else {
            state.mpptPowers[i] = 0;
            state.mpptStatuses[i] = "?";
        }
    }
    
    state.totalPower = newTotalPower;
    state.totalToday = newTotalToday;
        
    // Update daily data if session is active
    if (session.active && session.index < DATA_MINS) {
        if (session.daily[session.index] < state.totalPower) {
            session.daily[session.index] = state.totalPower;
        }
    }
    
    lastUpdateTime = millis();
    state.lastUpdate = lastUpdateTime;
    
    xSemaphoreGive(deviceDataMutex);
        
    // Handle console printing
    if (printData) {
        Serial.printf("Battery:          %6.2fV  %6.2fA  %6.0fW  %5.1fAh  %5.1f%%   %dm\r\n",
            state.battVoltage, state.battCurrent, state.battPower,
            state.battAmpHours, state.battSoc, state.battTimeToGo);
        Serial.printf("Solar Total:      %6dW  %6.0fWh  %s  Session: %s (%dm)\r\n",
            state.totalPower, state.totalToday, state.mpptStatuses[0], 
            session.active ? "Active" : "Inactive", session.index);
        printData = 0;
    }
    
    // Skip WebSocket if conditions not met
    if (otaInProgress || ESP.getFreeHeap() < 30000 || ws.count() == 0) {
        return;
    }
    
    // Build version hash
    static uint32_t versionHash = 0;
    if (versionHash == 0) {
        const char* buildTime = __TIMESTAMP__;
        for (int i = 0; buildTime[i]; i++) {
            versionHash = versionHash * 31 + buildTime[i];
        }
    }
    
    static char wsBuffer[1000];
    
    int pos = snprintf(wsBuffer, sizeof(wsBuffer),
        "{\"version\":%u,"
        "\"session\":{\"active\":%s,\"currentIndex\":%d}",
        versionHash,
        session.active ? "true" : "false",
        session.index
    );
    
    // Add battery data if valid
    if (state.batteryValid && pos < sizeof(wsBuffer) - 300) {
        pos += snprintf(wsBuffer + pos, sizeof(wsBuffer) - pos,
            ",\"battery\":{\"voltage\":\"%.2f\",\"current\":\"%.2f\",\"power\":\"%.0f\","
            "\"ampHours\":\"%.1f\",\"soc\":\"%.1f\",\"timeToGo\":%d}",
            state.battVoltage, state.battCurrent, state.battPower, 
            state.battAmpHours, state.battSoc, state.battTimeToGo
        );
    }
    
    // Add MPPT data
    if (pos < sizeof(wsBuffer) - 200) {
        pos += snprintf(wsBuffer + pos, sizeof(wsBuffer) - pos,
            ",\"Mppts\":{"
            "\"left\":{\"power\":%d,\"status\":\"%s\"},"
            "\"right\":{\"power\":%d,\"status\":\"%s\"},"
            "\"rear\":{\"power\":%d,\"status\":\"%s\"}"
            "}",
            state.mpptPowers[0], state.mpptStatuses[0],
            state.mpptPowers[1], state.mpptStatuses[1],
            state.mpptPowers[2], state.mpptStatuses[2]
        );
    }
    
    // Add solar summary and close JSON
    if (pos < sizeof(wsBuffer) - 50) {
        snprintf(wsBuffer + pos, sizeof(wsBuffer) - pos,
            ",\"solar\":{\"totalToday\":\"%.0f\"}}", state.totalToday
        );
    }

    ws.textAll(wsBuffer);
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      #define manDataSizeMax 31
      if (advertisedDevice.haveManufacturerData() == true) {
        uint8_t manCharBuf[manDataSizeMax+1];
        std::string manData = advertisedDevice.getManufacturerData();
        int manDataSize = manData.length();
        if (manDataSize > manDataSizeMax) {
          return;  // Skip oversized packets
        }
        manData.copy((char *)manCharBuf, manDataSize);
        victronManufacturerData * vicData=(victronManufacturerData *)manCharBuf;
        
        if (vicData->vendorID!=0x02e1 || vicData->victronRecordType < 1 || vicData->victronRecordType > 2 ) {
          return;
        }

        digitalWrite(led, 1);

        if (advertisedDevice.haveName()) {
          strncpy(savedDeviceName, advertisedDevice.getName().c_str(), sizeof(savedDeviceName) - 1);
          savedDeviceName[sizeof(savedDeviceName) - 1] = '\0';
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

        if (deviceDataMutex != NULL && xSemaphoreTake(deviceDataMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
          
          if (vicData->victronRecordType == 1) {
            // MPPT data processing
            victronMpptData * victronData = (victronMpptData *) outputData;
            uint8_t deviceState = victronData->deviceState;
            float batteryVoltage = float(victronData->batteryVoltage) * 0.01;
            float batteryCurrent = float(victronData->batteryCurrent) * 0.1;
            float todayYield = float(victronData->todayYield) * 0.01 * 1000;
            uint16_t inputPower = victronData->inputPower;
            
            if (deviceState == 245) deviceState = 8;
            if (deviceState > 8) deviceState = 1;
            
            if (matchingKeyIndex >= 0 && matchingKeyIndex < 3) {
              checkForNewDay(matchingKeyIndex, todayYield);
              
              solarDevices[matchingKeyIndex].voltage = batteryVoltage;
              solarDevices[matchingKeyIndex].current = batteryCurrent;
              solarDevices[matchingKeyIndex].power = inputPower;
              solarDevices[matchingKeyIndex].todayYield = todayYield;
              solarDevices[matchingKeyIndex].state = deviceState;
              solarDevices[matchingKeyIndex].lastUpdate = millis();
              solarDevices[matchingKeyIndex].valid = true;
            }
          } else {
            // Battery data processing
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
          
          xSemaphoreGive(deviceDataMutex); // Release mutex
        }
      }
    digitalWrite(led, 0);
    }
};

void bleScanTask(void *parameter) {
    while (true) {
        // Check if we should continue scanning
        if (bleScanTaskHandle == NULL) {
            Serial.println("BLE Task: Termination requested");
            break;
        }
        
        // Perform BLE scan
        BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
        pBLEScan->clearResults();
        
        // Signal that BLE data might have been updated
        bleDataUpdated = true;
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
        
        // Yield to watchdog
        yield();
    }
    vTaskDelete(NULL); // Delete this task
}

void manageDailySession() {
  if (session.active) {
    // Check if we should end the session
    if (state.totalPower < 5) { // Very low power
        lowPowerCount++;
        if (lowPowerCount >= 10) {
            Serial.println("SESSION END: 10 minutes of low power - ending session");
            session.active = false;
            return; // Don't process data when session ends
        }
    } else {
        lowPowerCount = 0; // Reset when power returns
    }
    
    // Check if we hit the time limit
    if (session.index >= DATA_MINS - 1) {
        Serial.println("SESSION END: Reached maximum session length!");
        session.active = false;
        return;
    }
  }
  
  // Session management logic
  if (!session.active && state.totalPower > 5) {
    // Start new session - first light detected
    Serial.println("Starting new solar session - first light detected");
    session.active = true;
    session.index = 0;
  }
  
  if (session.active && session.index + 1 < DATA_MINS) {
      session.index++;
  }

  if (!session.newDay) return;
  
  // Check if ALL MPPTs have reset to 0 (or are at least very low)
  bool allMpptsReset = true;
  int validMppts = 0;
  
  for (int i = 0; i < 3; i++) {
    if (solarDevices[i].valid && (millis() - solarDevices[i].lastUpdate < 30000)) {
      validMppts++;
      if (solarDevices[i].todayYield > 10) { // More than 10Wh = probably yesterday's data
        allMpptsReset = false;
      }
    }
  }
  
  if (validMppts == 0) {
    Serial.println("NEW DAY RESET: No valid MPPT data from any unit! - proceeding anyway");
    allMpptsReset = true; // If we can't see MPPTs, assume they reset
  }
  
  if (allMpptsReset) {
    Serial.printf("NEW DAY CONFIRMED: %d MPPTs have reset!\r\n", validMppts);

    initializeSessionData();

    reboot();
  }
}

void setup() {
    pinMode(led, OUTPUT);
    digitalWrite(led, 1);
    session.runTime = 0;
    Serial.begin(115200);  
    Serial.println();
    Serial.printf("%s - Built %s\r\n", hostname, __TIMESTAMP__);
    Serial.println();

    // Get reset reason
    esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.printf("Reset reason: %d (%s)\r\n", resetReason, getResetReasonString(resetReason));

    heap_caps_check_integrity_all(true);
    
    // Initialize file system FIRST
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed!");
    } else {
        Serial.println("SPIFFS Mounted Successfully");
    }
    
    initializeDevices();
    setupWiFi();  
    
    // Create mutex with error checking
    deviceDataMutex = xSemaphoreCreateMutex();
    if (deviceDataMutex == NULL) {
        Serial.println("ERROR: Failed to create device data mutex!");
    }
    
    // Initialize BLE
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    
    // Create BLE task with error logging
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        bleScanTask,
        "BLE_Scan_Task",
        8192,
        NULL,
        1,
        &bleScanTaskHandle,
        1
    );
    
    if (taskCreated != pdPASS) {
        Serial.println("✗ ERROR: Failed to create BLE scanning task!");
        bleScanTaskHandle = NULL;
    }
    
    Serial.println("Setup complete!");
}

void loop() {
    if (otaEnabled) {
        ArduinoOTA.handle();
    }

    if (digitalRead(button) == 0 && otaEnabled == false) {
      enableOTA();          // Press button to Enable OTA so we can avoid it's memory issues
    }
    
    checkMemoryHealth();
    monitorBLETask();
    
    // Process console commands
    while (Serial.available()) {
        uint8_t gc = Serial.read();
        if (gc == 13) {
            printData = 1;
        } else if (gc == 1) {    // CTRL-A
            reboot();
        } else if (gc == 'o') {  // Enable OTA
            enableOTA();
        } else if (gc == 't') { // BLE task status
            Serial.printf("Main task stack: %d bytes remaining\r\n", uxTaskGetStackHighWaterMark(NULL));
            if (bleScanTaskHandle) {
                Serial.printf("BLE task stack: %d bytes remaining\r\n", uxTaskGetStackHighWaterMark(bleScanTaskHandle));
                Serial.println("BLE task: Running");
            } else {
                Serial.println("BLE task: DEAD");
            }
        }
    }
            
    manageWiFi();
    updateData();
    
    // Tasks to run every minute:
    if (millis() - minTimer >= 60000) {
        minTimer = millis();

        session.runTime++;
        
        manageDailySession();

        Serial.printf("Up %u mins - Session: %s, Index: %d, Power: %u W, SoC: %3.0f%% - ", session.runTime, session.active ? "Active" : "Inactive",
              session.index, state.totalPower, state.battSoc);
        Serial.printf("Memory: Free=%d, Min=%d, WS clients=%d, Loops=%d\r\n", ESP.getFreeHeap(), minFreeHeap, ws.count(), loopCount);
        loopCount = 0;

        if (session.active) {
            backupTimer++;
            if (backupTimer >= 5) {
              saveSessionDataToSPIFFS();
            }
        }
    }

    loopCount++;
    
    yield();
    
    vTaskDelay(5 / portTICK_PERIOD_MS);
}
