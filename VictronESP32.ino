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
#include "Telnet.h"

// WiFi Configuration - Primary and Secondary SSIDs
const char* stssid = "SSID1";         // Primary WiFi network
const char* stkey = "PASS1";          // Primary WiFi password
const char* stssid2 = "SSID2";        // Secondary WiFi network
const char* stkey2 = "PASS2";         // Secondary WiFi password
const char* apssid = "ESP32-Victron";
const char* apkey = "PASS";

const char* otaPassword = "PASS";

const char* mpptNames[] = {"left", "right", "rear"};
const char* deviceNames[] = {"LEFT", "RIGHT", "REAR", "BATT"};

#define BATT_KWH 9.0           // Size of your battery in kWh
#define BATT_MAX_WATTS 1500    // Max expected battery charge/discharge for analog gauge
#define SOLAR_MAX_WATTS 2000   // Max Total Wattage you expect to get in most cases
#define DATA_MINS 840          // 14 hours of data - Needs to be a multiple of 120!
#define led         2          // GPIO for LED

extern const char index_html[] PROGMEM;
WiFiClient clientNode;
WiFiUDP wifiUDPServer;
BLEScan *pBLEScan;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// RTC SRAM storage (survives warm reboots and OTA updates)
RTC_DATA_ATTR uint16_t rtc_dailySolarData[DATA_MINS];
RTC_DATA_ATTR bool rtc_sessionActive = false;
RTC_DATA_ATTR int rtc_currentDataIndex = 0;
RTC_DATA_ATTR unsigned long rtc_sessionStartTime = 0;
RTC_DATA_ATTR uint32_t rtc_dataValid = 0; // Magic number for validation
RTC_DATA_ATTR unsigned long rtc_lastBootTime = 0;
RTC_DATA_ATTR bool rtc_newDayDetected = false;
RTC_DATA_ATTR unsigned long rtc_newDayDetectedTime = 0;
RTC_DATA_ATTR unsigned long rtc_lastMinuteUpdate = 0;

// Working copies (point to RTC SRAM data)
uint16_t* dailySolarData = rtc_dailySolarData;
bool& sessionActive = rtc_sessionActive;
int& currentDataIndex = rtc_currentDataIndex;
unsigned long& sessionStartTime = rtc_sessionStartTime;
bool& newDayDetected = rtc_newDayDetected;
unsigned long& newDayDetectedTime = rtc_newDayDetectedTime;
unsigned long& lastMinuteUpdate = rtc_lastMinuteUpdate;

#define RTC_DATA_MAGIC 0xDEADBEEF
const unsigned long BACKUP_SAVE_INTERVAL = 3600000; // Backup to SPIFFS every hour (3,600,000ms)
bool needsRestore = false; // Flag to indicate we need to restore from SPIFFS

#define STRING(x) #x
#define STRINGIFY(x) STRING(x)

// BLE Task Management
TaskHandle_t bleScanTaskHandle = NULL;
bool bleDataUpdated = false;
SemaphoreHandle_t deviceDataMutex = NULL; // For thread-safe access to device data

// Daily solar data storage - now using RTC SRAM
// (RTC SRAM variables declared above)
unsigned long lastDataSave = 0;
unsigned long lastBackupSave = 0;
const unsigned long SAVE_INTERVAL = 60000; // Save every minute (now just marks data as changed)
const unsigned long FLASH_SAVE_INTERVAL = 300000; // Legacy - now used for backup triggers
const unsigned long MINUTE_INTERVAL = 60000; // Update data every minute
bool allSolarOff = true;
bool printData = 0;
bool dataChangedSinceLastSave = false;
bool dataChangedSinceLastBackup = false;
bool otaInProgress = false;
bool rebootRequested = false;
unsigned long bootTime = 0;
const unsigned long BOOT_GRACE_PERIOD = 120000; // 2 minutes grace period after boot
static float lastTodayYield[3] = {-1, -1, -1}; // -1 = not initialized
static unsigned long lowPowerStart = 0;

// Memory monitoring
size_t minFreeHeap = SIZE_MAX;

// Grab your Victron Bluetooth Encryption keys from the Victron phone app (iOS/Android)
// under "Product Info", click the "Show" button under "Encryption Data", then add here:
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

// WiFi network credentials structure
struct WiFiNetwork {
  const char* ssid;
  const char* password;
};

WiFiNetwork wifiNetworks[] = {
  {stssid, stkey},    // Primary network
  {stssid2, stkey2}   // Secondary network  
};

const int numWifiNetworks = sizeof(wifiNetworks) / sizeof(wifiNetworks[0]);

// Storage for device data
SolarData solarDevices[3]; // LEFT, RIGHT, REAR
BatteryData batteryDevice;
unsigned long lastDisplayUpdate = 0;
unsigned long lastWebSocketUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000; // 1 second
const unsigned long WEBSOCKET_UPDATE_INTERVAL = 1000; // 1 second

uint16_t loopCount = 0;
int keyBits=128;
int scanTime = 1;
char savedDeviceName[32];
boolean needServerInit = false;
uint8_t cnt = 0;
uint8_t SSID[32];
uint8_t WPA2Key[64];
uint32_t timer;
int32_t RSSI;
bool wifiScanInProgress = false;
unsigned long lastWifiScan = 0;
const unsigned long WIFI_SCAN_INTERVAL = 30000; // Scan every 30 seconds when in AP mode
const unsigned long WIFI_CONNECT_TIMEOUT = 15000; // 15 seconds to connect
unsigned long wifiConnectStartTime = 0;
int currentSSIDIndex = -1; // -1 = none, 0 = primary, 1 = secondary
bool apModeActive = false;
boolean isWifiConnected = false;

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

// Function to scan for available WiFi networks
void scanForWiFiNetworks() {
  if (wifiScanInProgress) return;
  
  Serial.println("Scanning for WiFi networks...");
  wifiScanInProgress = true;
  lastWifiScan = millis();
  
  int n = WiFi.scanNetworks();
  wifiScanInProgress = false;
  
  if (n == 0) {
    Serial.println("No networks found");
    return;
  }
  
  Serial.printf("Found %d networks:\n", n);
  
  int foundNetworkIndex = -1;
  int bestRSSI = -999;
  
  for (int i = 0; i < n; i++) {
    String foundSSID = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    
    Serial.printf("  %d: %s (%d dBm) %s\n", i, foundSSID.c_str(), rssi, 
               WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " " : "*");
    
    // Check if this is one of our configured networks
    for (int j = 0; j < numWifiNetworks; j++) {
      if (foundSSID == wifiNetworks[j].ssid) {
        // Prefer networks with better signal, but prioritize primary network
        if (foundNetworkIndex == -1 || 
            (j == 0 && foundNetworkIndex != 0) || // Primary network found
            (j == foundNetworkIndex && rssi > bestRSSI)) { // Same network, better signal
          foundNetworkIndex = j;
          bestRSSI = rssi;
          Serial.printf("  -> Found configured network: %s (index %d, RSSI: %d)\n", 
                     wifiNetworks[j].ssid, j, rssi);
        }
      }
    }
  }
  
  // If we found a preferred network and we're not already connected to it
  if (foundNetworkIndex != -1) {
    if (!WiFi.isConnected() || currentSSIDIndex != foundNetworkIndex) {
      connectToWiFi(foundNetworkIndex);
    }
  } else {
    Serial.println("No configured networks found");
    if (!apModeActive) {
      startAPMode();
    }
  }
}

// Function to connect to a specific WiFi network
void connectToWiFi(int networkIndex) {
  if (networkIndex < 0 || networkIndex >= numWifiNetworks) return;
  
  Serial.printf("Attempting to connect to: %s\n", wifiNetworks[networkIndex].ssid);
  
  // If we're currently in AP mode, stop it
  if (apModeActive) {
    Serial.println("Stopping AP mode to connect to WiFi");
    WiFi.softAPdisconnect(true);
    apModeActive = false;
  }
  
  // Disconnect from current network if connected
  if (WiFi.isConnected()) {
    WiFi.disconnect();
    delay(100);
  }
  
  // Set to station mode and begin connection
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiNetworks[networkIndex].ssid, wifiNetworks[networkIndex].password);
  
  wifiConnectStartTime = millis();
  currentSSIDIndex = networkIndex;
  isWifiConnected = false;
}

// Function to start AP mode
void startAPMode() {
  if (apModeActive) return;
  
  Serial.println("Starting AP mode...");
  
  // Disconnect from any current connection
  if (WiFi.isConnected()) {
    WiFi.disconnect();
  }
  
  // Configure and start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  
  if (WiFi.softAP(apssid, apkey)) {
    apModeActive = true;
    isWifiConnected = true; // Consider AP mode as "connected" for server init
    needServerInit = true;
    currentSSIDIndex = -1;
    
    Serial.printf("AP started successfully\n");
    Serial.printf("SSID: %s\n", apssid);
    Serial.printf("IP address: %s\n", WiFi.softAPIP().toString().c_str());
    
    digitalWrite(led, 0); // Turn off LED to indicate network is ready
  } else {
    Serial.println("Failed to start AP mode");
    apModeActive = false;
  }
}

void manageWiFi() {
    static unsigned long lastStateChange = 0;
    static int lastConnectionState = -1; // -1=unknown, 0=disconnected, 1=connected
    unsigned long now = millis();
    
    int currentState = WiFi.isConnected() ? 1 : 0;
    
    // Only process state changes, not continuous checking
    if (currentState != lastConnectionState) {
        lastStateChange = now;
        lastConnectionState = currentState;
        
        if (currentState == 1 && !isWifiConnected && currentSSIDIndex >= 0) {
            // Just connected
            isWifiConnected = true;
            needServerInit = true;
            
            Serial.printf("\nSuccessfully connected to: %s\n", wifiNetworks[currentSSIDIndex].ssid);
            Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
            
            digitalWrite(led, 0);
            return;
        } else if (currentState == 0 && isWifiConnected && currentSSIDIndex >= 0) {
            // Just disconnected
            digitalWrite(led, 1);
            Serial.printf("Lost connection to %s\n", wifiNetworks[currentSSIDIndex].ssid);
            isWifiConnected = false;
            currentSSIDIndex = -1;
            
            scanForWiFiNetworks();
            return;
        }
    }
    
    // Handle connection timeout (only check if we're trying to connect)
    if (!WiFi.isConnected() && !apModeActive && currentSSIDIndex >= 0) {
        if (now - wifiConnectStartTime > WIFI_CONNECT_TIMEOUT) {
            Serial.printf("\nConnection to %s timed out\n", wifiNetworks[currentSSIDIndex].ssid);
            currentSSIDIndex = -1;
            scanForWiFiNetworks();
        }
    }
}

void setupWiFi() {
  Serial.println("Initializing WiFi...");
  
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Single scan at startup
  scanForWiFiNetworks();
}

void otaProgressCallback(unsigned int progress, unsigned int total) {
   Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
}

void handleWiFiInLoop() {
  // Handle WiFi management
  manageWiFi();
  
  // Initialize web server if needed
  if (needServerInit && (isWifiConnected || apModeActive)) {
    Serial.println("Initializing web server...");
    
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
       
       // Kick off all WebSocket clients before OTA starts
       Serial.printf("OTA Starting - Disconnecting %d WebSocket clients\n", ws.count());
       ws.closeAll(1012, "Service Restarting"); // 1012 = Service Restarting
       ws.cleanupClients(); // Clean up immediately
       server.end();
       
       saveSessionDataToSPIFFS(); // Backup before OTA starts
       String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
       Serial.println("Start updating " + type);
    }).onEnd([]() {
       otaInProgress = false;
       Serial.println("\nOTA End - WebSocket connections will be re-enabled");
    }).onProgress(otaProgressCallback)
    .onError([](ota_error_t error) {
       otaInProgress = false; // Re-enable connections on error
       Serial.printf("Error[%u]: ", error);
       if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
       else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
       else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
       else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
       else if (error == OTA_END_ERROR) Serial.println("End Failed");
       Serial.println("WebSocket connections re-enabled after OTA error");
    });
    ArduinoOTA.begin();
    
    needServerInit = false;
  }
}

void checkForNewDay(int deviceIndex, float currentYield) {
  // Only check if we have a previous valid yield value
  if (lastTodayYield[deviceIndex] >= 0) {
    // Detect new day: yield went from non-zero to zero
    if (lastTodayYield[deviceIndex] > 0 && currentYield == 0) {
      Serial.printf("NEW DAY DETECTED! %s MPPT yield reset: %.1f -> %.1f\n", 
                 deviceNames[deviceIndex], lastTodayYield[deviceIndex], currentYield);
      
      // Mark that we detected a new day
      if (!newDayDetected) {
        newDayDetected = true;
        newDayDetectedTime = millis();
        Serial.println("NEW DAY: Flagged for reset - waiting for all MPPTs to confirm");
      }
    }
  }
  
  // Update the tracked value
  lastTodayYield[deviceIndex] = currentYield;
}

// New function to check if we should reset for new day:
void checkNewDayReset() {
  if (!newDayDetected) return;
  
  unsigned long now = millis();
  
  // Wait 30 seconds after first detection to let all MPPTs reset
  if (now - newDayDetectedTime < 30000) {
    return; // Still waiting
  }
  
  // Check if ALL MPPTs have reset to 0 (or are at least very low)
  bool allMpptsReset = true;
  int validMppts = 0;
  
  for (int i = 0; i < 3; i++) {
    if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 30000)) {
      validMppts++;
      if (solarDevices[i].todayYield > 10) { // More than 10Wh = probably yesterday's data
        allMpptsReset = false;
        Serial.printf("MPPT %s still has yield: %.1f Wh\n", deviceNames[i], solarDevices[i].todayYield);
      }
    }
  }
  
  if (validMppts == 0) {
    Serial.println("NEW DAY RESET: No valid MPPT data - proceeding anyway");
    allMpptsReset = true; // If we can't see MPPTs, assume they reset
  }
  
  if (allMpptsReset) {
    Serial.println("NEW DAY CONFIRMED: All MPPTs have reset - clearing data and rebooting");
    
    // Clear all session data for new day
    for (int i = 0; i < DATA_MINS; i++) {
      rtc_dailySolarData[i] = 0;
    }
    rtc_sessionActive = false;
    rtc_currentDataIndex = 0;
    rtc_sessionStartTime = millis();
    
    // Clear new day detection flags
    rtc_newDayDetected = false;
    rtc_newDayDetectedTime = 0;
    
    // Save cleared data to SPIFFS
    saveSessionDataToSPIFFS();
    
    Serial.println("NEW DAY: Session data cleared - rebooting for fresh start!");
    delay(1000);
    ESP.restart();
  } else {
    // Check for timeout - if we've been waiting too long, force reset anyway
    if (now - newDayDetectedTime > 300000) { // 5 minutes timeout
      Serial.println("NEW DAY TIMEOUT: Forcing reset despite some MPPTs not confirming");
      
      // Clear all session data for new day
      for (int i = 0; i < DATA_MINS; i++) {
        rtc_dailySolarData[i] = 0;
      }
      rtc_sessionActive = false;
      rtc_currentDataIndex = 0;
      rtc_sessionStartTime = millis();
      
      // Clear new day detection flags
      rtc_newDayDetected = false;
      rtc_newDayDetectedTime = 0;
      
      saveSessionDataToSPIFFS();
      
      Serial.println("NEW DAY: Session data cleared - rebooting for fresh start!");
      delay(1000);
      ESP.restart();
    }
  }
}

// Function to monitor memory and trigger reset if needed
void checkMemoryHealth() {
    static unsigned long lastMemCheck = 0;
    unsigned long now = millis();
    
    // Only check every 5 seconds to reduce overhead
    if (now - lastMemCheck < 5000) return;
    lastMemCheck = now;
    
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minFreeHeap) {
        minFreeHeap = freeHeap;
    }
    
    // Critical memory threshold
    if (freeHeap < 8192) { // Lowered threshold for earlier warning
        Serial.printf("CRITICAL: Low memory detected (%d bytes), forcing reset!\n", freeHeap);
        saveSessionDataToSPIFFS();
        delay(1000);
        ESP.restart();
    }
    // Warning threshold
    else if (freeHeap < 12288) {
        Serial.printf("WARNING: Low memory (%d bytes)\n", freeHeap);
        // Force garbage collection
        ws.cleanupClients();
    }
}

// Web page HTML
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
      zoom: 0.95; /* Shrink everything by 10% */
    }
    .container {
      max-width: 1080px; /* Reduced from 1200px */
      margin: 0 auto;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 11px; /* Slightly smaller */
      padding: 5px; /* Reduced from 6px */
      backdrop-filter: blur(10px);
      box-shadow: 0 7px 29px rgba(0, 0, 0, 0.3); /* Slightly smaller shadow */
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(270px, 1fr)); /* Reduced from 300px */
      gap: 4px; /* Reduced from 5px */
      margin-bottom: 5px; /* Reduced from 6px */
    }
    .card {
      background: rgba(255, 255, 255, 0.15);
      border-radius: 11px; /* Slightly smaller */
      padding: 18px; /* Reduced from 25px */
      border: 1px solid rgba(255, 255, 255, 0.2);
      backdrop-filter: blur(5px);
      /* Removed hover transform effect */
    }
    .card h3 {
      margin-top: 0;
      font-size: 1.26rem; /* Reduced from 1.4rem */
      color: #ffd700;
      text-shadow: 1px 1px 2px rgba(0, 0, 0, 0.5);
    }
    .metric {
      display: flex;
      justify-content: space-between;
      margin: 9px 0; /* Reduced from 10px */
      font-size: 1rem; /* Reduced from 1.1rem */
    }
    .value {
      font-weight: bold;
      color: #00ff88;
    }
    .gauge-container {
      display: flex;
      justify-content: stretch;
      gap: 5px; /* Reduced from 6px */
      margin: 3px 0 5px 0; /* Reduced margins */
      flex-wrap: wrap;
    }
    .gauge-wrapper {
      display: flex;
      flex-direction: column;
      align-items: center;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 9px; /* Reduced from 10px */
      padding: 7px 4px; /* Reduced from 8px 5px */
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
    <!-- Gauge Dashboard -->
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
    
    <!-- Daily Solar Graph -->
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
    var sessionActive = false;
    var sessionStartTime = 0;
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
            
            // Bind methods to preserve 'this' context
            this.onOpen = this.onOpen.bind(this);
            this.onMessage = this.onMessage.bind(this);
            this.onClose = this.onClose.bind(this);
            this.onError = this.onError.bind(this);
            this.checkConnection = this.checkConnection.bind(this);
            
            // Initialize data arrays
            this.initializeDailyData();
            
            // Handle page visibility changes
            document.addEventListener('visibilitychange', () => {
                if (document.hidden) {
                    this.onPageHidden();
                } else {
                    this.onPageVisible();
                }
            });
            
            // Handle page unload
            window.addEventListener('beforeunload', () => {
                this.cleanup();
            });
            
            // Start connection after a brief delay
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
            // Clean up any existing connection first
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
                
                // Attach event listeners
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
            
            // Clear any pending reconnection
            this.clearReconnectTimer();
            
            // Update connection status in UI
            this.updateConnectionStatus('Connected');
        }
        
        onMessage(event) {
            this.lastDataTime = Date.now();
            
            try {
                const data = JSON.parse(event.data);
                
                // Check for version changes first
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
                
                // Handle different message types
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
            
            // Clean up
            this.ws = null;
            
            // Update UI
            this.updateConnectionStatus('Disconnected');
            
            // Only reconnect if not intentionally closed
            if (!this.isIntentionallyClosed) {
                this.scheduleReconnect();
            }
        }
        
        onError(event) {
            console.error('WebSocket error', event);
            // Don't schedule reconnect here - let onClose handle it
        }
        
        scheduleReconnect() {
            // Clear any existing timer
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
            
            // Exponential backoff with jitter
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
            
            // Clear timers
            this.clearReconnectTimer();
            
            if (this.connectionCheckTimer) {
                clearInterval(this.connectionCheckTimer);
                this.connectionCheckTimer = null;
            }
            
            if (this.heartbeatInterval) {
                clearInterval(this.heartbeatInterval);
                this.heartbeatInterval = null;
            }
            
            // Close WebSocket cleanly
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
                document.getElementById('battKwh').textContent = ((data.battery.soc / 100) * batteryKWH).toFixed(2) + ' kWh';
                
                const timeToGoFormatted = formatTimeToGo(data.battery.timeToGo);
                document.getElementById('battTimeToGo').innerHTML = timeToGoFormatted;
                document.getElementById('batteryTimeToGo').innerHTML = timeToGoFormatted;
            }
            
            if (data.Mppts) {
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
            }
            
            if (data.solar) {
                const todayKwh = (parseFloat(data.solar.totalToday) / 1000).toFixed(2);
                document.getElementById('totalToday').textContent = todayKwh + ' kWh';
                
                if (data.session && data.session.currentIndex >= 0) {
                    document.getElementById('sessionTime').textContent = formatTimeToGo(data.session.currentIndex);
                } else {
                    document.getElementById('sessionTime').textContent = '--';
                }
                
                // Update live data in chart
                if (data.session && data.session.currentIndex >= 0 && data.session.currentIndex < solarData.length) {
                    let totalPower = 0;
                    if (data.Mppts) {
                        if (data.Mppts.left && data.Mppts.left.power) totalPower += data.Mppts.left.power;
                        if (data.Mppts.right && data.Mppts.right.power) totalPower += data.Mppts.right.power;
                        if (data.Mppts.rear && data.Mppts.rear.power) totalPower += data.Mppts.rear.power;
                    }
                    // Only update if we have meaningful power data
                    if (totalPower > 0) {
                        solarData[data.session.currentIndex] = Math.max(solarData[data.session.currentIndex], totalPower);
                    }
                }
                drawSolarChart();
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
    
    // Utility functions
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
        
        if (sessionActive && lastDataIndex > 0) {
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
    
    // Initialize when page loads
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
        
        // Initialize the robust WebSocket connection
        robustWS = new RobustWebSocket();
        
        // Redraw chart every minute
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
        client->close(1012, "Service Restarting"); // 1012 = Service Restarting code
        return;
      }
      
      Serial.printf("WebSocket client #%u connected from %s\r\n", client->id(), client->remoteIP().toString().c_str());
      
      // Send daily data to new client
      sendDailySolarDataAsync(client);
      break;
      
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\r\n", client->id());
      break;
      
    case WS_EVT_ERROR:
      Serial.printf("WebSocket client #%u error(%u): %s\r\n", client->id(), *((uint16_t*)arg), (char*)data);
      break;
      
    case WS_EVT_DATA:
      // Ignore any data during OTA
      if (otaInProgress) {
        return;
      }
      break;
      
    default:
      break;
  }
}

void handleFavicon(AsyncWebServerRequest *request) {
  // Use a const string instead of building dynamically to save memory
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
    
    // Pre-allocate string to reduce fragmentation
    String output;
    output.reserve(1600); // Estimate size needed
    
    for (int chunk = 0; chunk < 7; chunk++) {
        StaticJsonDocument<2000> doc;
        JsonArray solarArray = doc.createNestedArray("dailyDataChunk");
        doc["chunkIndex"] = chunk;
        doc["totalChunks"] = 7;
        
        int startIndex = chunk * chunkSize;
        int endIndex = min((chunk + 1) * chunkSize, DATA_MINS);
        
        for (int i = startIndex; i < endIndex; i++) {
            solarArray.add(dailySolarData[i]);
        }
        
        output.clear(); // Reuse string
        serializeJson(doc, output);

        if (client->canSend()) {
            client->text(output);
            delay(5); // Reduced delay
        } else {
            Serial.println("Client disconnected during data send");
            return;
        }
    }
    
    // Send completion message
    if (client->canSend()) {
        output.clear();
        output = "{\"dailyDataComplete\":true}"; // Direct string instead of JSON
        client->text(output);
    }
}

void initializeDevices() {
  // Record boot time for grace period
  bootTime = millis();
  
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
  
  // Initialize RTC data if invalid (cold boot or first run)
  if (rtc_dataValid != RTC_DATA_MAGIC) {
    Serial.println("Initializing RTC SRAM data (cold boot)");
    for (int i = 0; i < DATA_MINS; i++) {
      rtc_dailySolarData[i] = 0;
    }
    rtc_sessionActive = false;
    rtc_currentDataIndex = 0;
    rtc_sessionStartTime = millis();
    rtc_lastBootTime = millis();
    rtc_lastMinuteUpdate = millis(); // Initialize minute timer
    rtc_newDayDetected = false;
    rtc_newDayDetectedTime = 0;
    rtc_dataValid = RTC_DATA_MAGIC;
    
    needsRestore = true;
  } else {
    Serial.println("RTC SRAM data valid (warm boot/OTA)");
    rtc_lastBootTime = millis();
    rtc_lastMinuteUpdate = millis(); // Reset minute timer on reboot
    
    Serial.printf("Session: %s, Index: %d\n", 
               sessionActive ? "Active" : "Inactive", currentDataIndex);
    
    if (sessionActive && currentDataIndex > 0) {
      Serial.printf("Existing session found with %d minutes of data\n", currentDataIndex);
    }
    
    // Check if we were in the middle of a new day detection
    if (newDayDetected) {
      Serial.printf("New day detection was in progress before reboot - resuming\n");
    }
  }
}

void initializeFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  Serial.println("SPIFFS Mounted Successfully");
  
  // If this was a cold boot, try to restore from backup
  if (needsRestore) {
    loadSessionDataFromSPIFFS();
    needsRestore = false;
  }
}

void saveSessionDataToSPIFFS() {
  File file = SPIFFS.open("/solarSession.bin", "w");
  if (file) {
    // Save only the essential session data to SPIFFS backup
    file.write((uint8_t*)&rtc_sessionActive, sizeof(rtc_sessionActive));
    file.write((uint8_t*)&rtc_currentDataIndex, sizeof(rtc_currentDataIndex));
    file.write((uint8_t*)&rtc_sessionStartTime, sizeof(rtc_sessionStartTime));
    file.write((uint8_t*)rtc_dailySolarData, sizeof(rtc_dailySolarData));
    file.write((uint8_t*)&rtc_lastBootTime, sizeof(rtc_lastBootTime));
    file.write((uint8_t*)&rtc_lastMinuteUpdate, sizeof(rtc_lastMinuteUpdate)); // Add this
    
    file.close();
    
    dataChangedSinceLastBackup = false;
    lastBackupSave = millis();

    Serial.println("Session data backed up to SPIFFS");
  } else {
    Serial.println("Failed to save session backup to SPIFFS!");
  }
}

void loadSessionDataFromSPIFFS() {
  File file = SPIFFS.open("/solarSession.bin", "r");
  if (file) {
    Serial.println("Restoring session data from SPIFFS backup");
    
    // Read session data into RTC SRAM
    file.read((uint8_t*)&rtc_sessionActive, sizeof(rtc_sessionActive));
    file.read((uint8_t*)&rtc_currentDataIndex, sizeof(rtc_currentDataIndex));
    file.read((uint8_t*)&rtc_sessionStartTime, sizeof(rtc_sessionStartTime));
    file.read((uint8_t*)rtc_dailySolarData, sizeof(rtc_dailySolarData));
    
    // Try to read newer fields (may not exist in old backups)
    if (file.available() >= sizeof(rtc_lastBootTime)) {
      file.read((uint8_t*)&rtc_lastBootTime, sizeof(rtc_lastBootTime));
    }
    if (file.available() >= sizeof(rtc_lastMinuteUpdate)) {
      file.read((uint8_t*)&rtc_lastMinuteUpdate, sizeof(rtc_lastMinuteUpdate));
    } else {
      rtc_lastMinuteUpdate = millis(); // Default for old backups
    }
    
    file.close();
    
    Serial.printf("Restored: Active=%s, Index=%d\n", 
               sessionActive ? "true" : "false", currentDataIndex);
    
    // Calculate how much existing data we have
    int dataPoints = 0;
    for (int i = 0; i < DATA_MINS; i++) {
      if (dailySolarData[i] > 0) dataPoints++;
    }
    if (dataPoints > 0) {
      Serial.printf("Restored %d data points from previous session\n", dataPoints);
    }
  } else {
    Serial.println("No SPIFFS backup found, starting fresh");
  }
}

bool shouldBackupToSPIFFS() {
  unsigned long now = millis();
  static unsigned long lastBackupCheck = 0;
  
  // Don't check too frequently
  if (now - lastBackupCheck < 60000) return false; // Only check once per minute
  lastBackupCheck = now;
  
  // Check various conditions that trigger a backup
  if (otaInProgress) {
    Serial.println("BACKUP_TRIGGER: OTA in progress - backing up before update");
    return true;
  }
  
  if (rebootRequested) {
    Serial.println("BACKUP_TRIGGER: Reboot requested - backing up before restart");
    return true;
  }
  
  if (dataChangedSinceLastBackup && (now - lastBackupSave >= BACKUP_SAVE_INTERVAL)) {
    // Additional check - only backup if we have meaningful data
    bool hasSignificantData = false;
    for (int i = 0; i < min(currentDataIndex + 10, DATA_MINS); i++) {
      if (dailySolarData[i] > 50) { // At least 50W recorded
        hasSignificantData = true;
        break;
      }
    }
    
    if (hasSignificantData || sessionActive) {
      Serial.printf("BACKUP_TRIGGER: Scheduled backup (last backup %lums ago, session=%s)\n", 
                 now - lastBackupSave, sessionActive ? "active" : "inactive");
      return true;
    }
  }
  
  return false;
}

void updateDailyData(uint16_t totalPower) {
  unsigned long now = millis();

  if (sessionActive) {
    // Check if we should end the session
    if (totalPower < 5) { // Very low power
        if (lowPowerStart == 0) {
            lowPowerStart = now; // Start timing low power period
        } else if (now - lowPowerStart > 300) { // 5 minutes of low power
            Serial.println("SESSION END: 5 minutes of low power - ending session");
            sessionActive = false;
            return; // Don't process data when session ends
        }
    } else {
        lowPowerStart = 0; // Reset timer when power returns
    }
    
    // Check if we hit the time limit
    if (currentDataIndex >= DATA_MINS - 1) {
        Serial.println("SESSION END: Reached maximum session length!");
        sessionActive = false;
        return;
    }
  }
  
  // Session management logic - simplified
  if (!sessionActive && totalPower > 5) {
    // Start new session - first light detected
    Serial.println("Starting new solar session - first light detected");
    sessionActive = true;
    sessionStartTime = now;
    currentDataIndex = 0;
    lastMinuteUpdate = now; // Reset minute timer
    dataChangedSinceLastSave = true;
    dataChangedSinceLastBackup = true;
  }
  
  if (sessionActive) {
    // Update current minute's data
    if (currentDataIndex < DATA_MINS) {
      uint16_t newValue = max(dailySolarData[currentDataIndex], totalPower);
      if (newValue != dailySolarData[currentDataIndex]) {
        dailySolarData[currentDataIndex] = newValue;
        dataChangedSinceLastSave = true;
      }
    }
  }
    
  // Check if a full minute has elapsed - much more reliable than loop counting
  if (now - lastMinuteUpdate >= 60000) { // 60,000ms = 1 minute
    lastMinuteUpdate = now;
    Serial.printf("Memory: Free=%d, Min=%d, WS clients=%d, Loops=%lu, ", ESP.getFreeHeap(), minFreeHeap, ws.count(),loopCount);
    Serial.printf("Session: %s, Index: %d\n", sessionActive ? "Active" : "Inactive", currentDataIndex);
    loopCount = 0;

    // Clean up websocket clients more aggressively
    ws.cleanupClients();
          
    // Force garbage collection by triggering a small allocation
    String dummy;
    dummy.reserve(100);
      
    if (sessionActive && currentDataIndex + 1 < DATA_MINS) {
      currentDataIndex++;
      dataChangedSinceLastSave = true;
    }
  }
}

void updateDisplayAndWebSocket() {
    unsigned long now = millis();
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
        lastDisplayUpdate = now;
        
        uint16_t totalPower = 0;
        float totalToday = 0;
        int activeMppts = 0;
        String solarStatus = "OFF";
        
        // Thread-safe device data access for display calculations
        if (deviceDataMutex != NULL && xSemaphoreTake(deviceDataMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
            // Cache validity checks to avoid repeated calculations
            bool deviceValid[3];
            for (int i = 0; i < 3; i++) {
                deviceValid[i] = solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000);
                if (deviceValid[i]) {
                    totalPower += solarDevices[i].power;
                    totalToday += solarDevices[i].todayYield;
                    activeMppts++;
                    if (solarDevices[i].state >= 3 && solarDevices[i].state <= 6) {
                        solarStatus = statTxt[solarDevices[i].state];
                    }
                }
            }
            xSemaphoreGive(deviceDataMutex);
        }
        
        updateDailyData(totalPower);
        
        if (shouldBackupToSPIFFS()) {
            saveSessionDataToSPIFFS();
        }
            
        if (printData && batteryDevice.valid) {
            Serial.printf("Battery:          %6.2fV  %6.2fA  %6.0fW  %5.1fAh  %5.1f%%   %dm\n",
                batteryDevice.voltage, batteryDevice.current, batteryDevice.power,
                batteryDevice.ampHours, batteryDevice.soc, batteryDevice.timeToGo);
            Serial.printf("Solar Total:      %6dW  %6.0fWh  %s  Session: %s (%dm)\n",
                totalPower, totalToday, solarStatus.c_str(), 
                sessionActive ? "Active" : "Inactive", currentDataIndex);
            printData = 0;
        }
    }
        
    // SKIP WebSocket updates completely during OTA
    if (otaInProgress) {
        return;
    }
    
    // WebSocket updates with proper mutex handling
    if (now - lastWebSocketUpdate >= WEBSOCKET_UPDATE_INTERVAL) {
        lastWebSocketUpdate = now;
        
        checkMemoryHealth();
        
        // Only send WebSocket data if we have clients and sufficient memory
        if (ws.count() == 0 || ESP.getFreeHeap() < 15000) {
            return;
        }
        
        // Pre-calculate commonly used values with thread safety
        bool batteryValid = false;
        uint16_t totalPower = 0;
        float totalToday = 0;
        
        // Thread-safe access to all device data
        if (deviceDataMutex != NULL && xSemaphoreTake(deviceDataMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
            batteryValid = batteryDevice.valid && (now - batteryDevice.lastUpdate < 10000);
            
            // Calculate solar totals
            for (int i = 0; i < 3; i++) {
                if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000)) {
                    totalPower += solarDevices[i].power;
                    totalToday += solarDevices[i].todayYield;
                }
            }
            xSemaphoreGive(deviceDataMutex);
        }
        
        // Use smaller, more efficient JSON document
        StaticJsonDocument<500> doc; // Increased slightly for solar data
        
        // Build version hash more efficiently
        static uint32_t versionHash = 0;
        if (versionHash == 0) { // Calculate once at startup
            String buildTime = __TIMESTAMP__;
            for (int i = 0; i < buildTime.length(); i++) {
                versionHash = versionHash * 31 + buildTime.charAt(i);
            }
        }
        doc["version"] = String(versionHash);
        
        // Session information
        JsonObject session = doc.createNestedObject("session");
        session["active"] = sessionActive;
        session["currentIndex"] = currentDataIndex;
        session["startTime"] = sessionStartTime;
        
        // Battery data with thread safety
        if (batteryValid && deviceDataMutex != NULL && xSemaphoreTake(deviceDataMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
            JsonObject battery = doc.createNestedObject("battery");
            battery["voltage"] = serialized(String(batteryDevice.voltage, 2));
            battery["current"] = serialized(String(batteryDevice.current, 2));
            battery["power"] = serialized(String(batteryDevice.power, 0));
            battery["ampHours"] = serialized(String(batteryDevice.ampHours, 1));
            battery["soc"] = serialized(String(batteryDevice.soc, 1));
            battery["timeToGo"] = batteryDevice.timeToGo;
            xSemaphoreGive(deviceDataMutex);
        }
        
        // MPPT data with thread safety
        JsonObject Mppts = doc.createNestedObject("Mppts");
        
        if (deviceDataMutex != NULL && xSemaphoreTake(deviceDataMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
            for (int i = 0; i < 3; i++) {
                JsonObject currentMppt = Mppts.createNestedObject(mpptNames[i]);
                bool valid = solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000);
                
                if (valid) {
                    currentMppt["power"] = solarDevices[i].power;
                    currentMppt["status"] = statTxt[solarDevices[i].state];
                } else {
                    currentMppt["power"] = 0;
                    currentMppt["status"] = "OFF";
                }
            }
            xSemaphoreGive(deviceDataMutex);
        }
        
        // Solar summary
        JsonObject solar = doc.createNestedObject("solar");
        solar["totalToday"] = serialized(String(totalToday, 0)); // Send as Wh, web page converts to kWh
        
        // Send efficiently
        String output;
        output.reserve(600); // Pre-allocate
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

        // Thread-safe data updates using mutex
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
  
  // Build response more efficiently to reduce memory fragmentation
  String response;
  response.reserve(500); // Pre-allocate to reduce fragmentation
  response = "{";
  
  if (batteryDevice.valid && (now - batteryDevice.lastUpdate < 10000)) {
    response += "\"battVoltage\":\"" + String(batteryDevice.voltage, 2) + " V\",";
    response += "\"battCurrent\":\"" + String(batteryDevice.current, 2) + " A\",";
    response += "\"battPower\":\"" + String(batteryDevice.power, 0) + " W\",";
    response += "\"battCapacity\":\"" + String(batteryDevice.ampHours, 1) + " Ah\",";
    response += "\"battSOC\":\"" + String(batteryDevice.soc, 1) + " %\",";
    
    // Add individual Mppt data
    for (int i = 0; i < 3; i++) {
      String MpptName = (i == 0) ? "left" : (i == 1) ? "right" : "rear";
      if (solarDevices[i].valid && (now - solarDevices[i].lastUpdate < 10000)) {
        response += "\"" + MpptName + "Power\":\"" + String(solarDevices[i].power) + "\",";
        response += "\"" + MpptName + "Status\":\"" + String(statTxt[solarDevices[i].state]) + "\",";
      } else {
        response += "\"" + MpptName + "Power\":\"--\",";
        response += "\"" + MpptName + "Status\":\"--\",";
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
        
        // Wait before next scan - adjust this based on your needs
        // 200ms is good for solar data which doesn't change rapidly
        vTaskDelay(50 / portTICK_PERIOD_MS);
        
        // Yield to watchdog
        yield();
    }
    vTaskDelete(NULL); // Delete this task
}

void setup() {
    pinMode(led, OUTPUT);
    digitalWrite(led, 1);
    Serial.begin(115200);  
    Serial.println();
    Serial.printf("%s - Built %s\n", apssid, __TIMESTAMP__);
    Serial.println();
    
    initializeDevices();
    initializeFileSystem();
    setupWiFi();  
    strcpy(savedDeviceName,"(unknown device name)");
    
    // Create mutex for thread-safe device data access
    deviceDataMutex = xSemaphoreCreateMutex();
    if (deviceDataMutex == NULL) {
        Serial.println("ERROR: Failed to create device data mutex!");
    }
    
    // Initialize BLE
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(50);
    pBLEScan->setWindow(49);
    
    // Create BLE scanning task on core 0 (main loop runs on core 1)
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        bleScanTask,           // Task function
        "BLE_Scan_Task",       // Task name for debugging
        4096,                  // Stack size (4KB should be sufficient)
        NULL,                  // Parameters passed to task
        1,                     // Priority (1 = low priority, below main loop)
        &bleScanTaskHandle,    // Task handle for management
        0                      // Pin to core 0 (core 1 runs main loop)
    );
    
    if (taskCreated != pdPASS) {
        Serial.println("✗ ERROR: Failed to create BLE scanning task!");
        bleScanTaskHandle = NULL;
    }
}

void loop() {
    ArduinoOTA.handle();
    
    checkMemoryHealth();
    checkNewDayReset();
    
    // Process console commands
    while (Serial.available()) {
        uint8_t gc = Serial.read();
        if (gc == 13) {
            printData = 1;
        } else if (gc == 1) {  // CTRL-A
            Serial.println("REBOOTING NOW...");
            rebootRequested = true;
            saveSessionDataToSPIFFS();
            
            // Stop BLE task before reboot
            if (bleScanTaskHandle != NULL) {
                vTaskDelete(bleScanTaskHandle);
                bleScanTaskHandle = NULL;
            }
            
            vTaskDelay(100 / portTICK_PERIOD_MS);
            ESP.restart();
        } else if (gc == 2) { // CTRL-B
            Serial.println("Manually clearing all session data...");
            for (int i = 0; i < DATA_MINS; i++) {
               rtc_dailySolarData[i] = 0;
            }
            rtc_sessionActive = false;
            rtc_currentDataIndex = 0;
            rtc_sessionStartTime = millis();
            saveSessionDataToSPIFFS();
            Serial.println("Session data cleared - ready for new day");
        } else if (gc == 'm') { // Memory status
            Serial.printf("Free heap: %d bytes, Min free: %d bytes\n", 
                       ESP.getFreeHeap(), minFreeHeap);
        } else if (gc == 'w') { // WiFi status
            if (apModeActive) {
              Serial.printf("WiFi Status: AP Mode - %s\n", WiFi.softAPIP().toString().c_str());
            } else if (WiFi.isConnected()) {
              Serial.printf("WiFi Status: Connected to %s - %s (RSSI: %d)\n", 
                         wifiNetworks[currentSSIDIndex].ssid, 
                         WiFi.localIP().toString().c_str(), WiFi.RSSI());
            } else {
              Serial.println("WiFi Status: Disconnected");
            }
        } else if (gc == 's') { // Force WiFi scan
            Serial.println("Forcing WiFi scan...");
            scanForWiFiNetworks();
        } else if (gc == 'b') { // BLE task status
            if (bleScanTaskHandle != NULL) {
                Serial.printf("BLE Task: Running on core 0, Stack high water: %d bytes\n", 
                          uxTaskGetStackHighWaterMark(bleScanTaskHandle));
            } else {
                Serial.println("BLE Task: Not running");
            }
        }
    }
            
    handleWiFiInLoop();
    updateDisplayAndWebSocket();

    loopCount++;
    
    yield();
    
    vTaskDelay(5 / portTICK_PERIOD_MS);
}
