/*
 * Wireless DMX512 Adapter
 * Receiver Code for ESP8266 NodeMCU
 *
 * Libraries Required:
 * - ESP8266WiFi
 * - WiFiManager (by tzapu)
 * - ArtnetWifi
 * - ESP8266HTTPClient
 */


#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ArtnetWifi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>

// Pin Definitions
#define DMX_TX_PIN 2         // NodeMCU Pin D4 (GPIO2 / TX1)
#define DMX_DE_PIN 5         // NodeMCU Pin D1 (GPIO5) - RS485 Driver Enable
#define LED_PIN LED_BUILTIN  // Built-in LED for status indication

// DMX Constants
#define DMX_CHANNELS 512
#define DMX_BREAK 100        // Break time in microseconds (88us min)
#define DMX_MAB 12           // Mark After Break in microseconds (8us min)

// EEPROM addresses for configuration storage
#define EEPROM_SIZE 512
#define CALLBACK_URL_ADDR 0
#define CALLBACK_URL_MAX_LEN 256
#define UNIVERSE_ADDR 256

WiFiManager wifiManager;
ArtnetWifi artnet;

char callbackUrl[CALLBACK_URL_MAX_LEN] = "";
byte dmxData[DMX_CHANNELS];
char universeStr[6] = "0";
uint16_t artnetUniverse = 0;

unsigned long frameCount = 0;
unsigned long lastFrameTime = 0;
unsigned long allPacketsCount = 0;  // Count all ArtNet packets, even wrong universe

void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Save callback URL
  for (int i = 0; i < CALLBACK_URL_MAX_LEN; i++) {
    EEPROM.write(CALLBACK_URL_ADDR + i, callbackUrl[i]);
    if (callbackUrl[i] == '\0') break;
  }
  
  // Save universe
  EEPROM.write(UNIVERSE_ADDR, artnetUniverse >> 8);
  EEPROM.write(UNIVERSE_ADDR + 1, artnetUniverse & 0xFF);
  
  EEPROM.commit();
  EEPROM.end();
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Load callback URL
  for (int i = 0; i < CALLBACK_URL_MAX_LEN; i++) {
    callbackUrl[i] = EEPROM.read(CALLBACK_URL_ADDR + i);
    if (callbackUrl[i] == '\0') break;
  }
  callbackUrl[CALLBACK_URL_MAX_LEN - 1] = '\0';
  
  // Load universe
  artnetUniverse = (EEPROM.read(UNIVERSE_ADDR) << 8) | EEPROM.read(UNIVERSE_ADDR + 1);
  
  // If EEPROM is uninitialized (0xFFFF), default to universe 0
  if (artnetUniverse == 0xFFFF || artnetUniverse > 32767) {
    artnetUniverse = 0;
  }
  
  snprintf(universeStr, sizeof(universeStr), "%d", artnetUniverse);
  
  EEPROM.end();
}

void sendCallback() {
  if (strlen(callbackUrl) == 0) return; // No callback URL set
  
  WiFiClient client;
  HTTPClient http;
  
  String ipAddress = WiFi.localIP().toString();
  String fullUrl = String(callbackUrl);
  
  // Add IP parameter to URL
  if (fullUrl.indexOf('?') >= 0) {
    fullUrl += "&ip=" + ipAddress;
  } else {
    fullUrl += "?ip=" + ipAddress;
  }
  
  http.begin(client, fullUrl);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    // Request successful
  }
  
  http.end();
}

void saveConfigCallback() {
  // Save configuration when WiFiManager saves config
  saveConfig();
}

void sendDMX() {
  // Enable transmitter
  digitalWrite(DMX_DE_PIN, HIGH);
  
  // Send BREAK (low signal)
  Serial1.end();
  pinMode(DMX_TX_PIN, OUTPUT);
  digitalWrite(DMX_TX_PIN, LOW);
  delayMicroseconds(DMX_BREAK);
  
  // Send MAB (high signal)
  digitalWrite(DMX_TX_PIN, HIGH);
  delayMicroseconds(DMX_MAB);
  
  // Re-initialize UART for data transmission
  Serial1.begin(250000, SERIAL_8N2);
  
  // Send START code (0x00)
  Serial1.write(0x00);
  
  // Send DMX channel data
  Serial1.write(dmxData, DMX_CHANNELS);
  Serial1.flush();
  
  // Disable transmitter
  digitalWrite(DMX_DE_PIN, LOW);
}

void onDmxFrame(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t* data) {
  // Count all packets received
  allPacketsCount++;
  
  // Debug: show all packets every 50 packets
  if (allPacketsCount % 50 == 0) {
    Serial.printf("[DEBUG] Total ArtNet packets received: %lu\n", allPacketsCount);
  }
  
  // Check if this is our universe
  if (universe != artnetUniverse) {
    // First wrong universe packet, log it
    if (allPacketsCount == 1 || allPacketsCount % 100 == 0) {
      Serial.printf("[INFO] Received universe %d, but listening for %d (ignoring)\n", 
                    universe, artnetUniverse);
    }
    return;  // Ignore wrong universe
  }
  
  // Only process the configured universe
  if (universe == artnetUniverse) {
    // Blink LED to indicate data received
    digitalWrite(LED_PIN, LOW);  // LED on (inverted)
    
    // Update frame counter
    frameCount++;
    lastFrameTime = millis();
    
    // Print debug info every 100 frames
    if (frameCount % 100 == 0) {
      Serial.printf("ArtNet frame %lu: Universe %d, Length %d, Seq %d\n", 
                    frameCount, universe, length, sequence);
      Serial.printf("  First 4 channels: %d, %d, %d, %d\n",
                    data[0], data[1], data[2], data[3]);
    }
    
    // Copy Art-Net data to DMX buffer
    for (int i = 0; i < length && i < DMX_CHANNELS; i++) {
      dmxData[i] = data[i];
    }
    // Send DMX data
    sendDMX();
    
    digitalWrite(LED_PIN, HIGH);  // LED off (inverted)
  }
}

void setup() {
  // Initialize Serial for debugging
  Serial.begin(115200);
  Serial.println("\n\nWireless DMX Receiver Starting...");
  
  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // LED off (inverted)
  
  // Initialize DMX buffer
  memset(dmxData, 0, DMX_CHANNELS);
  
  // Initialize pins
  pinMode(DMX_DE_PIN, OUTPUT);
  digitalWrite(DMX_DE_PIN, LOW);  // Disable transmitter initially
  
  // Initialize Serial1 for DMX (250kbaud, 8N2)
  Serial1.begin(250000, SERIAL_8N2);
  
  // Load saved configuration from EEPROM
  loadConfig();
  
  // Create custom parameters for WiFi configuration portal
  WiFiManagerParameter customCallbackUrl("callback", "Callback URL", callbackUrl, CALLBACK_URL_MAX_LEN);
  WiFiManagerParameter customUniverse("universe", "ArtNet Universe (0-32767)", universeStr, 6);
  
  // WiFiManager will try to connect to saved credentials
  // If it fails, it starts a captive portal named "DMX_RX_Setup"
  // Connect to this AP and configure WiFi at 192.168.4.1
  wifiManager.addParameter(&customCallbackUrl);
  wifiManager.addParameter(&customUniverse);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(180); // 3 minutes timeout
  
  if (!wifiManager.autoConnect("DMX_RX_Setup")) {
    // Failed to connect and timeout reached, restart and try again
    Serial.println("Failed to connect to WiFi, restarting...");
    delay(3000);
    ESP.restart();
    delay(5000);
  }

  Serial.println("WiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Subnet Mask: ");
  Serial.println(WiFi.subnetMask());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("mDNS: dmx-receiver.local\n");
  Serial.println();

  // Get the entered configuration values
  strncpy(callbackUrl, customCallbackUrl.getValue(), CALLBACK_URL_MAX_LEN - 1);
  callbackUrl[CALLBACK_URL_MAX_LEN - 1] = '\0';
  
  artnetUniverse = atoi(customUniverse.getValue());
  
  Serial.print("Listening on Universe: ");
  Serial.println(artnetUniverse);
  if (strlen(callbackUrl) > 0) {
    Serial.print("Callback URL: ");
    Serial.println(callbackUrl);
  }
  
  // WiFi connected successfully - send callback
  sendCallback();
  
  // Start mDNS responder
  if (MDNS.begin("dmx-receiver")) {
    MDNS.addService("artnet", "udp", 6454);
  }
  
  // Initialize ArtNet
  artnet.setArtDmxCallback(onDmxFrame);
  artnet.begin();
  
  Serial.println("ArtNet receiver ready!");
  Serial.println("Listening on UDP port 6454 (ArtNet)");
  Serial.println("Waiting for ArtNet data...\n");
}

void loop() {
  MDNS.update();
  artnet.read(); // Check for incoming Art-Net packets
  
  // Print status every 30 seconds if no data received
  static unsigned long lastStatusPrint = 0;
  if (millis() - lastStatusPrint > 30000) {
    lastStatusPrint = millis();
    if (allPacketsCount == 0) {
      Serial.println("[STATUS] No ArtNet packets received. Check:");
      Serial.println("  - Firewall settings");
      Serial.println("  - ArtNet sender is broadcasting to this network");
      Serial.printf("  - Device IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("  - Listening on universe: %d\n", artnetUniverse);
    } else {
      Serial.printf("[STATUS] Received %lu total packets, %lu on correct universe\n", 
                    allPacketsCount, frameCount);
    }
  }
}

