#include <WiFi.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP_I2S.h>

// ESP32-S3 WiFi provisioning via captive portal.
//
// Boot flow:
//   1. Read saved WiFi credentials from NVS (Preferences).
//   2. If present -> try to connect (STA). Success -> normal operation.
//   3. No credentials / connect failed -> open an OPEN SoftAP "ESP32-Setup",
//      hijack all DNS to ourselves so the phone's OS shows a captive-portal
//      popup, serve a page that lists nearby WiFi, let the user pick one and
//      enter the password, then connect + persist the credentials.
//
// The connect step is asynchronous (driven from loop) and the portal pages
// poll /status via <meta refresh> so the captive-portal webview never blocks.

// ---------- config ----------
const unsigned long SERIAL_BAUD_RATE = 115200;
const char *AP_SSID = "ESP32-Setup";  // open network (no password)
const byte DNS_PORT = 53;
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_NETMASK(255, 255, 255, 0);
const unsigned long STA_CONNECT_TIMEOUT_MS = 15000;
const unsigned long AP_LINGER_AFTER_SUCCESS_MS = 8000;  // keep AP up so the
                                                        // success page loads
const char *PREFS_NAMESPACE = "wifi";
const char *DEFAULT_ADMIN_URL = "http://203.195.202.54:8766";
const char *DEFAULT_WS_URL = "ws://203.195.202.54:8766/xiaozhi/v1";
const unsigned long ADMIN_CHAT_TIMEOUT_MS = 65000;
const unsigned long WS_TIMEOUT_MS = 12000;
const size_t WS_MAX_TEXT_FRAME = 2048;
const int PDM_MIC_CLK_PIN = 20;
const int PDM_MIC_DATA_PIN = 22;
const int AUDIO_BCLK_PIN = 18;
const int AUDIO_LRCLK_PIN = 19;
const int AUDIO_SDATA_PIN = 21;
const uint32_t MIC_SAMPLE_RATE = 48000;
const size_t MIC_READ_SAMPLES = 512;
const unsigned long MIC_TEST_MS = 3000;
const uint32_t SPEAKER_SAMPLE_RATE = 16000;
const uint16_t SPEAKER_TONE_HZ = 880;
const int16_t SPEAKER_TONE_AMPLITUDE = 2500;
const unsigned long SPEAKER_TEST_MS = 1000;

// ---------- state ----------
enum PortalState { PS_IDLE, PS_CONNECTING, PS_SUCCESS, PS_FAILED };

DNSServer dnsServer;
WebServer server(80);
Preferences prefs;
I2SClass micI2S;
I2SClass speakerI2S;

bool provisioning = false;
String cachedOptions;  // cached <option> list from the last scan
PortalState pstate = PS_IDLE;
String pendingSsid, pendingPass;
bool connectRequested = false;
unsigned long connectStartAt = 0;
unsigned long switchToStaAt = 0;  // 0 = no pending switch to STA-only
String serialLine;
bool micReady = false;

// ---------- credentials (NVS) ----------
bool loadCredentials(String &ssid, String &pass) {
  prefs.begin(PREFS_NAMESPACE, true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

void saveCredentials(const String &ssid, const String &pass) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

String loadAdminUrl() {
  prefs.begin(PREFS_NAMESPACE, true);
  String url = prefs.getString("admin_url", DEFAULT_ADMIN_URL);
  prefs.end();
  return url;
}

void saveAdminUrl(const String &url) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("admin_url", url);
  prefs.end();
}

String loadWebSocketUrl() {
  prefs.begin(PREFS_NAMESPACE, true);
  String url = prefs.getString("ws_url", DEFAULT_WS_URL);
  prefs.end();
  return url;
}

void saveWebSocketUrl(const String &url) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("ws_url", url);
  prefs.end();
}

String loadDeviceToken() {
  prefs.begin(PREFS_NAMESPACE, true);
  String token = prefs.getString("device_token", "");
  prefs.end();
  return token;
}

void saveDeviceToken(const String &token) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("device_token", token);
  prefs.end();
}

// ---------- LLM admin backend ----------
String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String jsonUnescape(const String &value) {
  String out;
  out.reserve(value.length());
  bool escaping = false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (escaping) {
      if (c == 'n') {
        out += '\n';
      } else if (c == 'r') {
        out += '\r';
      } else if (c == 't') {
        out += '\t';
      } else {
        out += c;
      }
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else {
      out += c;
    }
  }
  return out;
}

String extractJsonString(const String &json, const String &key) {
  String marker = "\"" + key + "\":";
  int keyPos = json.indexOf(marker);
  if (keyPos < 0) {
    return "";
  }
  int valueStart = json.indexOf('"', keyPos + marker.length());
  if (valueStart < 0) {
    return "";
  }
  String raw;
  bool escaping = false;
  for (int i = valueStart + 1; i < json.length(); i++) {
    char c = json.charAt(i);
    if (escaping) {
      raw += '\\';
      raw += c;
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else if (c == '"') {
      return jsonUnescape(raw);
    } else {
      raw += c;
    }
  }
  return "";
}

String buildAdminEndpoint(const char *path) {
  String adminUrl = loadAdminUrl();
  if (adminUrl.endsWith("/")) {
    adminUrl.remove(adminUrl.length() - 1);
  }
  return adminUrl + path;
}

String compactMacAddress() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return mac.length() > 0 ? mac : "unknown";
}

bool parseWebSocketUrl(const String &url, String &host, uint16_t &port,
                       String &path) {
  const String prefix = "ws://";
  if (!url.startsWith(prefix)) {
    return false;
  }

  int hostStart = prefix.length();
  int pathStart = url.indexOf('/', hostStart);
  String hostPort = pathStart >= 0 ? url.substring(hostStart, pathStart)
                                  : url.substring(hostStart);
  path = pathStart >= 0 ? url.substring(pathStart) : "/";
  if (hostPort.length() == 0 || path.length() == 0) {
    return false;
  }

  int colon = hostPort.lastIndexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    int parsedPort = hostPort.substring(colon + 1).toInt();
    if (host.length() == 0 || parsedPort <= 0 || parsedPort > 65535) {
      return false;
    }
    port = (uint16_t)parsedPort;
  } else {
    host = hostPort;
    port = 80;
  }
  return host.length() > 0;
}

bool readUntilMarker(WiFiClient &client, const String &marker, String &out,
                     unsigned long timeoutMs) {
  unsigned long deadline = millis() + timeoutMs;
  out = "";
  while (millis() < deadline) {
    while (client.available() > 0) {
      out += (char)client.read();
      if (out.endsWith(marker)) {
        return true;
      }
      if (out.length() > 4096) {
        return false;
      }
    }
    delay(5);
  }
  return false;
}

bool readExact(WiFiClient &client, uint8_t *buffer, size_t length,
               unsigned long timeoutMs) {
  size_t offset = 0;
  unsigned long deadline = millis() + timeoutMs;
  while (offset < length && millis() < deadline) {
    int available = client.available();
    if (available <= 0) {
      delay(5);
      continue;
    }
    int readCount = client.read(buffer + offset, length - offset);
    if (readCount > 0) {
      offset += (size_t)readCount;
    }
  }
  return offset == length;
}

bool sendWebSocketText(WiFiClient &client, const String &payload) {
  size_t length = payload.length();
  uint8_t header[4];
  header[0] = 0x81;  // FIN + text frame
  if (length <= 125) {
    header[1] = 0x80 | (uint8_t)length;
    client.write(header, 2);
  } else if (length <= 65535) {
    header[1] = 0x80 | 126;
    header[2] = (uint8_t)((length >> 8) & 0xff);
    header[3] = (uint8_t)(length & 0xff);
    client.write(header, 4);
  } else {
    return false;
  }

  const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  client.write(mask, sizeof(mask));
  for (size_t i = 0; i < length; i++) {
    uint8_t b = ((uint8_t)payload[i]) ^ mask[i % 4];
    client.write(&b, 1);
  }
  return true;
}

bool receiveWebSocketText(WiFiClient &client, String &payload,
                          uint8_t &opcode) {
  uint8_t header[2];
  if (!readExact(client, header, 2, WS_TIMEOUT_MS)) {
    return false;
  }

  opcode = header[0] & 0x0f;
  uint64_t length = header[1] & 0x7f;
  if (length == 126) {
    uint8_t ext[2];
    if (!readExact(client, ext, 2, WS_TIMEOUT_MS)) {
      return false;
    }
    length = ((uint16_t)ext[0] << 8) | ext[1];
  } else if (length == 127) {
    uint8_t ext[8];
    if (!readExact(client, ext, 8, WS_TIMEOUT_MS)) {
      return false;
    }
    length = 0;
    for (int i = 0; i < 8; i++) {
      length = (length << 8) | ext[i];
    }
  }

  uint8_t mask[4] = {0, 0, 0, 0};
  bool masked = (header[1] & 0x80) != 0;
  if (masked && !readExact(client, mask, 4, WS_TIMEOUT_MS)) {
    return false;
  }
  if (length > WS_MAX_TEXT_FRAME) {
    return false;
  }

  payload = "";
  payload.reserve((size_t)length);
  for (uint64_t i = 0; i < length; i++) {
    uint8_t b;
    if (!readExact(client, &b, 1, WS_TIMEOUT_MS)) {
      return false;
    }
    if (masked) {
      b ^= mask[i % 4];
    }
    payload += (char)b;
  }
  return true;
}

void addJsonDeviceHeaders(HTTPClient &http) {
  http.addHeader("Content-Type", "application/json");
  String deviceToken = loadDeviceToken();
  if (deviceToken.length() > 0) {
    http.addHeader("X-Device-Token", deviceToken);
  }
}

bool ensureMicReady() {
  if (micReady) {
    return true;
  }
  micI2S.setTimeout(1000);
  micI2S.setPinsPdmRx(PDM_MIC_CLK_PIN, PDM_MIC_DATA_PIN);
  micReady = micI2S.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE,
                          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
                          I2S_STD_SLOT_RIGHT);
  if (!micReady) {
    Serial.printf("PDM mic init failed, lastError=%d\n", micI2S.lastError());
  }
  return micReady;
}

void testPdmMicrophone() {
  if (!ensureMicReady()) {
    return;
  }

  int16_t samples[MIC_READ_SAMPLES];
  uint32_t chunks = 0;
  uint32_t totalSamples = 0;
  int32_t minSample = INT32_MAX;
  int32_t maxSample = INT32_MIN;
  int64_t sum = 0;
  uint64_t sumSquares = 0;
  unsigned long deadline = millis() + MIC_TEST_MS;

  Serial.printf("PDM mic test: CLK=GPIO%d DATA=GPIO%d rate=%luHz duration=%lums\n",
                PDM_MIC_CLK_PIN, PDM_MIC_DATA_PIN,
                (unsigned long)MIC_SAMPLE_RATE, (unsigned long)MIC_TEST_MS);
  Serial.println("Speak near the microphone now...");

  while (millis() < deadline) {
    size_t bytesRead = micI2S.readBytes((char *)samples, sizeof(samples));
    size_t sampleCount = bytesRead / sizeof(samples[0]);
    if (sampleCount == 0) {
      delay(5);
      continue;
    }
    chunks++;
    totalSamples += sampleCount;
    for (size_t i = 0; i < sampleCount; i++) {
      int32_t value = samples[i];
      sum += value;
      sumSquares += (int64_t)value * value;
      if (value < minSample) {
        minSample = value;
      }
      if (value > maxSample) {
        maxSample = value;
      }
    }
  }

  if (totalSamples == 0) {
    Serial.println("PDM mic test failed: no samples read.");
    return;
  }

  double mean = (double)sum / (double)totalSamples;
  double rms = sqrt((double)sumSquares / (double)totalSamples);
  int32_t peak = max(abs(minSample), abs(maxSample));
  Serial.printf("PDM mic samples=%lu chunks=%lu min=%ld max=%ld peak=%ld mean=%.1f rms=%.1f\n",
                (unsigned long)totalSamples, (unsigned long)chunks,
                (long)minSample, (long)maxSample, (long)peak, mean, rms);
}

void testSpeakerTone() {
  if (micReady) {
    micI2S.end();
    micReady = false;
  }

  speakerI2S.setTimeout(1000);
  speakerI2S.setPins(AUDIO_BCLK_PIN, AUDIO_LRCLK_PIN, AUDIO_SDATA_PIN);
  if (!speakerI2S.begin(I2S_MODE_STD, SPEAKER_SAMPLE_RATE,
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_STD_SLOT_BOTH)) {
    Serial.printf("Speaker I2S init failed, lastError=%d\n",
                  speakerI2S.lastError());
    return;
  }

  Serial.printf("Speaker test: BCLK=GPIO%d LRCLK=GPIO%d SDATA=GPIO%d rate=%luHz tone=%uHz duration=%lums\n",
                AUDIO_BCLK_PIN, AUDIO_LRCLK_PIN, AUDIO_SDATA_PIN,
                (unsigned long)SPEAKER_SAMPLE_RATE, SPEAKER_TONE_HZ,
                (unsigned long)SPEAKER_TEST_MS);

  const size_t framesPerChunk = 128;
  int16_t samples[framesPerChunk * 2];
  uint32_t totalFrames = SPEAKER_SAMPLE_RATE * SPEAKER_TEST_MS / 1000;
  uint32_t writtenFrames = 0;
  while (writtenFrames < totalFrames) {
    size_t frames = min((uint32_t)framesPerChunk, totalFrames - writtenFrames);
    for (size_t i = 0; i < frames; i++) {
      float phase = 2.0f * PI * SPEAKER_TONE_HZ *
                    (float)(writtenFrames + i) / (float)SPEAKER_SAMPLE_RATE;
      int16_t sample = (int16_t)(sinf(phase) * SPEAKER_TONE_AMPLITUDE);
      samples[i * 2] = sample;
      samples[i * 2 + 1] = sample;
    }
    speakerI2S.write((uint8_t *)samples, frames * 2 * sizeof(int16_t));
    writtenFrames += frames;
  }

  memset(samples, 0, sizeof(samples));
  for (int i = 0; i < 4; i++) {
    speakerI2S.write((uint8_t *)samples, sizeof(samples));
  }
  speakerI2S.end();
  Serial.println("Speaker test done.");
}

void printCommandHelp() {
  Serial.println("Commands:");
  Serial.println("  help");
  Serial.println("  admin <http://host:8766>");
  Serial.println("  ws <ws://host:8766/xiaozhi/v1>");
  Serial.println("  token <auth-token>   (empty to clear)");
  Serial.println("  ask <prompt>");
  Serial.println("  askstream <prompt>");
  Serial.println("  wshello");
  Serial.println("  mictest");
  Serial.println("  speakertest");
  Serial.printf("Current admin URL: %s\n", loadAdminUrl().c_str());
  Serial.printf("Current WebSocket URL: %s\n", loadWebSocketUrl().c_str());
  Serial.printf("Auth token: %s\n", loadDeviceToken().length() > 0 ? "set" : "unset");
}

void testXiaozhiWebSocketHello() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi is not connected; cannot open WebSocket.");
    return;
  }

  String url = loadWebSocketUrl();
  String host, path;
  uint16_t port = 80;
  if (!parseWebSocketUrl(url, host, port, path)) {
    Serial.println("WebSocket URL must look like ws://host:port/path");
    return;
  }

  String mac = compactMacAddress();
  String deviceId = "esp32-" + mac;
  String clientId = deviceId + "-client";
  String token = loadDeviceToken();

  WiFiClient client;
  client.setTimeout(WS_TIMEOUT_MS / 1000);
  Serial.printf("WS connect %s\n", url.c_str());
  if (!client.connect(host.c_str(), port)) {
    Serial.println("WebSocket TCP connect failed.");
    return;
  }

  String request;
  request.reserve(512 + token.length());
  request += "GET " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + ":" + String(port) + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += "Protocol-Version: 1\r\n";
  request += "Device-Id: " + deviceId + "\r\n";
  request += "Client-Id: " + clientId + "\r\n";
  if (token.length() > 0) {
    request += "Authorization: Bearer " + token + "\r\n";
  }
  request += "\r\n";
  client.print(request);

  String responseHeaders;
  if (!readUntilMarker(client, "\r\n\r\n", responseHeaders, WS_TIMEOUT_MS)) {
    Serial.println("WebSocket handshake timed out.");
    client.stop();
    return;
  }
  if (!responseHeaders.startsWith("HTTP/1.1 101") &&
      !responseHeaders.startsWith("HTTP/1.0 101")) {
    Serial.println("WebSocket handshake failed:");
    Serial.println(responseHeaders);
    client.stop();
    return;
  }
  Serial.println("WebSocket upgraded.");

  String hello =
      "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},"
      "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
      "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
  if (!sendWebSocketText(client, hello)) {
    Serial.println("Failed to send WebSocket hello.");
    client.stop();
    return;
  }

  String frame;
  uint8_t opcode = 0;
  if (!receiveWebSocketText(client, frame, opcode)) {
    Serial.println("Timed out waiting for server hello.");
    client.stop();
    return;
  }
  if (opcode == 0x8) {
    Serial.println("WebSocket closed before hello.");
    client.stop();
    return;
  }
  if (opcode != 0x1) {
    Serial.printf("Unexpected WebSocket opcode: %u\n", opcode);
    client.stop();
    return;
  }

  Serial.println("WebSocket server hello:");
  Serial.println(frame);
  client.stop();
}

void askAdminBackend(const String &prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi is not connected; cannot call admin backend.");
    return;
  }

  HTTPClient http;
  http.setTimeout(ADMIN_CHAT_TIMEOUT_MS);
  String endpoint = buildAdminEndpoint("/api/chat");
  Serial.printf("POST %s\n", endpoint.c_str());
  if (!http.begin(endpoint)) {
    Serial.println("HTTP begin failed.");
    return;
  }
  addJsonDeviceHeaders(http);

  String body = "{\"prompt\":\"" + jsonEscape(prompt) + "\"}";
  int code = http.POST(body);
  String response = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("LLM backend failed: HTTP %d\n", code);
    Serial.println(response);
    return;
  }

  String answer = extractJsonString(response, "answer");
  if (answer.length() == 0) {
    Serial.println("LLM backend response missing answer:");
    Serial.println(response);
    return;
  }

  Serial.println("LLM answer:");
  Serial.println(answer);
}

void dispatchSseEvent(const String &eventName, const String &dataLine, bool &printedAny) {
  if (eventName == "response.output_text.delta") {
    String delta = extractJsonString(dataLine, "delta");
    if (delta.length() > 0) {
      Serial.print(delta);
      printedAny = true;
    }
  } else if (eventName == "error") {
    String error = extractJsonString(dataLine, "error");
    Serial.println();
    Serial.print("LLM stream error: ");
    Serial.println(error.length() > 0 ? error : dataLine);
  }
}

void processSseLine(String line, String &eventName, String &dataLine, bool &printedAny) {
  if (line.endsWith("\r")) {
    line.remove(line.length() - 1);
  }
  if (line.length() == 0) {
    dispatchSseEvent(eventName, dataLine, printedAny);
    eventName = "";
    dataLine = "";
    return;
  }
  if (line.startsWith("event:")) {
    eventName = line.substring(6);
    eventName.trim();
    return;
  }
  if (line.startsWith("data:")) {
    String payload = line.substring(5);
    if (payload.startsWith(" ")) {
      payload.remove(0, 1);
    }
    if (dataLine.length() > 0) {
      dataLine += "\n";
    }
    dataLine += payload;
  }
}

void askStreamAdminBackend(const String &prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi is not connected; cannot call admin backend.");
    return;
  }

  HTTPClient http;
  http.setTimeout(ADMIN_CHAT_TIMEOUT_MS);
  String endpoint = buildAdminEndpoint("/api/chat/stream");
  Serial.printf("POST %s\n", endpoint.c_str());
  if (!http.begin(endpoint)) {
    Serial.println("HTTP begin failed.");
    return;
  }
  addJsonDeviceHeaders(http);
  http.addHeader("Accept", "text/event-stream");

  String body = "{\"prompt\":\"" + jsonEscape(prompt) + "\"}";
  int code = http.POST(body);
  if (code != 200) {
    String response = http.getString();
    http.end();
    Serial.printf("LLM stream backend failed: HTTP %d\n", code);
    Serial.println(response);
    return;
  }

  Serial.println("LLM stream:");
  NetworkClient *stream = http.getStreamPtr();
  int len = http.getSize();
  String line;
  String eventName;
  String dataLine;
  bool printedAny = false;

  while (http.connected() && (len > 0 || len == -1)) {
    size_t size = stream->available();
    if (size) {
      char c = (char)stream->read();
      if (len > 0) {
        len--;
      }
      if (c == '\n') {
        processSseLine(line, eventName, dataLine, printedAny);
        line = "";
      } else {
        line += c;
      }
    } else {
      delay(1);
    }
  }

  if (line.length() > 0) {
    processSseLine(line, eventName, dataLine, printedAny);
  }
  if (eventName.length() > 0 || dataLine.length() > 0) {
    dispatchSseEvent(eventName, dataLine, printedAny);
  }
  http.end();

  if (printedAny) {
    Serial.println();
  }
  Serial.println("LLM stream done.");
}

void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }
  if (line == "help") {
    printCommandHelp();
  } else if (line.startsWith("admin ")) {
    String url = line.substring(6);
    url.trim();
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
      Serial.println("Admin URL must start with http:// or https://");
      return;
    }
    saveAdminUrl(url);
    Serial.printf("Admin URL saved: %s\n", url.c_str());
  } else if (line.startsWith("ws ")) {
    String url = line.substring(3);
    url.trim();
    if (!url.startsWith("ws://")) {
      Serial.println("WebSocket URL must start with ws://");
      return;
    }
    saveWebSocketUrl(url);
    Serial.printf("WebSocket URL saved: %s\n", url.c_str());
  } else if (line.startsWith("token ")) {
    String token = line.substring(6);
    token.trim();
    saveDeviceToken(token);
    Serial.println(token.length() > 0 ? "Auth token saved." : "Auth token cleared.");
  } else if (line.startsWith("ask ")) {
    String prompt = line.substring(4);
    prompt.trim();
    if (prompt.length() == 0) {
      Serial.println("Prompt is empty.");
      return;
    }
    askAdminBackend(prompt);
  } else if (line.startsWith("askstream ")) {
    String prompt = line.substring(10);
    prompt.trim();
    if (prompt.length() == 0) {
      Serial.println("Prompt is empty.");
      return;
    }
    askStreamAdminBackend(prompt);
  } else if (line == "wshello") {
    testXiaozhiWebSocketHello();
  } else if (line == "mictest") {
    testPdmMicrophone();
  } else if (line == "speakertest") {
    testSpeakerTone();
  } else {
    Serial.println("Unknown command. Type help.");
  }
}

void processSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      handleSerialLine(serialLine);
      serialLine = "";
    } else if (c != '\r') {
      serialLine += c;
      if (serialLine.length() > 512) {
        serialLine = "";
        Serial.println("Serial command too long; cleared.");
      }
    }
  }
}

// ---------- STA connect (blocking, used only at boot) ----------
bool connectStaBlocking(const String &ssid, const String &pass) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// ---------- scan ----------
void scanNetworks() {
  Serial.println("Scanning networks for portal...");
  int n = WiFi.scanNetworks();
  cachedOptions = "";
  if (n <= 0) {
    cachedOptions = "<option value=''>(no networks found - tap rescan)</option>";
  } else {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      // TODO(escape): an SSID containing " or < breaks the HTML attribute;
      // first version does not HTML-escape. Add escaping before production.
      cachedOptions += "<option value=\"" + ssid + "\">" + ssid + "  (" +
                       String(WiFi.RSSI(i)) + "dBm)</option>";
    }
  }
  WiFi.scanDelete();
  Serial.printf("Scan done, %d networks.\n", n);
}

// ---------- HTML helpers ----------
String pageHeader(const String &title) {
  return "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" +
         title +
         "</title><style>"
         "body{font-family:-apple-system,BlinkMacSystemFont,Helvetica,Arial,"
         "sans-serif;margin:0;padding:24px;background:#f2f2f7;color:#1c1c1e}"
         ".card{max-width:420px;margin:0 auto;background:#fff;border-radius:14px;"
         "padding:22px;box-shadow:0 1px 6px rgba(0,0,0,.12)}"
         "h2{margin:0 0 16px;font-size:21px}"
         "label{display:block;margin:14px 0 6px;font-size:14px;color:#3a3a3c}"
         "select,input{width:100%;padding:12px;border:1px solid #d1d1d6;"
         "border-radius:10px;font-size:16px;box-sizing:border-box;background:#fff}"
         "button{width:100%;margin-top:22px;padding:14px;border:0;"
         "border-radius:10px;background:#007aff;color:#fff;font-size:17px}"
         "a{color:#007aff;display:inline-block;margin-top:16px;font-size:14px}"
         ".msg{padding:12px;border-radius:10px;margin-bottom:12px;font-size:15px}"
         ".ok{background:#e3f9e5;color:#1b7f2e}.err{background:#ffe5e5;color:#c0392b}"
         ".spin{color:#007aff}"
         "</style></head><body><div class='card'>";
}

String pageFooter() { return "</div></body></html>"; }

void sendHtml(const String &body) {
  server.send(200, "text/html; charset=utf-8", body);
}

// ---------- HTTP handlers ----------
void handleRoot() {
  String html = pageHeader("Device WiFi Setup");
  html += "<h2>\xF0\x9F\x93\xB6 \xE8\xBF\x9E\xE6\x8E\xA5 WiFi</h2>";  // 📶 连接 WiFi
  html += "<form action='/connect' method='POST'>";
  html += "<label>\xE9\x80\x89\xE6\x8B\xA9 WiFi \xE7\xBD\x91\xE7\xBB\x9C</label>";  // 选择 WiFi 网络
  html += "<select name='ssid'>" + cachedOptions + "</select>";
  html += "<label>WiFi \xE5\xAF\x86\xE7\xA0\x81</label>";  // WiFi 密码
  html += "<input type='password' name='pass' placeholder='password'>";
  html += "<button type='submit'>\xE8\xBF\x9E\xE6\x8E\xA5</button>";  // 连接
  html += "</form><a href='/rescan'>\xF0\x9F\x94\x84 \xE9\x87\x8D\xE6\x96\xB0\xE6\x89\xAB\xE6\x8F\x8F</a>";  // 🔄 重新扫描
  html += pageFooter();
  sendHtml(html);
}

void handleRescan() {
  scanNetworks();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleConnect() {
  pendingSsid = server.arg("ssid");
  pendingPass = server.arg("pass");
  Serial.printf("Portal connect request: ssid='%s'\n", pendingSsid.c_str());

  if (pendingSsid.length() == 0) {
    String html = pageHeader("Setup");
    html += "<div class='msg err'>\xE6\x9C\xAA\xE9\x80\x89\xE6\x8B\xA9 WiFi</div>";  // 未选择 WiFi
    html += "<a href='/'>\xE8\xBF\x94\xE5\x9B\x9E</a>";  // 返回
    html += pageFooter();
    sendHtml(html);
    return;
  }

  connectRequested = true;  // loop() will start the actual connection
  pstate = PS_CONNECTING;

  // "Connecting..." page that auto-polls /status every 2s.
  String html =
      "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='2;url=/status'>"
      "<title>Connecting</title></head><body style='font-family:sans-serif;"
      "text-align:center;padding-top:60px'>"
      "<h2 style='color:#007aff'>\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5\xE2\x80\xA6</h2>"  // 正在连接…
      "<p>\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5 <b>" +
      pendingSsid +
      "</b><br>\xE8\xAF\xB7\xE7\xA8\x8D\xE5\x80\x99\xEF\xBC\x8C\xE9\xA1\xB5\xE9\x9D\xA2\xE4\xBC\x9A\xE8\x87\xAA\xE5\x8A\xA8\xE5\x88\xB7\xE6\x96\xB0</p>"  // 请稍候，页面会自动刷新
      "</body></html>";
  sendHtml(html);
}

void handleStatus() {
  if (pstate == PS_SUCCESS) {
    String ip = WiFi.localIP().toString();
    String html = pageHeader("Success");
    html += "<div class='msg ok'>\xE2\x9C\x85 \xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5\xE5\x88\xB0 <b>" + pendingSsid + "</b></div>";  // ✅ 已连接到
    html += "<p>\xE8\xAE\xBE\xE5\xA4\x87 IP\xEF\xBC\x9A<b>" + ip + "</b></p>";  // 设备 IP：
    html += "<p>\xE5\xAF\x86\xE7\xA0\x81\xE5\xB7\xB2\xE4\xBF\x9D\xE5\xAD\x98\xEF\xBC\x8C\xE4\xB8\x8B\xE6\xAC\xA1\xE9\x80\x9A\xE7\x94\xB5\xE8\x87\xAA\xE5\x8A\xA8\xE8\xBF\x9E\xE6\x8E\xA5\xE3\x80\x82\xE7\x83\xAD\xE7\x82\xB9\xE5\x8D\xB3\xE5\xB0\x86\xE5\x85\xB3\xE9\x97\xAD\xE3\x80\x82</p>";  // 密码已保存，下次通电自动连接。热点即将关闭。
    html += pageFooter();
    sendHtml(html);
  } else if (pstate == PS_FAILED) {
    String html = pageHeader("Failed");
    html += "<div class='msg err'>\xE2\x9D\x8C \xE8\xBF\x9E\xE6\x8E\xA5 <b>" + pendingSsid + "</b> \xE5\xA4\xB1\xE8\xB4\xA5</div>";  // ❌ 连接 失败
    html += "<p>\xE8\xAF\xB7\xE6\xA3\x80\xE6\x9F\xA5\xE5\xAF\x86\xE7\xA0\x81\xE5\x90\x8E\xE9\x87\x8D\xE8\xAF\x95\xE3\x80\x82</p>";  // 请检查密码后重试。
    html += "<a href='/'>\xE8\xBF\x94\xE5\x9B\x9E\xE9\x87\x8D\xE8\xAF\x95</a>";  // 返回重试
    html += pageFooter();
    sendHtml(html);
    pstate = PS_IDLE;  // allow retry
  } else if (pstate == PS_CONNECTING) {
    String html =
        "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='2;url=/status'>"
        "<title>Connecting</title></head><body style='font-family:sans-serif;"
        "text-align:center;padding-top:60px'>"
        "<h2 style='color:#007aff'>\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD\xE2\x80\xA6</h2>"  // 连接中…
        "</body></html>";
    sendHtml(html);
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  }
}

// ---------- provisioning lifecycle ----------
void startProvisioning() {
  provisioning = true;
  pstate = PS_IDLE;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(AP_SSID);  // open network
  Serial.printf("SoftAP '%s' up, IP=%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());

  scanNetworks();

  dnsServer.start(DNS_PORT, "*", AP_IP);  // hijack all DNS -> captive portal

  server.on("/", HTTP_GET, handleRoot);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/rescan", HTTP_GET, handleRescan);
  // Every other request (OS captive-portal probes) funnels to the portal page,
  // which makes iOS / Android / Windows show the "sign in to network" popup.
  server.onNotFound(handleRoot);
  server.begin();
  Serial.println("HTTP server started. Join the AP; the portal should pop up.");
}

void finishProvisioning() {
  Serial.println("Switching to STA-only mode.");
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  provisioning = false;
  switchToStaAt = 0;
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 WiFi provisioning ===");
  printCommandHelp();

  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    Serial.printf("Saved credentials found, connecting to '%s'...\n",
                  ssid.c_str());
    WiFi.mode(WIFI_STA);
    if (connectStaBlocking(ssid, pass)) {
      Serial.printf("Connected. IP=%s\n", WiFi.localIP().toString().c_str());
      Serial.println("Type 'ask <prompt>' to call the LLM admin backend.");
      return;  // stay in STA / normal operation
    }
    Serial.println("Saved credentials failed; starting provisioning portal.");
  } else {
    Serial.println("No saved credentials; starting provisioning portal.");
  }
  startProvisioning();
}

void loop() {
  processSerialCommands();

  if (provisioning) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (connectRequested) {
      connectRequested = false;
      Serial.printf("Connecting to '%s'...\n", pendingSsid.c_str());
      WiFi.begin(pendingSsid.c_str(), pendingPass.c_str());
      connectStartAt = millis();
    }

    if (pstate == PS_CONNECTING && connectStartAt != 0) {
      if (WiFi.status() == WL_CONNECTED) {
        pstate = PS_SUCCESS;
        saveCredentials(pendingSsid, pendingPass);
        switchToStaAt = millis() + AP_LINGER_AFTER_SUCCESS_MS;
        Serial.printf("Provision OK, IP=%s\n",
                      WiFi.localIP().toString().c_str());
      } else if (millis() - connectStartAt > STA_CONNECT_TIMEOUT_MS) {
        pstate = PS_FAILED;
        connectStartAt = 0;
        WiFi.disconnect();
        Serial.println("Provision connect timed out.");
      }
    }

    if (switchToStaAt != 0 && millis() >= switchToStaAt &&
        pstate == PS_SUCCESS) {
      finishProvisioning();
    }
  } else {
    static unsigned long last = 0;
    if (millis() - last > 5000) {
      last = millis();
      Serial.printf("[STA] connected=%d ip=%s rssi=%d\n",
                    WiFi.status() == WL_CONNECTED ? 1 : 0,
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    }
    delay(50);
  }
}
