#include <WiFi.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>

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
const char *DEFAULT_ADMIN_URL = "http://192.168.3.100:8766";
const unsigned long ADMIN_CHAT_TIMEOUT_MS = 65000;

// ---------- state ----------
enum PortalState { PS_IDLE, PS_CONNECTING, PS_SUCCESS, PS_FAILED };

DNSServer dnsServer;
WebServer server(80);
Preferences prefs;

bool provisioning = false;
String cachedOptions;  // cached <option> list from the last scan
PortalState pstate = PS_IDLE;
String pendingSsid, pendingPass;
bool connectRequested = false;
unsigned long connectStartAt = 0;
unsigned long switchToStaAt = 0;  // 0 = no pending switch to STA-only
String serialLine;

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

void printCommandHelp() {
  Serial.println("Commands:");
  Serial.println("  help");
  Serial.println("  admin <http://host:8766>");
  Serial.println("  token <device-token>   (empty to clear)");
  Serial.println("  ask <prompt>");
  Serial.printf("Current admin URL: %s\n", loadAdminUrl().c_str());
  Serial.printf("Device token: %s\n", loadDeviceToken().length() > 0 ? "set" : "unset");
}

void askAdminBackend(const String &prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi is not connected; cannot call admin backend.");
    return;
  }

  String adminUrl = loadAdminUrl();
  if (adminUrl.endsWith("/")) {
    adminUrl.remove(adminUrl.length() - 1);
  }

  HTTPClient http;
  http.setTimeout(ADMIN_CHAT_TIMEOUT_MS);
  String endpoint = adminUrl + "/api/chat";
  Serial.printf("POST %s\n", endpoint.c_str());
  if (!http.begin(endpoint)) {
    Serial.println("HTTP begin failed.");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  String deviceToken = loadDeviceToken();
  if (deviceToken.length() > 0) {
    http.addHeader("X-Device-Token", deviceToken);
  }

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
  } else if (line.startsWith("token ")) {
    String token = line.substring(6);
    token.trim();
    saveDeviceToken(token);
    Serial.println(token.length() > 0 ? "Device token saved." : "Device token cleared.");
  } else if (line.startsWith("ask ")) {
    String prompt = line.substring(4);
    prompt.trim();
    if (prompt.length() == 0) {
      Serial.println("Prompt is empty.");
      return;
    }
    askAdminBackend(prompt);
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
