// SPDX-License-Identifier: MIT
// Copyright (c) 2026 taiwansensor

#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <algorithm>
#include <time.h>

namespace {

constexpr char kVersion[] = "0.1.0-alpha.3";
constexpr char kDefaultHostname[] = "uart-logger";
constexpr char kApPassword[] = "uartlogger";
constexpr uint32_t kDefaultBaud = 115200;
constexpr int kDefaultRxPin = 4;
constexpr uint16_t kDefaultTcpPort = 2323;
constexpr size_t kHardwareRxBufferSize = 16 * 1024;
constexpr size_t kUartStreamSize = 32 * 1024;
constexpr size_t kRamRingSize = 24 * 1024;
constexpr size_t kMainSegmentLimit = 384 * 1024;
constexpr size_t kPanicFileLimit = 128 * 1024;
constexpr size_t kWifiSegmentLimit = 64 * 1024;
constexpr uint32_t kPanicCaptureMs = 90 * 1000;
constexpr uint32_t kLogFlushMs = 1000;
constexpr uint32_t kWifiConnectMs = 15000;
constexpr uint32_t kWifiHeartbeatMs = 5 * 60 * 1000;
constexpr size_t kTcpClientCount = 3;
constexpr size_t kWifiEventQueueLength = 32;

struct WifiEventRecord {
  uint32_t uptime_ms;
  arduino_event_id_t event_id;
  uint8_t reason;
  uint8_t channel;
  uint8_t client_mac[6];
};

struct RuntimeConfig {
  String ssid;
  String wifi_password;
  String hostname{kDefaultHostname};
  String web_user{"admin"};
  String web_password;
  uint32_t baud{kDefaultBaud};
  int rx_pin{kDefaultRxPin};
  uint16_t tcp_port{kDefaultTcpPort};
};

RuntimeConfig config;
Preferences preferences;
HardwareSerial target_uart(1);
WebServer web_server(80);
WiFiServer *tcp_server = nullptr;
WiFiClient tcp_clients[kTcpClientCount];
StreamBufferHandle_t uart_stream = nullptr;
TaskHandle_t uart_reader_task_handle = nullptr;
QueueHandle_t wifi_event_queue = nullptr;

File main_log;
File panic_log;
File wifi_log;
uint8_t active_segment = 0;
uint8_t active_wifi_segment = 0;
uint8_t next_panic_index = 0;
uint32_t panic_capture_until = 0;
uint32_t last_flush_ms = 0;
uint64_t total_uart_bytes = 0;
uint32_t panic_count = 0;
uint32_t tcp_drop_count = 0;
volatile uint32_t uart_error_count = 0;
volatile uint32_t uart_overflow_count = 0;
volatile uint32_t last_uart_error_ms = 0;
volatile uint32_t last_uart_overflow_ms = 0;
volatile uint32_t uart_stream_drop_bytes = 0;
uint32_t boot_count = 0;
uint32_t wifi_connected_events = 0;
uint32_t wifi_got_ip_events = 0;
uint32_t wifi_disconnected_events = 0;
uint32_t wifi_lost_ip_events = 0;
uint32_t fallback_ap_start_events = 0;
uint32_t fallback_ap_client_connects = 0;
uint32_t fallback_ap_client_disconnects = 0;
uint32_t last_wifi_connected_ms = 0;
uint32_t last_wifi_got_ip_ms = 0;
uint32_t last_wifi_disconnected_ms = 0;
uint32_t last_wifi_heartbeat_ms = 0;
uint8_t last_wifi_disconnect_reason = 0;
volatile uint32_t wifi_event_queue_drops = 0;
bool filesystem_ready = false;
bool ap_active = false;
bool mdns_ready = false;
bool ota_upload_started = false;
bool ota_upload_success = false;
size_t ota_received_bytes = 0;
uint32_t ota_reboot_at = 0;
String ota_status{"never"};

uint8_t ram_ring[kRamRingSize];
size_t ram_ring_head = 0;
size_t ram_ring_used = 0;
String detector_line;

const char kRootHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-C3 UART Logger</title><style>
body{font-family:system-ui,sans-serif;background:#10151c;color:#e8edf3;margin:0;padding:18px}
.wrap{max-width:1100px;margin:auto}.card{background:#18212c;border:1px solid #2b3948;border-radius:12px;padding:16px;margin:12px 0}
h1,h2{margin:.2em 0 .6em}a{color:#7ec8ff}button,.button{background:#2675b8;color:white;border:0;border-radius:7px;padding:9px 13px;text-decoration:none;cursor:pointer}
button.danger{background:#a33131}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}.metric{background:#111923;padding:10px;border-radius:8px}
.metric b{display:block;color:#8fb6d8;font-size:.85rem}pre{height:55vh;overflow:auto;background:#05080c;color:#b9f6ca;padding:12px;border-radius:8px;white-space:pre-wrap;word-break:break-all}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.muted{color:#9cacbc;font-size:.9rem}
</style></head><body><div class="wrap"><h1>ESP32-C3 UART Logger</h1>
<div class="card"><div class="grid" id="status"></div></div>
<div class="card"><div class="row"><button onclick="pause=!pause;this.textContent=pause?'繼續':'暫停'">暫停</button><button onclick="out.textContent=''">清除畫面</button><a class="button" href="/files">下載日誌</a><a class="button" href="/config">設定</a></div>
<p class="muted">即時畫面來自 RAM 尾端；LittleFS 日誌與 Panic 快照不受畫面清除影響。</p><pre id="out"></pre></div>
</div><script>
let cursor=0,pause=false;const out=document.getElementById('out');
async function poll(){try{const r=await fetch('/api/tail?cursor='+cursor,{cache:'no-store'});if(r.ok){cursor=Number(r.headers.get('X-Uart-Cursor')||cursor);const t=await r.text();if(!pause&&t){out.textContent+=t;if(out.textContent.length>250000)out.textContent=out.textContent.slice(-200000);out.scrollTop=out.scrollHeight;}}}catch(e){}setTimeout(poll,1000)}
async function status(){try{const r=await fetch('/api/status',{cache:'no-store'}),s=await r.json();document.getElementById('status').innerHTML=Object.entries(s).map(([k,v])=>`<div class="metric"><b>${k}</b>${v}</div>`).join('')}catch(e){}setTimeout(status,10000)}
poll();status();</script></body></html>
)HTML";

String html_escape(const String &value) {
  String output;
  output.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': output += F("&amp;"); break;
      case '<': output += F("&lt;"); break;
      case '>': output += F("&gt;"); break;
      case '"': output += F("&quot;"); break;
      case '\'': output += F("&#39;"); break;
      default: output += value[i]; break;
    }
  }
  return output;
}

String json_escape(const String &value) {
  String output;
  output.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"': output += F("\\\""); break;
      case '\\': output += F("\\\\"); break;
      case '\n': output += F("\\n"); break;
      case '\r': output += F("\\r"); break;
      case '\t': output += F("\\t"); break;
      default:
        if (static_cast<uint8_t>(c) >= 0x20) output += c;
        break;
    }
  }
  return output;
}

String uint64_string(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
}

String local_timestamp() {
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    return String(F("uptime-ms:")) + String(millis());
  }
  struct tm local_tm {};
  localtime_r(&now, &local_tm);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &local_tm);
  return String(buffer);
}

bool require_authentication() {
  if (config.web_password.isEmpty()) return true;
  if (web_server.authenticate(config.web_user.c_str(), config.web_password.c_str())) return true;
  web_server.requestAuthentication(BASIC_AUTH, "UART Logger");
  return false;
}

String main_log_name(uint8_t index) {
  return String(F("/uart-")) + String(index) + F(".log");
}

String panic_log_name(uint8_t index) {
  return String(F("/panic-")) + String(index) + F(".log");
}

String wifi_log_name(uint8_t index) {
  return String(F("/wifi-")) + String(index) + F(".log");
}

String format_mac(const uint8_t *mac) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

const char *wifi_event_name(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_OFF: return "WIFI_OFF";
    case ARDUINO_EVENT_WIFI_READY: return "WIFI_READY";
    case ARDUINO_EVENT_WIFI_STA_START: return "STA_START";
    case ARDUINO_EVENT_WIFI_STA_STOP: return "STA_STOP";
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: return "STA_CONNECTED";
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: return "STA_DISCONNECTED";
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: return "STA_GOT_IP";
    case ARDUINO_EVENT_WIFI_STA_LOST_IP: return "STA_LOST_IP";
    case ARDUINO_EVENT_WIFI_AP_START: return "AP_START";
    case ARDUINO_EVENT_WIFI_AP_STOP: return "AP_STOP";
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: return "AP_CLIENT_CONNECTED";
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: return "AP_CLIENT_DISCONNECTED";
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: return "AP_CLIENT_IP_ASSIGNED";
    default: return "OTHER";
  }
}

size_t file_size(const String &path) {
  if (!filesystem_ready) return 0;
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return 0;
  const size_t size = file.size();
  file.close();
  return size;
}

void append_ram_ring(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    ram_ring[ram_ring_head] = data[i];
    ram_ring_head = (ram_ring_head + 1) % kRamRingSize;
    if (ram_ring_used < kRamRingSize) ++ram_ring_used;
  }
  total_uart_bytes += length;
}

size_t copy_ram_ring(uint8_t *destination, size_t maximum, uint64_t cursor, uint64_t &new_cursor) {
  const uint64_t oldest_cursor = total_uart_bytes - ram_ring_used;
  if (cursor < oldest_cursor || cursor > total_uart_bytes) cursor = oldest_cursor;
  size_t requested = static_cast<size_t>(total_uart_bytes - cursor);
  if (requested > maximum) {
    cursor = total_uart_bytes - maximum;
    requested = maximum;
  }
  const size_t oldest_index = (ram_ring_head + kRamRingSize - ram_ring_used) % kRamRingSize;
  const size_t offset = static_cast<size_t>(cursor - oldest_cursor);
  for (size_t i = 0; i < requested; ++i) {
    destination[i] = ram_ring[(oldest_index + offset + i) % kRamRingSize];
  }
  new_cursor = total_uart_bytes;
  return requested;
}

void write_ring_snapshot(File &file) {
  if (!file || ram_ring_used == 0) return;
  const size_t oldest_index = (ram_ring_head + kRamRingSize - ram_ring_used) % kRamRingSize;
  const size_t first_length = std::min(ram_ring_used, kRamRingSize - oldest_index);
  file.write(ram_ring + oldest_index, first_length);
  if (first_length < ram_ring_used) file.write(ram_ring, ram_ring_used - first_length);
}

void write_session_header(File &file, const char *kind) {
  if (!file) return;
  file.printf("\r\n===== %s | logger=%s | boot=%lu | %s =====\r\n", kind, kVersion,
              static_cast<unsigned long>(boot_count), local_timestamp().c_str());
}

bool open_main_log() {
  if (!filesystem_ready) return false;
  if (main_log) main_log.close();
  main_log = LittleFS.open(main_log_name(active_segment), FILE_APPEND);
  if (!main_log) return false;
  write_session_header(main_log, "UART LOGGER SESSION");
  main_log.flush();
  return true;
}

void rotate_main_log_if_needed() {
  if (!main_log || main_log.size() < kMainSegmentLimit) return;
  main_log.flush();
  main_log.close();
  active_segment = static_cast<uint8_t>((active_segment + 1) % 2);
  const String next_name = main_log_name(active_segment);
  LittleFS.remove(next_name);
  preferences.putUChar("segment", active_segment);
  open_main_log();
}

bool open_wifi_log() {
  if (!filesystem_ready) return false;
  if (wifi_log) wifi_log.close();
  wifi_log = LittleFS.open(wifi_log_name(active_wifi_segment), FILE_APPEND);
  if (!wifi_log) return false;
  write_session_header(wifi_log, "WIFI MONITOR SESSION");
  wifi_log.flush();
  return true;
}

void rotate_wifi_log_if_needed() {
  if (!wifi_log || wifi_log.size() < kWifiSegmentLimit) return;
  wifi_log.flush();
  wifi_log.close();
  active_wifi_segment = static_cast<uint8_t>((active_wifi_segment + 1) % 2);
  const String next_name = wifi_log_name(active_wifi_segment);
  LittleFS.remove(next_name);
  preferences.putUChar("wifi_segment", active_wifi_segment);
  open_wifi_log();
}

void append_wifi_log(const String &event, const String &details = "") {
  if (!wifi_log) return;
  wifi_log.print(local_timestamp());
  wifi_log.print(F(" event="));
  wifi_log.print(event);
  wifi_log.print(F(" uptime_ms="));
  wifi_log.print(millis());
  if (!details.isEmpty()) {
    wifi_log.print(' ');
    wifi_log.print(details);
  }
  wifi_log.print(F("\r\n"));
  rotate_wifi_log_if_needed();
}

String wifi_link_details() {
  String details;
  if (WiFi.status() == WL_CONNECTED) {
    details = F("sta=connected ssid=\"");
    details += json_escape(WiFi.SSID());
    details += F("\" bssid=");
    details += WiFi.BSSIDstr();
    details += F(" channel=");
    details += String(WiFi.channel());
    details += F(" rssi=");
    details += String(WiFi.RSSI());
    details += F(" ip=");
    details += WiFi.localIP().toString();
    details += F(" gateway=");
    details += WiFi.gatewayIP().toString();
  } else {
    details = F("sta=disconnected ssid=\"");
    details += json_escape(config.ssid);
    details += '"';
  }
  details += F(" fallback_ap=");
  details += ap_active ? F("on") : F("off");
  details += F(" ap_clients=");
  details += String(ap_active ? WiFi.softAPgetStationNum() : 0);
  details += F(" heap=");
  details += String(ESP.getFreeHeap());
  return details;
}

void wifi_event_callback(arduino_event_id_t event, arduino_event_info_t info) {
  if (wifi_event_queue == nullptr) return;
  WifiEventRecord record{};
  record.uptime_ms = millis();
  record.event_id = event;
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    record.channel = info.wifi_sta_connected.channel;
    memcpy(record.client_mac, info.wifi_sta_connected.bssid, sizeof(record.client_mac));
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    record.reason = info.wifi_sta_disconnected.reason;
    memcpy(record.client_mac, info.wifi_sta_disconnected.bssid, sizeof(record.client_mac));
  } else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    memcpy(record.client_mac, info.wifi_ap_staconnected.mac, sizeof(record.client_mac));
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    memcpy(record.client_mac, info.wifi_ap_stadisconnected.mac, sizeof(record.client_mac));
  }
  if (xQueueSend(wifi_event_queue, &record, 0) != pdTRUE) ++wifi_event_queue_drops;
}

void service_wifi_monitor(bool force_heartbeat = false) {
  if (wifi_event_queue != nullptr) {
    WifiEventRecord record{};
    while (xQueueReceive(wifi_event_queue, &record, 0) == pdTRUE) {
      String details = String(F("event_uptime_ms=")) + String(record.uptime_ms);
      switch (record.event_id) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
          ++wifi_connected_events;
          last_wifi_connected_ms = record.uptime_ms;
          details += F(" ap_bssid=");
          details += format_mac(record.client_mac);
          details += F(" channel=");
          details += String(record.channel);
          break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
          ++wifi_got_ip_events;
          last_wifi_got_ip_ms = record.uptime_ms;
          details += ' ';
          details += wifi_link_details();
          break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
          ++wifi_disconnected_events;
          last_wifi_disconnected_ms = record.uptime_ms;
          last_wifi_disconnect_reason = record.reason;
          details += F(" ap_bssid=");
          details += format_mac(record.client_mac);
          details += F(" reason=");
          details += String(record.reason);
          details += F(" reason_name=\"");
          details += WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(record.reason));
          details += '"';
          break;
        }
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
          ++wifi_lost_ip_events;
          break;
        case ARDUINO_EVENT_WIFI_AP_START:
          ++fallback_ap_start_events;
          break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
          ++fallback_ap_client_connects;
          details += F(" client=");
          details += format_mac(record.client_mac);
          break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
          ++fallback_ap_client_disconnects;
          details += F(" client=");
          details += format_mac(record.client_mac);
          break;
        default:
          break;
      }
      append_wifi_log(wifi_event_name(record.event_id), details);
    }
  }

  const uint32_t now = millis();
  if (force_heartbeat || last_wifi_heartbeat_ms == 0 || now - last_wifi_heartbeat_ms >= kWifiHeartbeatMs) {
    append_wifi_log("HEARTBEAT", wifi_link_details());
    last_wifi_heartbeat_ms = now;
  }
}

bool line_has_panic_marker(String line) {
  line.toLowerCase();
  static const char *markers[] = {
      "guru meditation error", "backtrace:", "assert failed", "task watchdog got triggered",
      "brownout detector was triggered", "corrupt heap", "stack canary watchpoint triggered",
      "abort() was called", "panic'ed", "panic handler"};
  for (const char *marker : markers) {
    if (line.indexOf(marker) >= 0) return true;
  }
  return false;
}

void close_panic_capture() {
  if (!panic_log) return;
  write_session_header(panic_log, "PANIC CAPTURE END");
  panic_log.flush();
  panic_log.close();
  panic_capture_until = 0;
}

bool start_or_extend_panic_capture() {
  panic_capture_until = millis() + kPanicCaptureMs;
  if (panic_log) return false;
  if (!filesystem_ready) return false;

  const String path = panic_log_name(next_panic_index);
  LittleFS.remove(path);
  panic_log = LittleFS.open(path, FILE_WRITE);
  if (!panic_log) return false;
  ++panic_count;
  preferences.putUInt("panics", panic_count);
  next_panic_index = static_cast<uint8_t>((next_panic_index + 1) % 3);
  preferences.putUChar("panic_idx", next_panic_index);
  write_session_header(panic_log, "PANIC MARKER DETECTED; PREBUFFER FOLLOWS");
  write_ring_snapshot(panic_log);
  panic_log.flush();
  return true;
}

bool process_detector(const uint8_t *data, size_t length) {
  bool capture_started = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n' || detector_line.length() >= 500) {
      if (line_has_panic_marker(detector_line)) {
        capture_started = start_or_extend_panic_capture() || capture_started;
      }
      detector_line = "";
    } else if (c != '\r' && static_cast<uint8_t>(c) >= 0x20) {
      detector_line += c;
    }
  }
  return capture_started;
}

void broadcast_tcp(const uint8_t *data, size_t length) {
  for (auto &client : tcp_clients) {
    if (!client || !client.connected()) {
      if (client) client.stop();
      continue;
    }
    // Never let a slow network reader stall the UART capture path.
    if (client.availableForWrite() < static_cast<int>(length)) {
      ++tcp_drop_count;
      continue;
    }
    const size_t written = client.write(data, length);
    if (written != length) ++tcp_drop_count;
  }
}

void append_uart_data(const uint8_t *data, size_t length) {
  append_ram_ring(data, length);
  if (main_log) main_log.write(data, length);
  const bool capture_started = process_detector(data, length);
  if (panic_log && !capture_started && panic_log.size() < kPanicFileLimit) {
    const size_t remaining = kPanicFileLimit - panic_log.size();
    panic_log.write(data, std::min(length, remaining));
  }
  broadcast_tcp(data, length);
  rotate_main_log_if_needed();
}

void uart_reader_task(void *) {
  uint8_t buffer[512];
  for (;;) {
    const int available = target_uart.available();
    if (available <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    const size_t wanted = std::min(static_cast<size_t>(available), sizeof(buffer));
    const size_t received = target_uart.readBytes(buffer, wanted);
    if (received == 0) continue;
    const size_t queued = xStreamBufferSend(uart_stream, buffer, received, 0);
    if (queued < received) {
      uart_stream_drop_bytes += static_cast<uint32_t>(received - queued);
    }
    taskYIELD();
  }
}

void service_target_uart() {
  uint8_t buffer[768];
  if (uart_stream != nullptr) {
    size_t received = 0;
    while ((received = xStreamBufferReceive(uart_stream, buffer, sizeof(buffer), 0)) > 0) {
      append_uart_data(buffer, received);
    }
  } else {
    while (target_uart.available() > 0) {
      const size_t available = static_cast<size_t>(target_uart.available());
      const size_t wanted = std::min(available, sizeof(buffer));
      const size_t received = target_uart.readBytes(buffer, wanted);
      if (received == 0) break;
      append_uart_data(buffer, received);
    }
  }

  const uint32_t now = millis();
  if (panic_log && ((static_cast<int32_t>(now - panic_capture_until) >= 0) ||
                    panic_log.size() >= kPanicFileLimit)) {
    close_panic_capture();
  }
  if (main_log && now - last_flush_ms >= kLogFlushMs) {
    main_log.flush();
    if (panic_log) panic_log.flush();
    if (wifi_log) wifi_log.flush();
    last_flush_ms = now;
  }
}

void load_config() {
  preferences.begin("uartlogger", false);
  config.ssid = preferences.getString("ssid", "");
  config.wifi_password = preferences.getString("wifi_pass", "");
  config.hostname = preferences.getString("hostname", kDefaultHostname);
  config.web_user = preferences.getString("web_user", "admin");
  config.web_password = preferences.getString("web_pass", "");
  config.baud = preferences.getUInt("baud", kDefaultBaud);
  config.rx_pin = preferences.getInt("rx_pin", kDefaultRxPin);
  config.tcp_port = preferences.getUShort("tcp_port", kDefaultTcpPort);
  active_segment = preferences.getUChar("segment", 0) % 2;
  active_wifi_segment = preferences.getUChar("wifi_segment", 0) % 2;
  next_panic_index = preferences.getUChar("panic_idx", 0) % 3;
  panic_count = preferences.getUInt("panics", 0);
  boot_count = preferences.getUInt("boots", 0) + 1;
  preferences.putUInt("boots", boot_count);

  if (config.hostname.isEmpty()) config.hostname = kDefaultHostname;
  if (config.web_user.isEmpty()) config.web_user = "admin";
  if (config.baud < 300 || config.baud > 2000000) config.baud = kDefaultBaud;
  if (config.rx_pin < 0 || config.rx_pin > 21) config.rx_pin = kDefaultRxPin;
  if (config.tcp_port == 0) config.tcp_port = kDefaultTcpPort;
}

void start_wifi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setHostname(config.hostname.c_str());

  if (!config.ssid.isEmpty()) {
    append_wifi_log("STA_CONNECT_ATTEMPT",
                    String(F("ssid=\"")) + json_escape(config.ssid) + F("\" timeout_ms=") + String(kWifiConnectMs));
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid.c_str(), config.wifi_password.c_str());
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < kWifiConnectMs) {
      service_target_uart();
      service_wifi_monitor();
      delay(20);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    append_wifi_log("STA_INITIAL_TIMEOUT",
                    String(F("ssid=\"")) + json_escape(config.ssid) + F("\" after_ms=") + String(kWifiConnectMs));
    WiFi.mode(config.ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
    const uint64_t chip_id = ESP.getEfuseMac();
    char ap_name[32];
    snprintf(ap_name, sizeof(ap_name), "UART-Logger-%04X", static_cast<unsigned>(chip_id & 0xFFFF));
    ap_active = WiFi.softAP(ap_name, kApPassword);
    append_wifi_log("FALLBACK_AP_RESULT",
                    String(F("started=")) + (ap_active ? F("true") : F("false")) +
                        F(" ssid=\"") + ap_name + F("\" ip=") + WiFi.softAPIP().toString());
  }

  configTzTime("CST-8", "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  mdns_ready = MDNS.begin(config.hostname.c_str());
  if (mdns_ready) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("uart", "tcp", config.tcp_port);
  }
}

String network_address() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (ap_active) return WiFi.softAPIP().toString();
  return String(F("offline"));
}

void accept_tcp_clients() {
  if (tcp_server == nullptr) return;
  WiFiClient incoming = tcp_server->accept();
  if (incoming) {
    bool assigned = false;
    for (auto &client : tcp_clients) {
      if (!client || !client.connected()) {
        if (client) client.stop();
        client = incoming;
        client.setNoDelay(true);
        client.printf("\r\n===== ESP32-C3 UART Logger %s | RX-only | %lu baud | GPIO%d =====\r\n",
                      kVersion, static_cast<unsigned long>(config.baud), config.rx_pin);
        uint8_t tail[4096];
        uint64_t ignored = 0;
        const uint64_t cursor = total_uart_bytes > sizeof(tail) ? total_uart_bytes - sizeof(tail) : 0;
        const size_t copied = copy_ram_ring(tail, sizeof(tail), cursor, ignored);
        if (copied) client.write(tail, copied);
        assigned = true;
        break;
      }
    }
    if (!assigned) incoming.stop();
  }

  for (auto &client : tcp_clients) {
    if (client && client.connected()) {
      while (client.available()) client.read();  // RX-only: discard all remote input.
    } else if (client) {
      client.stop();
    }
  }
}

void handle_status() {
  if (!require_authentication()) return;
  const size_t fs_total = filesystem_ready ? LittleFS.totalBytes() : 0;
  const size_t fs_used = filesystem_ready ? LittleFS.usedBytes() : 0;
  String json = F("{");
  json += String(F("\"version\":\"")) + String(kVersion) + F("\",");
  json += String(F("\"network\":\"")) + json_escape(network_address()) + F("\",");
  const bool sta_connected = WiFi.status() == WL_CONNECTED;
  const char *mode = sta_connected ? (ap_active ? "Wi-Fi STA + fallback AP" : "Wi-Fi STA")
                                   : (ap_active ? "fallback AP only" : "offline");
  json += String(F("\"mode\":\"")) + mode + F("\",");
  if (WiFi.status() == WL_CONNECTED) {
    const int rssi = WiFi.RSSI();
    const int quality = constrain(2 * (rssi + 100), 0, 100);
    json += String(F("\"wifi rssi\":\"")) + String(rssi) + F(" dBm (") + String(quality) + F("%)\",");
    json += String(F("\"wifi ap\":\"")) + json_escape(WiFi.SSID()) + F(" / ") + WiFi.BSSIDstr() +
            F(" / ch ") + String(WiFi.channel()) + F("\",");
  } else {
    json += F("\"wifi rssi\":\"not connected\",");
    json += String(F("\"wifi ap\":\"")) + json_escape(config.ssid) + F(" (disconnected)\",");
  }
  json += String(F("\"wifi connect events\":\"")) + String(wifi_connected_events) + F("\",");
  json += String(F("\"wifi got-ip events\":\"")) + String(wifi_got_ip_events) + F("\",");
  json += String(F("\"wifi disconnects\":\"")) + String(wifi_disconnected_events) + F("\",");
  json += String(F("\"wifi lost-ip events\":\"")) + String(wifi_lost_ip_events) + F("\",");
  if (last_wifi_disconnected_ms == 0) {
    json += F("\"last wifi disconnect\":\"never this boot\",");
  } else {
    json += String(F("\"last wifi disconnect\":\"")) + String((millis() - last_wifi_disconnected_ms) / 1000) +
            F(" s ago; ") + String(last_wifi_disconnect_reason) + F(" ") +
            json_escape(WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(last_wifi_disconnect_reason))) + F("\",");
  }
  String wifi_link_uptime{F("not connected")};
  if (sta_connected && last_wifi_got_ip_ms != 0) {
    wifi_link_uptime = String((millis() - last_wifi_got_ip_ms) / 1000) + F(" s");
  }
  json += String(F("\"wifi link uptime\":\"")) + wifi_link_uptime + F("\",");
  json += String(F("\"fallback ap\":\"")) + String(ap_active ? "on" : "off") + F("; clients=") +
          String(ap_active ? WiFi.softAPgetStationNum() : 0) + F("; starts=") + String(fallback_ap_start_events) + F("\",");
  json += String(F("\"wifi event queue drops\":\"")) + String(wifi_event_queue_drops) + F("\",");
  json += String(F("\"uart\":\"")) + String(config.baud) + F(" 8N1 RX GPIO") + String(config.rx_pin) + F("\",");
  json += String(F("\"tcp\":\"")) + String(config.tcp_port) + F(" (read-only)\",");
  json += String(F("\"captured bytes\":\"")) + uint64_string(total_uart_bytes) + F("\",");
  json += String(F("\"panic snapshots\":\"")) + String(panic_count) + F("\",");
  json += String(F("\"panic capture\":\"")) + String(panic_log ? "active" : "idle") + F("\",");
  json += String(F("\"uart errors\":\"")) + String(uart_error_count) + F("\",");
  json += String(F("\"uart overflows\":\"")) + String(uart_overflow_count) + F("\",");
  json += String(F("\"last uart error\":\"")) +
          String(last_uart_error_ms == 0 ? "never" : (String((millis() - last_uart_error_ms) / 1000) + F(" s ago"))) + F("\",");
  json += String(F("\"last uart overflow\":\"")) +
          String(last_uart_overflow_ms == 0 ? "never" : (String((millis() - last_uart_overflow_ms) / 1000) + F(" s ago"))) + F("\",");
  json += String(F("\"uart queue drops\":\"")) + String(uart_stream_drop_bytes) + F(" bytes\",");
  json += String(F("\"tcp skipped chunks\":\"")) + String(tcp_drop_count) + F("\",");
  json += String(F("\"logger boots\":\"")) + String(boot_count) + F("\",");
  json += String(F("\"ota status\":\"")) + json_escape(ota_status) + F("\",");
  json += String(F("\"filesystem\":\"")) + String(fs_used) + F(" / ") + String(fs_total) + F(" bytes\",");
  json += String(F("\"wifi logs\":\"")) + String(file_size(wifi_log_name(0)) + file_size(wifi_log_name(1))) + F(" bytes\",");
  json += String(F("\"heap\":\"")) + String(ESP.getFreeHeap()) + F(" bytes\",");
  json += String(F("\"uptime\":\"")) + String(millis() / 1000) + F(" s\"");
  json += F("}");
  web_server.send(200, "application/json", json);
}

void handle_tail() {
  if (!require_authentication()) return;
  uint64_t cursor = 0;
  if (web_server.hasArg("cursor")) cursor = strtoull(web_server.arg("cursor").c_str(), nullptr, 10);
  static uint8_t output[8192];
  uint64_t new_cursor = total_uart_bytes;
  const size_t copied = copy_ram_ring(output, sizeof(output), cursor, new_cursor);
  String body;
  body.reserve(copied);
  for (size_t i = 0; i < copied; ++i) {
    const uint8_t c = output[i];
    if (c == '\r' || c == '\n' || c == '\t' || c >= 0x20) body += static_cast<char>(c);
  }
  web_server.sendHeader("X-Uart-Cursor", uint64_string(new_cursor));
  web_server.sendHeader("Cache-Control", "no-store");
  web_server.send(200, "text/plain; charset=utf-8", body);
}

String files_page() {
  String html = F("<!doctype html><html lang='zh-Hant'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>UART Logs</title><body><h1>UART 日誌檔</h1><ul>");
  for (uint8_t i = 0; i < 2; ++i) {
    const String path = main_log_name(i);
    html += String(F("<li><a href='/download?file=")) + path + F("'>") + path.substring(1) + F("</a> (") + String(file_size(path)) + F(" bytes)</li>");
  }
  for (uint8_t i = 0; i < 3; ++i) {
    const String path = panic_log_name(i);
    html += String(F("<li><a href='/download?file=")) + path + F("'>") + path.substring(1) + F("</a> (") + String(file_size(path)) + F(" bytes)</li>");
  }
  for (uint8_t i = 0; i < 2; ++i) {
    const String path = wifi_log_name(i);
    html += String(F("<li><a href='/download?file=")) + path + F("'>") + path.substring(1) + F("</a> (") + String(file_size(path)) + F(" bytes)</li>");
  }
  html += F("</ul><form method='post' action='/clear' onsubmit=\"return confirm('確定清除所有日誌？')\"><button>清除所有日誌</button></form><p><a href='/'>返回</a></p></body></html>");
  return html;
}

void handle_files() {
  if (!require_authentication()) return;
  if (main_log) main_log.flush();
  if (panic_log) panic_log.flush();
  if (wifi_log) wifi_log.flush();
  web_server.send(200, "text/html; charset=utf-8", files_page());
}

bool is_allowed_log_path(const String &path) {
  for (uint8_t i = 0; i < 2; ++i) if (path == main_log_name(i)) return true;
  for (uint8_t i = 0; i < 3; ++i) if (path == panic_log_name(i)) return true;
  for (uint8_t i = 0; i < 2; ++i) if (path == wifi_log_name(i)) return true;
  return false;
}

void handle_download() {
  if (!require_authentication()) return;
  const String path = web_server.arg("file");
  if (!is_allowed_log_path(path) || !LittleFS.exists(path)) {
    web_server.send(404, "text/plain", "Not found");
    return;
  }
  if (main_log) main_log.flush();
  if (panic_log) panic_log.flush();
  if (wifi_log) wifi_log.flush();
  File file = LittleFS.open(path, FILE_READ);
  web_server.sendHeader("Content-Disposition", String(F("attachment; filename=\"")) + path.substring(1) + F("\""));
  web_server.streamFile(file, "application/octet-stream");
  file.close();
}

void clear_all_logs() {
  if (main_log) main_log.close();
  if (panic_log) panic_log.close();
  if (wifi_log) wifi_log.close();
  for (uint8_t i = 0; i < 2; ++i) LittleFS.remove(main_log_name(i));
  for (uint8_t i = 0; i < 3; ++i) LittleFS.remove(panic_log_name(i));
  for (uint8_t i = 0; i < 2; ++i) LittleFS.remove(wifi_log_name(i));
  active_segment = 0;
  active_wifi_segment = 0;
  next_panic_index = 0;
  panic_capture_until = 0;
  panic_count = 0;
  preferences.putUChar("segment", active_segment);
  preferences.putUChar("wifi_segment", active_wifi_segment);
  preferences.putUChar("panic_idx", next_panic_index);
  preferences.putUInt("panics", panic_count);
  open_main_log();
  open_wifi_log();
  append_wifi_log("LOGS_CLEARED");
}

void handle_clear() {
  if (!require_authentication()) return;
  clear_all_logs();
  web_server.sendHeader("Location", "/files");
  web_server.send(303, "text/plain", "Logs cleared");
}

String config_page() {
  String html = F("<!doctype html><html lang='zh-Hant'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>UART Logger 設定</title><style>body{font-family:system-ui;max-width:720px;margin:24px auto;padding:0 14px}label{display:block;margin-top:12px}input{width:100%;padding:8px;box-sizing:border-box}button{margin-top:18px;padding:10px}</style><body><h1>UART Logger 設定</h1><form method='post'>");
  html += String(F("<label>Wi-Fi SSID<input name='ssid' value='")) + html_escape(config.ssid) + F("'></label>");
  html += F("<label>Wi-Fi 密碼（留空表示維持原值）<input name='wifi_password' type='password'></label><label><input style='width:auto' type='checkbox' name='clear_wifi_password' value='1'> 清除 Wi-Fi 密碼</label>");
  html += String(F("<label>Hostname<input name='hostname' value='")) + html_escape(config.hostname) + F("'></label>");
  html += String(F("<label>UART Baud<input name='baud' type='number' min='300' max='2000000' value='")) + String(config.baud) + F("'></label>");
  html += String(F("<label>UART RX GPIO<input name='rx_pin' type='number' min='0' max='21' value='")) + String(config.rx_pin) + F("'></label>");
  html += String(F("<label>唯讀 TCP Port<input name='tcp_port' type='number' min='1' max='65535' value='")) + String(config.tcp_port) + F("'></label>");
  html += String(F("<label>Web 使用者<input name='web_user' value='")) + html_escape(config.web_user) + F("'></label>");
  html += F("<label>Web 密碼（留空表示維持原值）<input name='web_password' type='password'></label><label><input style='width:auto' type='checkbox' name='clear_web_password' value='1'> 清除 Web 密碼</label>");
  html += F("<button>儲存並重新啟動</button></form><hr><h2>Logger 韌體更新</h2><form method='post' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required><button>上傳並更新</button></form><p>Fallback AP：<code>UART-Logger-xxxx</code>，密碼 <code>uartlogger</code>。</p><p><a href='/'>返回</a></p></body></html>");
  return html;
}

void handle_config_get() {
  if (!require_authentication()) return;
  web_server.send(200, "text/html; charset=utf-8", config_page());
}

void handle_config_post() {
  if (!require_authentication()) return;
  String hostname = web_server.arg("hostname");
  hostname.trim();
  hostname.toLowerCase();
  if (hostname.isEmpty()) hostname = kDefaultHostname;
  hostname.replace(" ", "-");

  uint32_t baud = strtoul(web_server.arg("baud").c_str(), nullptr, 10);
  int rx_pin = web_server.arg("rx_pin").toInt();
  uint32_t tcp_port = strtoul(web_server.arg("tcp_port").c_str(), nullptr, 10);
  if (baud < 300 || baud > 2000000 || rx_pin < 0 || rx_pin > 21 || tcp_port == 0 || tcp_port > 65535) {
    web_server.send(400, "text/plain", "Invalid UART or TCP settings");
    return;
  }

  preferences.putString("ssid", web_server.arg("ssid"));
  if (web_server.hasArg("clear_wifi_password")) preferences.putString("wifi_pass", "");
  else if (!web_server.arg("wifi_password").isEmpty()) preferences.putString("wifi_pass", web_server.arg("wifi_password"));
  preferences.putString("hostname", hostname);
  preferences.putUInt("baud", baud);
  preferences.putInt("rx_pin", rx_pin);
  preferences.putUShort("tcp_port", static_cast<uint16_t>(tcp_port));
  String web_user = web_server.arg("web_user");
  if (web_user.isEmpty()) web_user = "admin";
  preferences.putString("web_user", web_user);
  if (web_server.hasArg("clear_web_password")) preferences.putString("web_pass", "");
  else if (!web_server.arg("web_password").isEmpty()) preferences.putString("web_pass", web_server.arg("web_password"));

  web_server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'><p>設定已儲存，Logger 即將重新啟動。</p>");
  delay(300);
  ESP.restart();
}

void handle_update_finish() {
  if (!require_authentication()) return;
  const bool success = ota_upload_started && ota_upload_success && !Update.hasError();
  if (success) {
    ota_status = String(F("complete; ")) + String(ota_received_bytes) + F(" bytes; reboot pending");
    web_server.sendHeader("Connection", "close");
    web_server.send(200, "text/plain; charset=utf-8", "Update complete; rebooting in 2 seconds");
    ota_reboot_at = millis() + 2000;
  } else {
    ota_status = String(F("failed: ")) + Update.errorString();
    web_server.sendHeader("Connection", "close");
    web_server.send(500, "text/plain; charset=utf-8", ota_status);
  }
}

void handle_update_upload() {
  if (!config.web_password.isEmpty() && !web_server.authenticate(config.web_user.c_str(), config.web_password.c_str())) return;
  HTTPUpload &upload = web_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (main_log) main_log.flush();
    if (panic_log) panic_log.flush();
    if (wifi_log) wifi_log.flush();
    ota_upload_started = true;
    ota_received_bytes = 0;
    ota_upload_success = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    ota_status = ota_upload_success ? "receiving" : (String(F("begin failed: ")) + Update.errorString());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (ota_upload_success) {
      const size_t written = Update.write(upload.buf, upload.currentSize);
      ota_received_bytes += written;
      if (written != upload.currentSize) {
        ota_upload_success = false;
        ota_status = String(F("write failed after ")) + String(ota_received_bytes) + F(" bytes: ") + Update.errorString();
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (ota_upload_success) {
      ota_upload_success = Update.end(true);
      ota_status = ota_upload_success ? "ready to reboot" : (String(F("end failed: ")) + Update.errorString());
    } else {
      Update.abort();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    ota_upload_success = false;
    ota_status = String(F("aborted after ")) + String(ota_received_bytes) + F(" bytes");
  }
}

void start_web_server() {
  web_server.on("/", HTTP_GET, []() {
    if (!require_authentication()) return;
    web_server.send_P(200, "text/html; charset=utf-8", kRootHtml);
  });
  web_server.on("/api/status", HTTP_GET, handle_status);
  web_server.on("/api/tail", HTTP_GET, handle_tail);
  web_server.on("/files", HTTP_GET, handle_files);
  web_server.on("/download", HTTP_GET, handle_download);
  web_server.on("/clear", HTTP_POST, handle_clear);
  web_server.on("/config", HTTP_GET, handle_config_get);
  web_server.on("/config", HTTP_POST, handle_config_post);
  web_server.on("/update", HTTP_POST, handle_update_finish, handle_update_upload);
  web_server.on("/reboot", HTTP_POST, []() {
    if (!require_authentication()) return;
    web_server.send(200, "text/plain", "Rebooting");
    delay(200);
    ESP.restart();
  });
  web_server.onNotFound([]() { web_server.send(404, "text/plain", "Not found"); });
  web_server.begin();
}

void setup_logger() {
  load_config();
  setenv("TZ", "CST-8", 1);
  tzset();
  filesystem_ready = LittleFS.begin(true);
  if (filesystem_ready) {
    open_main_log();
    open_wifi_log();
    append_wifi_log("LOGGER_BOOT", String(F("boot=")) + String(boot_count) + F(" version=") + kVersion);
  }

  // ESP32-C3 UART1 is routed through the GPIO matrix. TX is intentionally disabled.
  target_uart.setRxBufferSize(kHardwareRxBufferSize);
  target_uart.onReceiveError([](hardwareSerial_error_t error) {
    ++uart_error_count;
    last_uart_error_ms = millis();
    if (error == UART_BUFFER_FULL_ERROR || error == UART_FIFO_OVF_ERROR) {
      ++uart_overflow_count;
      last_uart_overflow_ms = last_uart_error_ms;
    }
  });
  target_uart.begin(config.baud, SERIAL_8N1, config.rx_pin, -1);
  target_uart.setTimeout(2);

  uart_stream = xStreamBufferCreate(kUartStreamSize, 1);
  if (uart_stream != nullptr) {
    if (xTaskCreate(uart_reader_task, "uart_reader", 3072, nullptr, 3,
                    &uart_reader_task_handle) != pdPASS) {
      vStreamBufferDelete(uart_stream);
      uart_stream = nullptr;
      uart_reader_task_handle = nullptr;
    }
  }

  wifi_event_queue = xQueueCreate(kWifiEventQueueLength, sizeof(WifiEventRecord));
  WiFi.onEvent(wifi_event_callback);
  start_wifi();
  service_wifi_monitor(true);
  tcp_server = new WiFiServer(config.tcp_port);
  tcp_server->begin();
  tcp_server->setNoDelay(true);
  start_web_server();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  setup_logger();
  Serial.printf("UART Logger %s ready: RX GPIO%d at %lu baud, web http://%s/, TCP %u\n",
                kVersion, config.rx_pin, static_cast<unsigned long>(config.baud),
                network_address().c_str(), config.tcp_port);
}

void loop() {
  service_target_uart();
  service_wifi_monitor();
  accept_tcp_clients();
  web_server.handleClient();
  if (ota_reboot_at != 0 && static_cast<int32_t>(millis() - ota_reboot_at) >= 0) {
    ESP.restart();
  }
  delay(1);
}
