#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/core/version.h"
#include "line_protocol.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"
#endif

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#endif

#if __has_include("esphome/components/globals/globals_component.h")
#include "esphome/components/globals/globals_component.h"
#define INFLUXDB_UDP_HAS_GLOBALS 1
#endif

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace esphome::influxdb_udp {

class InfluxDBUDP;

/// Lightweight handle for a named measurement or batch that can be published on demand
/// via `- influxdb_udp.publish: <id>` or scheduled periodically.
class BatchTarget {
 public:
  using PublishFn = void (*)(InfluxDBUDP *);

  void set_parent(InfluxDBUDP *parent) { this->parent_ = parent; }
  void set_publish_fn(PublishFn fn) { this->fn_ = fn; }
  void publish() {
    if (this->parent_ != nullptr && this->fn_ != nullptr) {
      this->fn_(this->parent_);
    }
  }

 protected:
  InfluxDBUDP *parent_{nullptr};
  PublishFn fn_{nullptr};
};

class InfluxDBUDP : public Component {
 public:
  using BatchCallback = void (*)(InfluxDBUDP *);

  InfluxDBUDP() : packet_buf_(this->inline_buf_, DEFAULT_MAX_PACKET_SIZE, &InfluxDBUDP::flush_trampoline_, this) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Configure publish_all mode: automatically stream telemetry from all non-internal entities.
  void set_publish_all(bool publish_all) { this->publish_all_ = publish_all; }
  void set_publish_all_include_internal(bool val) { this->publish_all_include_internal_ = val; }
  void set_publish_all_send_timeout(uint32_t ms) { this->send_timeout_ms_ = ms; }
  void set_publish_all_tags(const std::string &tags) { this->publish_all_tags_ = tags; }
  void set_publish_all_include_sensors(bool val) { this->publish_all_include_sensors_ = val; }
  void set_publish_all_include_binary_sensors(bool val) { this->publish_all_include_binary_sensors_ = val; }
  void set_publish_all_include_text_sensors(bool val) { this->publish_all_include_text_sensors_ = val; }
  void set_publish_all_include_switches(bool val) { this->publish_all_include_switches_ = val; }
  void set_publish_all_include_numbers(bool val) { this->publish_all_include_numbers_ = val; }
  void set_publish_all_include_selects(bool val) { this->publish_all_include_selects_ = val; }
  void add_publish_all_exclude(EntityBase *entity) {
    if (entity != nullptr) {
      this->publish_all_excludes_.push_back(entity);
    }
  }

  /// Configure destination host/IP address at boot or dynamically at runtime (no reboot required).
  void set_host(const std::string &host);
  const std::string &get_host() const { return this->host_; }

  /// Configure destination UDP port at boot or dynamically at runtime (no reboot required).
  void set_port(uint16_t port);
  uint16_t get_port() const { return this->port_; }

  /// Configure maximum UDP datagram payload size (defaults to 1472 bytes).
  void set_max_packet_size(size_t max_packet_size);
  size_t get_max_packet_size() const { return this->max_packet_size_; }

  /// Register a periodic batch callback to be executed every `interval_ms` milliseconds.
  void register_periodic_batch(uint32_t interval_ms, BatchCallback cb);

  /// Direct access to the single shared Line Protocol packet buffer.
  PacketBuffer &buffer() { return this->packet_buf_; }

  /// Flush any pending lines in the packet buffer over the UDP socket.
  void flush() { this->packet_buf_.flush(); }

  /// Send a raw UDP payload directly to the currently configured destination.
  void send_udp_payload(const uint8_t *data, size_t len);

  // ---------------------------------------------------------------------------
  // Overloaded Entity Field & Tag Helpers (called by generated C++ code)
  // ---------------------------------------------------------------------------

#ifdef USE_SENSOR
  void add_entity_field(const char *key, sensor::Sensor *s, int decimals = -1) {
    if (s == nullptr || !s->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    int dec = (decimals >= 0) ? decimals : static_cast<int>(s->get_accuracy_decimals());
    this->packet_buf_.add_field_float(key, s->state, dec);
  }

  void add_entity_field_int(const char *key, sensor::Sensor *s) {
    if (s == nullptr || !s->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_float_as_int(key, s->state);
  }

  void add_entity_field_uint(const char *key, sensor::Sensor *s, bool int_suffix = true) {
    if (s == nullptr || !s->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_float_as_uint(key, s->state, int_suffix);
  }

  void add_entity_field_bool(const char *key, sensor::Sensor *s) {
    if (s == nullptr || !s->has_state() || std::isnan(s->state)) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bool(key, s->state != 0.0f);
  }

  void add_entity_field_bit(const char *key, sensor::Sensor *s, uint8_t bit) {
    if (s == nullptr || !s->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bit(key, s->state, bit);
  }

  void add_entity_tag(const char *key, sensor::Sensor *s, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) ignore_values;
    if (s == nullptr || !s->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    int dec = (decimals >= 0) ? decimals : static_cast<int>(s->get_accuracy_decimals());
    this->packet_buf_.add_tag_float(key, s->state, dec);
  }
#endif

#ifdef USE_BINARY_SENSOR
  void add_entity_field(const char *key, binary_sensor::BinarySensor *b, int decimals = -1) {
    (void) decimals;
    if (b == nullptr || !b->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bool(key, b->state);
  }

  void add_entity_field_int(const char *key, binary_sensor::BinarySensor *b) {
    if (b == nullptr || !b->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_int(key, b->state ? 1 : 0);
  }

  void add_entity_field_uint(const char *key, binary_sensor::BinarySensor *b, bool int_suffix = true) {
    if (b == nullptr || !b->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_uint(key, b->state ? 1ULL : 0ULL, int_suffix);
  }

  void add_entity_field_bool(const char *key, binary_sensor::BinarySensor *b) { this->add_entity_field(key, b); }

  void add_entity_tag(const char *key, binary_sensor::BinarySensor *b, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    (void) ignore_values;
    if (b == nullptr || !b->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag_bool(key, b->state);
  }
#endif

#ifdef USE_SWITCH
  void add_entity_field(const char *key, switch_::Switch *sw, int decimals = -1) {
    (void) decimals;
    if (sw == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bool(key, sw->state);
  }

  void add_entity_field_int(const char *key, switch_::Switch *sw) {
    if (sw == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_int(key, sw->state ? 1 : 0);
  }

  void add_entity_field_uint(const char *key, switch_::Switch *sw, bool int_suffix = true) {
    if (sw == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_uint(key, sw->state ? 1ULL : 0ULL, int_suffix);
  }

  void add_entity_field_bool(const char *key, switch_::Switch *sw) { this->add_entity_field(key, sw); }

  void add_entity_tag(const char *key, switch_::Switch *sw, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    (void) ignore_values;
    if (sw == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag_bool(key, sw->state);
  }
#endif

#ifdef USE_TEXT_SENSOR
  void add_entity_field(const char *key, text_sensor::TextSensor *ts, int decimals = -1,
                        std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    if (ts == nullptr || !ts->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_str(key, ts->state, ignore_values);
  }

  void add_entity_tag(const char *key, text_sensor::TextSensor *ts, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    if (ts == nullptr || !ts->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag(key, ts->state, ignore_values);
  }
#endif

#ifdef USE_NUMBER
  void add_entity_field(const char *key, number::Number *num, int decimals = -1) {
    if (num == nullptr || !num->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_float(key, num->state, decimals);
  }

  void add_entity_field_int(const char *key, number::Number *num) {
    if (num == nullptr || !num->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_float_as_int(key, num->state);
  }

  void add_entity_field_uint(const char *key, number::Number *num, bool int_suffix = true) {
    if (num == nullptr || !num->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_float_as_uint(key, num->state, int_suffix);
  }

  void add_entity_field_bool(const char *key, number::Number *num) {
    if (num == nullptr || !num->has_state() || std::isnan(num->state)) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bool(key, num->state != 0.0f);
  }

  void add_entity_tag(const char *key, number::Number *num, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) ignore_values;
    if (num == nullptr || !num->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag_float(key, num->state, decimals);
  }
#endif

#ifdef USE_SELECT
  void add_entity_field(const char *key, select::Select *sel, int decimals = -1,
                        std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    if (sel == nullptr || !sel->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_str(key, sel->current_option(), ignore_values);
  }

  void add_entity_tag(const char *key, select::Select *sel, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    if (sel == nullptr || !sel->has_state()) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag(key, sel->current_option(), ignore_values);
  }
#endif

#ifdef INFLUXDB_UDP_HAS_GLOBALS
  template<typename T>
  void add_entity_field(const char *key, globals::GlobalsComponent<T> *g, int decimals = -1,
                        std::initializer_list<const char *> ignore_values = {}) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_auto(key, g->value(), decimals, ignore_values);
  }

  template<typename T>
  void add_entity_field(const char *key, globals::RestoringGlobalsComponent<T> *g, int decimals = -1,
                        std::initializer_list<const char *> ignore_values = {}) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_auto(key, g->value(), decimals, ignore_values);
  }

  template<typename T, uint8_t SZ>
  void add_entity_field(const char *key, globals::RestoringGlobalStringComponent<T, SZ> *g, int decimals = -1,
                        std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_str(key, g->value().c_str(), ignore_values);
  }

  template<typename T>
  void add_entity_field_int(const char *key, globals::GlobalsComponent<T> *g) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_int(key, static_cast<int64_t>(g->value()));
  }

  template<typename T>
  void add_entity_field_int(const char *key, globals::RestoringGlobalsComponent<T> *g) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_int(key, static_cast<int64_t>(g->value()));
  }

  template<typename T>
  void add_entity_field_uint(const char *key, globals::GlobalsComponent<T> *g, bool int_suffix = true) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_uint(key, static_cast<uint64_t>(g->value()), int_suffix);
  }

  template<typename T>
  void add_entity_field_uint(const char *key, globals::RestoringGlobalsComponent<T> *g, bool int_suffix = true) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_uint(key, static_cast<uint64_t>(g->value()), int_suffix);
  }

  template<typename T>
  void add_entity_field_bool(const char *key, globals::GlobalsComponent<T> *g) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bool(key, static_cast<bool>(g->value()));
  }

  template<typename T>
  void add_entity_field_bool(const char *key, globals::RestoringGlobalsComponent<T> *g) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bool(key, static_cast<bool>(g->value()));
  }

  template<typename T>
  void add_entity_field_bit(const char *key, globals::GlobalsComponent<T> *g, uint8_t bit) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bit(key, static_cast<float>(g->value()), bit);
  }

  template<typename T>
  void add_entity_field_bit(const char *key, globals::RestoringGlobalsComponent<T> *g, uint8_t bit) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_field_bit(key, static_cast<float>(g->value()), bit);
  }

  template<typename T>
  void add_entity_tag(const char *key, globals::GlobalsComponent<T> *g, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag_auto(key, g->value(), decimals, ignore_values);
  }

  template<typename T>
  void add_entity_tag(const char *key, globals::RestoringGlobalsComponent<T> *g, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag_auto(key, g->value(), decimals, ignore_values);
  }

  template<typename T, uint8_t SZ>
  void add_entity_tag(const char *key, globals::RestoringGlobalStringComponent<T, SZ> *g, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    (void) decimals;
    if (g == nullptr) {
      this->packet_buf_.mark_field_unavailable();
      return;
    }
    this->packet_buf_.add_tag(key, g->value().c_str(), ignore_values);
  }
#endif

 protected:
  static void flush_trampoline_(void *ctx, const uint8_t *data, size_t len) {
    static_cast<InfluxDBUDP *>(ctx)->send_udp_payload(data, len);
  }

  void update_sockaddr_();
  void setup_publish_all_();
  bool is_publish_all_excluded_(EntityBase *obj) const;
  void schedule_publish_all_flush_();

  bool publish_all_{false};
  bool publish_all_include_internal_{false};
  bool publish_all_include_sensors_{true};
  bool publish_all_include_binary_sensors_{true};
  bool publish_all_include_text_sensors_{true};
  bool publish_all_include_switches_{false};
  bool publish_all_include_numbers_{false};
  bool publish_all_include_selects_{false};
  uint32_t send_timeout_ms_{100};
  uint32_t last_buffered_time_{0};
  bool has_pending_flush_{false};
  std::string publish_all_tags_{};
  std::vector<EntityBase *> publish_all_excludes_{};

  struct PeriodicBatchEntry {
    uint32_t interval_ms;
    BatchCallback callback;
  };

  std::string host_{};
  uint16_t port_{8089};
  size_t max_packet_size_{DEFAULT_MAX_PACKET_SIZE};
  char inline_buf_[DEFAULT_MAX_PACKET_SIZE]{};
  std::unique_ptr<char[]> custom_buf_{nullptr};
  PacketBuffer packet_buf_;
  std::vector<PeriodicBatchEntry> periodic_batches_{};

#ifdef USE_NETWORK
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 11, 0)
  std::unique_ptr<socket::UDPSendSocket> socket_{nullptr};
#else
  std::unique_ptr<socket::Socket> socket_{nullptr};
#endif
  struct sockaddr_storage dest_addr_{};
  socklen_t dest_addr_len_{0};
#endif
  bool dest_valid_{false};
};

#ifdef USE_TEXT
/// Auto-created runtime configuration `text` entity for the InfluxDB UDP host.
/// Restores from NVS flash on boot and updates the UDP destination address immediately on change.
class InfluxDBHostText : public text::Text, public Component, public Parented<InfluxDBUDP> {
 public:
  void set_initial_value(const std::string &initial_value) { this->initial_value_ = initial_value; }
  void set_restore_value(bool restore_value) { this->restore_value_ = restore_value; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(const std::string &value) override;

  std::string initial_value_{};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};
#endif

#ifdef USE_NUMBER
/// Auto-created runtime configuration `number` entity for the InfluxDB UDP port.
/// Restores from NVS flash on boot and updates the UDP destination port immediately on change.
class InfluxDBPortNumber : public number::Number, public Component, public Parented<InfluxDBUDP> {
 public:
  void set_initial_value(uint16_t initial_value) { this->initial_value_ = initial_value; }
  void set_restore_value(bool restore_value) { this->restore_value_ = restore_value; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(float value) override;

  uint16_t initial_value_{8089};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};
#endif

/// Automation action for `- influxdb_udp.publish: <batch_or_measurement_id>`.
template<typename... Ts> class PublishAction : public Action<Ts...> {
 public:
  explicit PublishAction(BatchTarget *target) : target_(target) {}

  void play(const Ts &...x) override {
    if (this->target_ != nullptr) {
      this->target_->publish();
    }
  }

 protected:
  BatchTarget *target_{nullptr};
};

}  // namespace esphome::influxdb_udp

