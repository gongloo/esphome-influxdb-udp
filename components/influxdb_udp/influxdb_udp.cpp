#include "influxdb_udp.h"
#include "esphome/core/application.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace esphome::influxdb_udp {

static const char *const TAG = "influxdb_udp";

void InfluxDBUDP::set_max_packet_size(size_t max_packet_size) {
  if (max_packet_size == 0)
    return;
  this->max_packet_size_ = max_packet_size;
  if (max_packet_size <= DEFAULT_MAX_PACKET_SIZE) {
    this->custom_buf_.reset();
    this->packet_buf_ = PacketBuffer(this->inline_buf_, max_packet_size, &InfluxDBUDP::flush_trampoline_, this);
  } else {
    this->custom_buf_ = std::make_unique<char[]>(max_packet_size);
    this->packet_buf_ = PacketBuffer(this->custom_buf_.get(), max_packet_size, &InfluxDBUDP::flush_trampoline_, this);
  }
}

void InfluxDBUDP::set_host(const std::string &host) {
  this->host_ = host;
  this->update_sockaddr_();
}

void InfluxDBUDP::set_port(uint16_t port) {
  this->port_ = port;
  this->update_sockaddr_();
}

void InfluxDBUDP::update_sockaddr_() {
  this->dest_valid_ = false;
  if (this->host_.empty() || this->port_ == 0) {
    return;
  }
#ifdef USE_NETWORK
  std::memset(&this->dest_addr_, 0, sizeof(this->dest_addr_));
  this->dest_addr_len_ = socket::set_sockaddr(reinterpret_cast<struct sockaddr *>(&this->dest_addr_),
                                              sizeof(this->dest_addr_), this->host_, this->port_);
  if (this->dest_addr_len_ > 0) {
    this->dest_valid_ = true;
    ESP_LOGD(TAG, "Updated InfluxDB UDP destination to %s:%u", this->host_.c_str(), this->port_);
  } else {
    ESP_LOGW(TAG, "Invalid InfluxDB UDP destination address '%s:%u'", this->host_.c_str(), this->port_);
  }
#endif
}

void InfluxDBUDP::register_periodic_batch(uint32_t interval_ms, BatchCallback cb) {
  if (cb == nullptr || interval_ms == 0)
    return;
  this->periodic_batches_.push_back({interval_ms, cb});
}

void InfluxDBUDP::setup() {
#ifdef USE_NETWORK
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 11, 0)
  this->socket_ = socket::socket_ip_udp_send(0);
#else
  this->socket_ = socket::socket_ip(SOCK_DGRAM, 0);
#endif
  if (this->socket_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create UDP socket for InfluxDB");
    this->mark_failed();
    return;
  }
  this->socket_->setblocking(false);
  int enable = 1;
  this->socket_->setsockopt(SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
#endif

  this->update_sockaddr_();

  for (const auto &entry : this->periodic_batches_) {
    BatchCallback cb = entry.callback;
    this->set_interval(entry.interval_ms, [this, cb]() { cb(this); });
  }

  this->setup_publish_all_();
}

void InfluxDBUDP::dump_config() {
  ESP_LOGCONFIG(TAG, "InfluxDB UDP:");
  ESP_LOGCONFIG(TAG, "  Destination: %s:%u (%s)", this->host_.empty() ? "<unset>" : this->host_.c_str(), this->port_,
                this->dest_valid_ ? "valid" : "inactive");
  ESP_LOGCONFIG(TAG, "  Max Packet Size: %u bytes", static_cast<unsigned>(this->max_packet_size_));
  ESP_LOGCONFIG(TAG, "  Periodic Batch Groups: %u", static_cast<unsigned>(this->periodic_batches_.size()));
  if (this->publish_all_) {
    ESP_LOGCONFIG(TAG, "  Publish All: enabled (send_timeout: %u ms)", static_cast<unsigned>(this->send_timeout_ms_));
  }
}

void InfluxDBUDP::loop() {
  if (this->has_pending_flush_ && (millis() - this->last_buffered_time_ >= this->send_timeout_ms_)) {
    this->flush();
    this->has_pending_flush_ = false;
  }
}

bool InfluxDBUDP::is_publish_all_excluded_(EntityBase *obj) const {
  if (obj == nullptr)
    return true;
  if (!this->publish_all_include_internal_ && obj->is_internal())
    return true;
  return std::any_of(this->publish_all_excludes_.begin(), this->publish_all_excludes_.end(),
                     [obj](EntityBase *e) { return e == obj; });
}

void InfluxDBUDP::schedule_publish_all_flush_() {
  if (this->send_timeout_ms_ == 0) {
    this->flush();
    return;
  }
  this->last_buffered_time_ = millis();
  this->has_pending_flush_ = true;
}

void InfluxDBUDP::setup_publish_all_() {
  if (!this->publish_all_)
    return;

#ifndef OBJECT_ID_MAX_LEN
  static constexpr size_t OBJECT_ID_MAX_LEN = 128;
#endif

#ifdef USE_BINARY_SENSOR
  if (this->publish_all_include_binary_sensors_) {
    for (auto *obj : App.get_binary_sensors()) {
      if (this->is_publish_all_excluded_(obj))
        continue;
      char buf[OBJECT_ID_MAX_LEN];
      std::string measurement = obj->get_object_id_to(buf).str();
      if (measurement.empty())
        continue;
      obj->add_on_state_callback([this, measurement](bool state) {
        this->packet_buf_.build_line(false, [&]() {
          this->packet_buf_.begin_measurement_escaped(measurement.c_str(),
                                                      this->publish_all_tags_.empty() ? nullptr : this->publish_all_tags_.c_str());
          this->packet_buf_.add_field_bool("value", state);
        });
        this->schedule_publish_all_flush_();
      });
    }
  }
#endif

#ifdef USE_SENSOR
  if (this->publish_all_include_sensors_) {
    for (auto *obj : App.get_sensors()) {
      if (this->is_publish_all_excluded_(obj))
        continue;
      char buf[OBJECT_ID_MAX_LEN];
      std::string measurement = obj->get_object_id_to(buf).str();
      if (measurement.empty())
        continue;
      int8_t dec = obj->get_accuracy_decimals();
      obj->add_on_state_callback([this, measurement, dec](float state) {
        if (std::isnan(state) || std::isinf(state))
          return;
        this->packet_buf_.build_line(false, [&]() {
          this->packet_buf_.begin_measurement_escaped(measurement.c_str(),
                                                      this->publish_all_tags_.empty() ? nullptr : this->publish_all_tags_.c_str());
          this->packet_buf_.add_field_float("value", state, dec);
        });
        this->schedule_publish_all_flush_();
      });
    }
  }
#endif

#ifdef USE_TEXT_SENSOR
  if (this->publish_all_include_text_sensors_) {
    for (auto *obj : App.get_text_sensors()) {
      if (this->is_publish_all_excluded_(obj))
        continue;
      char buf[OBJECT_ID_MAX_LEN];
      std::string measurement = obj->get_object_id_to(buf).str();
      if (measurement.empty())
        continue;
      obj->add_on_state_callback([this, measurement](const std::string &state) {
        if (state.empty())
          return;
        this->packet_buf_.build_line(false, [&]() {
          this->packet_buf_.begin_measurement_escaped(measurement.c_str(),
                                                      this->publish_all_tags_.empty() ? nullptr : this->publish_all_tags_.c_str());
          this->packet_buf_.add_field_str("value", state);
        });
        this->schedule_publish_all_flush_();
      });
    }
  }
#endif

#ifdef USE_SWITCH
  if (this->publish_all_include_switches_) {
    for (auto *obj : App.get_switches()) {
      if (this->is_publish_all_excluded_(obj))
        continue;
      char buf[OBJECT_ID_MAX_LEN];
      std::string measurement = obj->get_object_id_to(buf).str();
      if (measurement.empty())
        continue;
      obj->add_on_state_callback([this, measurement](bool state) {
        this->packet_buf_.build_line(false, [&]() {
          this->packet_buf_.begin_measurement_escaped(measurement.c_str(),
                                                      this->publish_all_tags_.empty() ? nullptr : this->publish_all_tags_.c_str());
          this->packet_buf_.add_field_bool("value", state);
        });
        this->schedule_publish_all_flush_();
      });
    }
  }
#endif

#ifdef USE_NUMBER
  if (this->publish_all_include_numbers_) {
    for (auto *obj : App.get_numbers()) {
      if (this->is_publish_all_excluded_(obj))
        continue;
      char buf[OBJECT_ID_MAX_LEN];
      std::string measurement = obj->get_object_id_to(buf).str();
      if (measurement.empty())
        continue;
      obj->add_on_state_callback([this, measurement](float state) {
        if (std::isnan(state) || std::isinf(state))
          return;
        this->packet_buf_.build_line(false, [&]() {
          this->packet_buf_.begin_measurement_escaped(measurement.c_str(),
                                                      this->publish_all_tags_.empty() ? nullptr : this->publish_all_tags_.c_str());
          this->packet_buf_.add_field_float("value", state);
        });
        this->schedule_publish_all_flush_();
      });
    }
  }
#endif
}

void InfluxDBUDP::send_udp_payload(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || !this->dest_valid_)
    return;
#ifdef USE_NETWORK
  if (this->socket_ == nullptr || !network::is_connected())
    return;
  ssize_t sent = this->socket_->sendto(data, len, 0, reinterpret_cast<const struct sockaddr *>(&this->dest_addr_),
                                       this->dest_addr_len_);
  if (sent < 0) {
    ESP_LOGW(TAG, "UDP sendto(%s:%u, %u bytes) failed: errno %d", this->host_.c_str(), this->port_,
             static_cast<unsigned>(len), errno);
  }
#endif
}

#ifdef USE_TEXT
namespace {
struct StoredHost {
  char host[64];
};
}  // namespace

void InfluxDBHostText::setup() {
  std::string value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<StoredHost>(this->get_object_id_hash());
    StoredHost stored{};
    if (this->pref_.load(&stored)) {
      stored.host[sizeof(stored.host) - 1] = '\0';
      value = stored.host;
    }
  }
  this->publish_state(value);
  if (this->parent_ != nullptr) {
    this->parent_->set_host(value);
  }
}

void InfluxDBHostText::control(const std::string &value) {
  if (this->restore_value_) {
    StoredHost stored{};
    std::strncpy(stored.host, value.c_str(), sizeof(stored.host) - 1);
    stored.host[sizeof(stored.host) - 1] = '\0';
    this->pref_.save(&stored);
  }
  this->publish_state(value);
  if (this->parent_ != nullptr) {
    this->parent_->set_host(value);
  }
}
#endif

#ifdef USE_NUMBER
void InfluxDBPortNumber::setup() {
  uint16_t port = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<uint16_t>(this->get_object_id_hash());
    uint16_t stored = 0;
    if (this->pref_.load(&stored) && stored > 0) {
      port = stored;
    }
  }
  this->publish_state(static_cast<float>(port));
  if (this->parent_ != nullptr) {
    this->parent_->set_port(port);
  }
}

void InfluxDBPortNumber::control(float value) {
  auto port = static_cast<uint16_t>(value);
  if (this->restore_value_) {
    this->pref_.save(&port);
  }
  this->publish_state(static_cast<float>(port));
  if (this->parent_ != nullptr) {
    this->parent_->set_port(port);
  }
}
#endif

}  // namespace esphome::influxdb_udp

