#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include "mqtt_client.h"

namespace esphome {
namespace blflow {

extern const uint8_t INDEX_HTML_GZ[];
extern const size_t INDEX_HTML_GZ_SIZE;

static const uint8_t MAX_POINTS = 12;
static const uint32_t SETTINGS_MAGIC = 0xB1F10002;

struct Settings {
  uint32_t magic;
  char printer_ip[16];
  char access_code[16];
  char serial[24];
  bool static_fan;
  uint8_t static_speed;
  uint8_t num_points;
  float temps[MAX_POINTS];
  uint8_t speeds[MAX_POINTS];
} __attribute__((packed));

class BLFlow : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void add_fan_output(output::FloatOutput *out) { this->fan_outputs_.push_back(out); }
  void set_firmware_version(const char *v) { this->firmware_version_ = v; }
  void set_nozzle_sensor(sensor::Sensor *s) { this->nozzle_sensor_ = s; }
  void set_fan_sensor(sensor::Sensor *s) { this->fan_sensor_ = s; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { this->connected_sensor_ = s; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  static float interpolate(const Settings &s, float temp);
  static bool parse_points(const std::string &in, Settings &s);

 protected:
  void load_defaults_();
  bool migrate_legacy_config_();
  void save_settings_();
  void start_mqtt_();
  void stop_mqtt_();
  static void mqtt_event_handler_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
  void on_mqtt_data_(esp_mqtt_event_handle_t event);
  void parse_report_(const char *data, size_t len);

  void send_index_(AsyncWebServerRequest *request);
  void send_sensor_data_(AsyncWebServerRequest *request);
  void send_fan_config_(AsyncWebServerRequest *request);
  void send_options_(AsyncWebServerRequest *request);
  void send_info_(AsyncWebServerRequest *request);
  void handle_update_fan_config_(AsyncWebServerRequest *request);
  void handle_submit_options_(AsyncWebServerRequest *request);

  Settings settings_{};
  ESPPreferenceObject pref_;
  std::vector<output::FloatOutput *> fan_outputs_;
  const char *firmware_version_{"dev"};
  sensor::Sensor *nozzle_sensor_{nullptr};
  sensor::Sensor *fan_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};

  esp_mqtt_client_handle_t mqtt_{nullptr};
  std::string rx_buffer_;  // touched only from the MQTT task
  std::atomic<bool> mqtt_connected_{false};
  std::atomic<float> nozzle_temp_{0.0f};
  std::atomic<uint32_t> last_report_ms_{0};
  std::atomic<uint32_t> report_count_{0};
  std::atomic<uint32_t> dropped_count_{0};

  // Web handlers run in the httpd task; settings_ is shared with loop()
  mutable std::mutex settings_mutex_;
  std::atomic<bool> save_pending_{false};
  std::atomic<bool> restart_mqtt_{false};
  std::atomic<bool> reboot_pending_{false};
  std::atomic<uint8_t> fan_speed_{0};
  bool printer_online_{false};
  uint32_t last_control_ms_{0};
  uint32_t last_publish_ms_{0};
};

}  // namespace blflow
}  // namespace esphome
