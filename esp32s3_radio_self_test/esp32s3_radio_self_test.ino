#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

const unsigned long SERIAL_BAUD_RATE = 115200;
const char *BLE_DEVICE_NAME = "ESP32S3-Radio-Test";
const unsigned long WIFI_SCAN_INTERVAL_MS = 15000;

unsigned long lastWifiScanAt = 0;

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-S3 radio self test starting...");
  printChipInfo();

  startBleAdvertising();
  scanWifiNetworks();
}

void loop() {
  unsigned long now = millis();
  if (now - lastWifiScanAt >= WIFI_SCAN_INTERVAL_MS) {
    scanWifiNetworks();
  }
  delay(100);
}

void printChipInfo() {
  Serial.print("Chip model: ");
  Serial.println(ESP.getChipModel());
  Serial.print("Chip revision: ");
  Serial.println(ESP.getChipRevision());
  Serial.print("CPU frequency MHz: ");
  Serial.println(ESP.getCpuFreqMHz());
  Serial.print("Flash size bytes: ");
  Serial.println(ESP.getFlashChipSize());
}

void startBleAdvertising() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer *server = BLEDevice::createServer();
  BLEAdvertising *advertising = server->getAdvertising();
  advertising->setScanResponse(true);
  advertising->start();
  BLEDevice::startAdvertising();

  Serial.print("BLE advertising as: ");
  Serial.println(BLE_DEVICE_NAME);
}

void scanWifiNetworks() {
  lastWifiScanAt = millis();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.println("Scanning WiFi networks...");
  int networkCount = WiFi.scanNetworks();
  if (networkCount < 0) {
    Serial.print("WiFi scan failed: ");
    Serial.println(networkCount);
    return;
  }

  Serial.print("WiFi networks found: ");
  Serial.println(networkCount);
  for (int i = 0; i < networkCount; i++) {
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(WiFi.SSID(i));
    Serial.print(" RSSI=");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm CH=");
    Serial.print(WiFi.channel(i));
    Serial.print(" ENC=");
    Serial.println(WiFi.encryptionType(i));
  }
  WiFi.scanDelete();
}
