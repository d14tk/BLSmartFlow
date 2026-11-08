#include "blflow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace blflow {

static const char *const TAG = "blflow";
static const char *const LEGACY_LABEL = "spiffs";
static const char *const LEGACY_FILE = "/legacy/blledconfig.json";
static const uint32_t STALE_MS = 60000;
static const size_t MAX_REPORT = 65536;

// ---------------------------------------------------------------- pure helpers

float BLFlow::interpolate(const Settings &s, float temp) {
  if (s.num_points == 0)
    return 0;
  if (temp <= s.temps[0])
    return s.speeds[0];
  for (uint8_t i = 1; i < s.num_points; i++) {
    if (temp <= s.temps[i]) {
      float t1 = s.temps[i - 1], t2 = s.temps[i];
      float s1 = s.speeds[i - 1], s2 = s.speeds[i];
      if (t2 <= t1)
        return s2;
      return s1 + (temp - t1) * (s2 - s1) / (t2 - t1);
    }
  }
  return s.speeds[s.num_points - 1];
}

// Format: "temp:speed,temp:speed,..." (2..MAX_POINTS points)
bool BLFlow::parse_points(const std::string &in, Settings &s) {
  struct P {
    float t;
    int sp;
  };
  std::vector<P> pts;
  size_t pos = 0;
  while (pos < in.size()) {
    size_t end = in.find(',', pos);
    if (end == std::string::npos)
      end = in.size();
    std::string item = in.substr(pos, end - pos);
    float t;
    int sp;
    if (sscanf(item.c_str(), "%f:%d", &t, &sp) != 2)
      return false;
    if (!std::isfinite(t) || t < 0 || t > 500 || sp < 0 || sp > 100)
      return false;
    pts.push_back({t, sp});
    pos = end + 1;
  }
  if (pts.size() < 2 || pts.size() > MAX_POINTS)
    return false;
  std::stable_sort(pts.begin(), pts.end(), [](const P &a, const P &b) { return a.t < b.t; });
  s.num_points = pts.size();
  for (size_t i = 0; i < pts.size(); i++) {
    s.temps[i] = pts[i].t;
    s.speeds[i] = pts[i].sp;
  }
  return true;
}

static void copy_str(char *dst, size_t cap, const char *src) {
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static bool valid_ip(const std::string &v) {
  if (v.empty() || v.size() > 15)
    return false;
  return std::all_of(v.begin(), v.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

static bool valid_token(const std::string &v, size_t max) {
  if (v.empty() || v.size() > max)
    return false;
  return std::all_of(v.begin(), v.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)); });
}

static std::string json_escape(const char *s) {
  std::string out;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') {
      out += '\\';
      out += *s;
    } else if (static_cast<unsigned char>(*s) >= 0x20) {
      out += *s;
    }
  }
  return out;
}

// ---------------------------------------------------------------- lifecycle

void BLFlow::load_defaults_() {
  memset(&this->settings_, 0, sizeof(this->settings_));
  this->settings_.magic = SETTINGS_MAGIC;
  const float t[] = {0, 50, 180, 250, 280, 400};
  const uint8_t s[] = {0, 0, 100, 60, 30, 20};
  this->settings_.num_points = 6;
  for (int i = 0; i < 6; i++) {
    this->settings_.temps[i] = t[i];
    this->settings_.speeds[i] = s[i];
  }
}

bool BLFlow::migrate_legacy_config_() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/legacy";
  conf.partition_label = LEGACY_LABEL;
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;
  if (esp_vfs_littlefs_register(&conf) != ESP_OK) {
    ESP_LOGW(TAG, "Legacy LittleFS not mountable, skipping migration");
    return false;
  }

  bool ok = false;
  FILE *f = fopen(LEGACY_FILE, "r");
  if (f == nullptr) {
    ESP_LOGW(TAG, "Legacy config file not found");
  } else {
    std::string raw;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && raw.size() < 8192)
      raw.append(buf, n);
    fclose(f);

    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) {
      ESP_LOGW(TAG, "Legacy config is not valid JSON");
    } else {
      copy_str(this->settings_.printer_ip, sizeof(this->settings_.printer_ip), doc["printerIp"] | "");
      copy_str(this->settings_.access_code, sizeof(this->settings_.access_code), doc["accessCode"] | "");
      copy_str(this->settings_.serial, sizeof(this->settings_.serial), doc["serialNumber"] | "");

      JsonArray pts = doc["fanPoints"].as<JsonArray>();
      if (pts.size() >= 2 && pts.size() <= MAX_POINTS) {
        std::string spec;
        for (JsonObject p : pts) {
          char item[32];
          snprintf(item, sizeof(item), "%s%.1f:%d", spec.empty() ? "" : ",", p["temp"] | 0.0f, p["speed"] | 0);
          spec += item;
        }
        Settings tmp = this->settings_;
        if (parse_points(spec, tmp))
          this->settings_ = tmp;
      }

      const char *ssid = doc["ssid"] | "";
      const char *pass = doc["appw"] | "";
      if (ssid[0] != '\0') {
        ESP_LOGI(TAG, "Migrating WiFi network '%s' from legacy firmware", ssid);
        wifi::global_wifi_component->save_wifi_sta(ssid, pass);
      }
      ESP_LOGI(TAG, "Migrated legacy config: printer %s, serial %s, %u curve points",
               this->settings_.printer_ip, this->settings_.serial, this->settings_.num_points);
      ok = true;
    }
  }
  esp_vfs_littlefs_unregister(LEGACY_LABEL);
  return ok;
}

void BLFlow::save_settings_() {
  Settings copy;
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    copy = this->settings_;
  }
  this->pref_.save(&copy);
  global_preferences->sync();
}

void BLFlow::setup() {
  this->pref_ = global_preferences->make_preference<Settings>(fnv1_hash("blflow_settings_v2"), true);
  if (!this->pref_.load(&this->settings_) || this->settings_.magic != SETTINGS_MAGIC) {
    ESP_LOGI(TAG, "No stored settings, loading defaults and trying legacy migration");
    this->load_defaults_();
    this->migrate_legacy_config_();
    this->save_settings_();
  }

  for (auto *out : this->fan_outputs_)
    out->set_level(0.0f);

  web_server_base::global_web_server_base->init();
  web_server_base::global_web_server_base->add_handler(this);

  this->start_mqtt_();
}

void BLFlow::dump_config() {
  ESP_LOGCONFIG(TAG, "BLFlow %s", this->firmware_version_);
  ESP_LOGCONFIG(TAG, "  Printer IP: %s", this->settings_.printer_ip);
  ESP_LOGCONFIG(TAG, "  Serial: %s", this->settings_.serial);
  ESP_LOGCONFIG(TAG, "  Access code set: %s", YESNO(this->settings_.access_code[0] != '\0'));
  ESP_LOGCONFIG(TAG, "  Curve points: %u", this->settings_.num_points);
  ESP_LOGCONFIG(TAG, "  Fan outputs: %u", (unsigned) this->fan_outputs_.size());
}

void BLFlow::loop() {
  const uint32_t now = millis();

  if (this->save_pending_.exchange(false))
    this->save_settings_();
  if (this->restart_mqtt_.exchange(false)) {
    this->stop_mqtt_();
    this->start_mqtt_();
  }
  if (this->reboot_pending_.load()) {
    App.safe_reboot();
    return;
  }

  if (now - this->last_control_ms_ < 1000)
    return;
  this->last_control_ms_ = now;

  const uint32_t last = this->last_report_ms_.load();
  const bool online = this->mqtt_connected_.load() && last != 0 && (now - last) < STALE_MS;
  const float temp = online ? this->nozzle_temp_.load() : 0.0f;

  float speed;
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    speed = this->settings_.static_fan ? this->settings_.static_speed : interpolate(this->settings_, temp);
  }
  speed = clamp(speed, 0.0f, 100.0f);
  const uint8_t speed_i = static_cast<uint8_t>(lroundf(speed));
  for (auto *out : this->fan_outputs_)
    out->set_level(speed / 100.0f);

  const bool changed = speed_i != this->fan_speed_.load() || online != this->printer_online_;
  this->fan_speed_.store(speed_i);
  this->printer_online_ = online;

  if (online)
    this->status_clear_warning();
  else
    this->status_set_warning();

  if (changed || now - this->last_publish_ms_ >= 10000) {
    this->last_publish_ms_ = now;
    if (this->nozzle_sensor_ != nullptr)
      this->nozzle_sensor_->publish_state(online ? temp : NAN);
    if (this->fan_sensor_ != nullptr)
      this->fan_sensor_->publish_state(speed_i);
    if (this->connected_sensor_ != nullptr)
      this->connected_sensor_->publish_state(online);
  }
}

// ---------------------------------------------------------------- MQTT

void BLFlow::start_mqtt_() {
  Settings s;
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    s = this->settings_;
  }
  if (s.printer_ip[0] == '\0' || s.access_code[0] == '\0' || s.serial[0] == '\0') {
    ESP_LOGW(TAG, "Printer not configured, MQTT not started");
    return;
  }

  char uri[40];
  snprintf(uri, sizeof(uri), "mqtts://%s:8883", s.printer_ip);
  char client_id[24];
  snprintf(client_id, sizeof(client_id), "blflow-%08x", (unsigned) random_uint32());

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri;
  cfg.broker.verification.skip_cert_common_name_check = true;
  cfg.credentials.username = "bblp";
  cfg.credentials.client_id = client_id;
  cfg.credentials.authentication.password = s.access_code;
  cfg.session.keepalive = 30;
  cfg.network.reconnect_timeout_ms = 5000;
  cfg.network.timeout_ms = 20000;
  cfg.buffer.size = 8192;
  cfg.buffer.out_size = 1024;
  cfg.task.stack_size = 8192;

  this->mqtt_ = esp_mqtt_client_init(&cfg);
  if (this->mqtt_ == nullptr) {
    ESP_LOGE(TAG, "esp_mqtt_client_init failed");
    return;
  }
  esp_mqtt_client_register_event(this->mqtt_, MQTT_EVENT_ANY, &BLFlow::mqtt_event_handler_, this);
  esp_mqtt_client_start(this->mqtt_);
  ESP_LOGI(TAG, "MQTT started for %s", uri);
}

void BLFlow::stop_mqtt_() {
  if (this->mqtt_ == nullptr)
    return;
  esp_mqtt_client_stop(this->mqtt_);
  esp_mqtt_client_destroy(this->mqtt_);
  this->mqtt_ = nullptr;
  this->mqtt_connected_.store(false);
  this->last_report_ms_.store(0);
}

void BLFlow::mqtt_event_handler_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
  auto *self = static_cast<BLFlow *>(arg);
  auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED: {
      self->mqtt_connected_.store(true);
      char serial[sizeof(Settings::serial)];
      {
        std::lock_guard<std::mutex> lock(self->settings_mutex_);
        copy_str(serial, sizeof(serial), self->settings_.serial);
      }
      char topic[64];
      snprintf(topic, sizeof(topic), "device/%s/report", serial);
      esp_mqtt_client_subscribe(event->client, topic, 0);
      snprintf(topic, sizeof(topic), "device/%s/request", serial);
      const char *pushall = R"({"pushing":{"sequence_id":"0","command":"pushall"}})";
      esp_mqtt_client_publish(event->client, topic, pushall, 0, 0, 0);
      ESP_LOGI(TAG, "MQTT connected to printer");
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      self->mqtt_connected_.store(false);
      ESP_LOGW(TAG, "MQTT disconnected");
      break;
    case MQTT_EVENT_ERROR:
      if (event->error_handle != nullptr && event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        ESP_LOGW(TAG, "MQTT connection refused (code %d), check access code",
                 (int) event->error_handle->connect_return_code);
      break;
    case MQTT_EVENT_DATA:
      self->on_mqtt_data_(event);
      break;
    default:
      break;
  }
}

void BLFlow::on_mqtt_data_(esp_mqtt_event_handle_t event) {
  if (event->total_data_len <= 0 || static_cast<size_t>(event->total_data_len) > MAX_REPORT) {
    if (event->current_data_offset == 0)
      this->dropped_count_++;
    return;
  }
  if (event->current_data_offset == 0) {
    this->rx_buffer_.clear();
    this->rx_buffer_.reserve(event->total_data_len);
  }
  if (static_cast<size_t>(event->current_data_offset) != this->rx_buffer_.size()) {
    // Chunk out of sequence: discard this message
    this->rx_buffer_.clear();
    return;
  }
  this->rx_buffer_.append(event->data, event->data_len);
  if (this->rx_buffer_.size() >= static_cast<size_t>(event->total_data_len)) {
    this->parse_report_(this->rx_buffer_.data(), this->rx_buffer_.size());
    this->rx_buffer_.clear();
    this->rx_buffer_.shrink_to_fit();
  }
}

void BLFlow::parse_report_(const char *data, size_t len) {
  JsonDocument filter;
  filter["print"]["nozzle_temper"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len, DeserializationOption::Filter(filter));
  if (err) {
    this->dropped_count_++;
    return;
  }
  JsonVariant t = doc["print"]["nozzle_temper"];
  if (t.is<float>())
    this->nozzle_temp_.store(t.as<float>());
  // Any well-formed report proves the link is alive, even without a temperature field
  this->last_report_ms_.store(millis());
  this->report_count_++;
}

// ---------------------------------------------------------------- web

bool BLFlow::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  if (request->method() == HTTP_GET) {
    if (url == "/")
      return !wifi::global_wifi_component->is_ap_active();
    return url == "/sensorData" || url == "/getFanConfig" || url == "/getOptions" || url == "/api/info";
  }
  if (request->method() == HTTP_POST)
    return url == "/updateFanConfig" || url == "/submitOptions" || url == "/api/restart";
  return false;
}

void BLFlow::handleRequest(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  if (url == "/")
    this->send_index_(request);
  else if (url == "/sensorData")
    this->send_sensor_data_(request);
  else if (url == "/getFanConfig")
    this->send_fan_config_(request);
  else if (url == "/getOptions")
    this->send_options_(request);
  else if (url == "/api/info")
    this->send_info_(request);
  else if (url == "/updateFanConfig")
    this->handle_update_fan_config_(request);
  else if (url == "/submitOptions")
    this->handle_submit_options_(request);
  else if (url == "/api/restart") {
    request->send(200, "text/plain", "restarting");
    this->reboot_pending_.store(true);
  } else
    request->send(404, "text/plain", "Not found");
}

void BLFlow::send_index_(AsyncWebServerRequest *request) {
  auto *res = request->beginResponse(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_SIZE);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

void BLFlow::send_sensor_data_(AsyncWebServerRequest *request) {
  const uint32_t now = millis();
  const uint32_t last = this->last_report_ms_.load();
  const bool connected = this->mqtt_connected_.load();
  const bool online = connected && last != 0 && (now - last) < STALE_MS;
  char out[200];
  snprintf(out, sizeof(out),
           R"({"temp":%.1f,"speed":%u,"mqtt":%s,"online":%s,"age":%ld,"reports":%u,"dropped":%u})",
           online ? this->nozzle_temp_.load() : 0.0f, (unsigned) this->fan_speed_.load(), connected ? "true" : "false",
           online ? "true" : "false", last == 0 ? -1L : (long) ((now - last) / 1000),
           (unsigned) this->report_count_.load(), (unsigned) this->dropped_count_.load());
  request->send(200, "application/json", out);
}

void BLFlow::send_fan_config_(AsyncWebServerRequest *request) {
  std::string out = R"({"points":[)";
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    for (uint8_t i = 0; i < this->settings_.num_points; i++) {
      char item[48];
      snprintf(item, sizeof(item), R"(%s{"temp":%g,"speed":%u})", i ? "," : "", this->settings_.temps[i],
               this->settings_.speeds[i]);
      out += item;
    }
  }
  out += "]}";
  request->send(200, "application/json", out.c_str());
}

void BLFlow::send_options_(AsyncWebServerRequest *request) {
  Settings s;
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    s = this->settings_;
  }
  std::string code_masked;
  size_t code_len = strlen(s.access_code);
  if (code_len > 0)
    code_masked = std::string(code_len > 3 ? code_len - 3 : 0, '*') + (s.access_code + (code_len > 3 ? code_len - 3 : 0));
  std::string out = R"({"firmwareversion":")" + json_escape(this->firmware_version_) + R"(","ip":")" +
                    json_escape(s.printer_ip) + R"(","code":")" + json_escape(code_masked.c_str()) +
                    R"(","id":")" + json_escape(s.serial) + R"(","staticfans":)" + (s.static_fan ? "true" : "false") +
                    R"(,"staticfanspeed":)" + std::to_string(s.static_speed) + "}";
  request->send(200, "application/json", out.c_str());
}

void BLFlow::send_info_(AsyncWebServerRequest *request) {
  auto *wifi = wifi::global_wifi_component;
  char ip[network::IP_ADDRESS_BUFFER_SIZE] = "";
  for (auto &addr : wifi->wifi_sta_ip_addresses()) {
    if (addr.is_ip4()) {
      addr.str_to(ip);
      break;
    }
  }
  char ssid_buf[wifi::SSID_BUFFER_SIZE];
  const char *ssid = wifi->wifi_ssid_to(ssid_buf);
  char out[320];
  snprintf(out, sizeof(out),
           R"({"fwversion":"%s","ssid":"%s","rssi":%d,"ip":"%s","uptime":%u,"heap":%u,"mqtt":%s,"reports":%u,"dropped":%u})",
           json_escape(this->firmware_version_).c_str(), json_escape(ssid).c_str(),
           (int) wifi->wifi_rssi(), ip, (unsigned) (millis() / 1000),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL), this->mqtt_connected_.load() ? "true" : "false",
           (unsigned) this->report_count_.load(), (unsigned) this->dropped_count_.load());
  request->send(200, "application/json", out);
}

void BLFlow::handle_update_fan_config_(AsyncWebServerRequest *request) {
  if (!request->hasArg("p")) {
    request->send(400, "text/plain", "missing p");
    return;
  }
  bool ok;
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    Settings tmp = this->settings_;
    ok = parse_points(request->arg("p"), tmp);
    if (ok)
      this->settings_ = tmp;
  }
  if (!ok) {
    request->send(400, "text/plain", "invalid points");
    return;
  }
  this->save_pending_.store(true);
  request->send(200, "text/plain", "OK");
}

void BLFlow::handle_submit_options_(AsyncWebServerRequest *request) {
  std::string ip = request->arg("ip");
  std::string code = request->arg("code");
  std::string serial = request->arg("serial");
  std::string staticfan = request->arg("staticfan");
  std::string staticspeed = request->arg("staticfanspeed");

  const bool keep_code = code.empty() || code.find('*') != std::string::npos;
  if (!valid_ip(ip) || !valid_token(serial, sizeof(Settings::serial) - 1) ||
      (!keep_code && !valid_token(code, sizeof(Settings::access_code) - 1))) {
    request->send(400, "text/plain", "invalid printer settings");
    return;
  }
  int speed = atoi(staticspeed.c_str());
  speed = std::max(0, std::min(100, speed));

  bool printer_changed;
  {
    std::lock_guard<std::mutex> lock(this->settings_mutex_);
    Settings &s = this->settings_;
    printer_changed = ip != s.printer_ip || serial != s.serial || (!keep_code && code != s.access_code);
    copy_str(s.printer_ip, sizeof(s.printer_ip), ip.c_str());
    copy_str(s.serial, sizeof(s.serial), serial.c_str());
    if (!keep_code)
      copy_str(s.access_code, sizeof(s.access_code), code.c_str());
    s.static_fan = staticfan == "on" || staticfan == "true" || staticfan == "1";
    s.static_speed = speed;
  }
  this->save_pending_.store(true);
  if (printer_changed)
    this->restart_mqtt_.store(true);
  request->send(200, "text/plain", "OK");
}

}  // namespace blflow
}  // namespace esphome
