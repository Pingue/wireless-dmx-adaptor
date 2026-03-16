/*
 * Wireless DMX512 Adapter
 * Transmitter Code for ESP8266 NodeMCU
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
#define DMX_RX_PIN 3         // NodeMCU Pin RX (GPIO3)
#define DMX_RE_PIN 5         // NodeMCU Pin D1 (GPIO5) - RS485 Receiver Enable (active low)

// DMX Constants
#define DMX_CHANNELS 512

// EEPROM addresses for configuration storage
#define EEPROM_SIZE 512
#define CALLBACK_URL_ADDR 0
#define CALLBACK_URL_MAX_LEN 256
#define TARGET_IP_ADDR 256
#define TARGET_IP_MAX_LEN 16
#define UNIVERSE_ADDR 272

WiFiManager wifiManager;
ArtnetWifi artnet;
WiFiUDP pollReplyUdp;

char callbackUrl[CALLBACK_URL_MAX_LEN] = "";
char targetIP[TARGET_IP_MAX_LEN] = "255.255.255.255"; // Default broadcast
char universeStr[6] = "0";
uint16_t artnetUniverse = 0;

uint8_t dmxData[512];
unsigned long lastDmxUpdate = 0;
const unsigned long DMX_UPDATE_INTERVAL = 30; // Send ArtNet every 30ms (~33Hz)

void sendArtPollReply(IPAddress targetIp) {
  uint8_t reply[239];
  memset(reply, 0, sizeof(reply));

  // Art-Net header + OpPollReply
  memcpy(reply, "Art-Net\0", 8);
  reply[8] = 0x00;
  reply[9] = 0x21;

  IPAddress localIp = WiFi.localIP();
  reply[10] = localIp[0];
  reply[11] = localIp[1];
  reply[12] = localIp[2];
  reply[13] = localIp[3];

  // Art-Net UDP port 0x1936
  reply[14] = 0x19;
  reply[15] = 0x36;

  // Firmware version
  reply[16] = 0x01;
  reply[17] = 0x00;

  // Port-Address split into Net/Subnet/Universe
  uint8_t net = (artnetUniverse >> 8) & 0x7F;
  uint8_t sub = (artnetUniverse >> 4) & 0x0F;
  uint8_t uni = artnetUniverse & 0x0F;
  reply[18] = net;
  reply[19] = sub;

  // ESTA manufacturer code (unused here)
  reply[24] = 0x00;
  reply[25] = 0x00;

  // Node names
  strncpy((char*)&reply[26], "WirelessDMX TX", 17);
  strncpy((char*)&reply[44], "Wireless DMX512 Transmitter", 63);
  strncpy((char*)&reply[108], "OK", 63);

  // One output port
  reply[172] = 0x00;
  reply[173] = 0x01;
  reply[174] = 0x80;
  reply[182] = 0x80;
  reply[190] = uni;

  // Node style: StNode
  reply[200] = 0x00;

  // Bind IP
  reply[207] = localIp[0];
  reply[208] = localIp[1];
  reply[209] = localIp[2];
  reply[210] = localIp[3];
  reply[211] = 0x01;

  // Reply directly to poll sender
  pollReplyUdp.beginPacket(targetIp, 6454);
  pollReplyUdp.write(reply, sizeof(reply));
  pollReplyUdp.endPacket();
}

void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Save callback URL
  for (int i = 0; i < CALLBACK_URL_MAX_LEN; i++) {
    EEPROM.write(CALLBACK_URL_ADDR + i, callbackUrl[i]);
    if (callbackUrl[i] == '\0') break;
  }
  
  // Save target IP
  for (int i = 0; i < TARGET_IP_MAX_LEN; i++) {
    EEPROM.write(TARGET_IP_ADDR + i, targetIP[i]);
    if (targetIP[i] == '\0') break;
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
  
  // Load target IP
  for (int i = 0; i < TARGET_IP_MAX_LEN; i++) {
    targetIP[i] = EEPROM.read(TARGET_IP_ADDR + i);
    if (targetIP[i] == '\0') break;
  }
  targetIP[TARGET_IP_MAX_LEN - 1] = '\0';
  
  // Load universe
  artnetUniverse = (EEPROM.read(UNIVERSE_ADDR) << 8) | EEPROM.read(UNIVERSE_ADDR + 1);
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

void setup() {
  // Initialize DMX data array
  memset(dmxData, 0, sizeof(dmxData));
  
  // Initialize RS485 receiver enable pin (active low for receiving)
  pinMode(DMX_RE_PIN, OUTPUT);
  digitalWrite(DMX_RE_PIN, LOW);  // Enable receiver
  
  // Initialize Serial (main UART) for DMX reception at 250kbaud, 8N2
  Serial.begin(250000, SERIAL_8N2);
  
  // Load saved configuration from EEPROM
  loadConfig();
  
  // Create custom parameters for WiFi configuration portal
  WiFiManagerParameter customCallbackUrl("callback", "Callback URL", callbackUrl, CALLBACK_URL_MAX_LEN);
  WiFiManagerParameter customTargetIP("target", "Target IP (broadcast: 255.255.255.255)", targetIP, TARGET_IP_MAX_LEN);
  WiFiManagerParameter customUniverse("universe", "ArtNet Universe (0-32767)", universeStr, 6);
  
  // WiFiManager will try to connect to saved credentials
  // If it fails, it starts a captive portal named "DMX_TX_Setup"
  wifiManager.addParameter(&customCallbackUrl);
  wifiManager.addParameter(&customTargetIP);
  wifiManager.addParameter(&customUniverse);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(180); // 3 minutes timeout
  
  if (!wifiManager.autoConnect("DMX_TX_Setup")) {
    // Failed to connect and timeout reached, restart and try again
    delay(3000);
    ESP.restart();
    delay(5000);
  }

  // Get the entered configuration values
  strncpy(callbackUrl, customCallbackUrl.getValue(), CALLBACK_URL_MAX_LEN - 1);
  callbackUrl[CALLBACK_URL_MAX_LEN - 1] = '\0';
  
  strncpy(targetIP, customTargetIP.getValue(), TARGET_IP_MAX_LEN - 1);
  targetIP[TARGET_IP_MAX_LEN - 1] = '\0';
  
  artnetUniverse = atoi(customUniverse.getValue());
  
  // WiFi connected successfully - send callback
  sendCallback();
  
  // Start mDNS responder
  if (MDNS.begin("dmx-transmitter")) {
    MDNS.addService("artnet", "udp", 6454);
  }
  
  // Initialize ArtNet
  artnet.begin();
  pollReplyUdp.begin(0);
}

void loop() {
  MDNS.update();
  uint16_t opcode = artnet.read();
  if (opcode == ART_POLL) {
    sendArtPollReply(artnet.getSenderIp());
  }
  
  // Read DMX data from Serial (hardware UART)
  static bool inFrame = false;
  static int channelIndex = -1;  // -1 = waiting for start code, 0-511 = channel data
  
  while (Serial.available()) {
    byte receivedByte = Serial.read();
    
    // Check for break condition (indicated by framing error)
    if (Serial.hasRxError()) {
      inFrame = true;
      channelIndex = -1;
      continue;
    }
    
    if (inFrame) {
      if (channelIndex == -1) {
        // This should be the start code (0x00 for standard DMX)
        if (receivedByte == 0x00) {
          channelIndex = 0;
        } else {
          inFrame = false;  // Invalid start code
        }
      } else if (channelIndex < DMX_CHANNELS) {
        dmxData[channelIndex] = receivedByte;
        channelIndex++;
        if (channelIndex >= DMX_CHANNELS) {
          inFrame = false;  // Frame complete
        }
      }
    }
  }
  
  // Send ArtNet packet at regular intervals
  unsigned long now = millis();
  if (now - lastDmxUpdate >= DMX_UPDATE_INTERVAL) {
    lastDmxUpdate = now;
    
    // Parse target IP address
    artnet.setUniverse(artnetUniverse);
    artnet.setLength(512);
    memcpy(artnet.getDmxFrame(), dmxData, 512);
    IPAddress targetAddr;
    if (targetAddr.fromString(targetIP)) {
      artnet.write(targetAddr);
    } else {
      // Default to broadcast if IP parsing fails
      artnet.write(IPAddress(255, 255, 255, 255));
    }
  }
}
