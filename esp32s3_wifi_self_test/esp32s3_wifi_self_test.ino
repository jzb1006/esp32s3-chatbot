#include <WiFi.h>

// WiFi-only radio self test for ESP32-S3 (native USB / HWCDC serial).
// Prints chip info, then repeatedly scans for nearby WiFi networks and
// emits a once-per-second heartbeat so a freshly attached USB host can
// immediately confirm the firmware is alive.

const unsigned long SERIAL_BAUD_RATE = 115200;
const unsigned long WIFI_SCAN_INTERVAL_MS = 8000;
const unsigned long HOST_ATTACH_TIMEOUT_MS = 5000;

unsigned long lastWifiScanAt = 0;

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);

  // HWCDC drops bytes written before the USB host attaches. Wait a moment
  // (but never block forever) so the boot banner is not lost.
  unsigned long startedAt = millis();
  while (!Serial && (millis() - startedAt) < HOST_ATTACH_TIMEOUT_MS) {
    delay(50);
  }
  delay(500);

  Serial.println();
  Serial.println("=== ESP32-S3 WiFi self test boot ===");
  printChipInfo();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
}

void loop() {
  unsigned long now = millis();
  if (lastWifiScanAt == 0 || now - lastWifiScanAt >= WIFI_SCAN_INTERVAL_MS) {
    scanWifiNetworks();
  }
  Serial.printf("heartbeat uptime=%lus heap=%u\n",
                now / 1000, (unsigned)ESP.getFreeHeap());
  delay(1000);
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
  Serial.print("Free heap bytes: ");
  Serial.println(ESP.getFreeHeap());
}

void scanWifiNetworks() {
  lastWifiScanAt = millis();

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
    Serial.printf("  %2d) %-32s RSSI=%4d dBm CH=%2d ENC=%d\n",
                  i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.channel(i), WiFi.encryptionType(i));
  }
  WiFi.scanDelete();
}
