#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>

namespace esphome::influxdb_udp {

/// Default maximum UDP payload size (1500 Ethernet/Wi-Fi MTU - 20 IPv4 header - 8 UDP header = 1472 bytes).
static constexpr size_t DEFAULT_MAX_PACKET_SIZE = 1472;

/**
 * Zero-allocation, single-pass InfluxDB Line Protocol serializer.
 *
 * Formats measurements, tags, and typed fields directly into a pre-allocated buffer
 * with in-place rollback when a line has no valid fields or triggers `skip_if_nan`,
 * and automatic MTU boundary flushing when batching multiple lines into a single UDP datagram.
 */
class PacketBuffer {
 public:
  using FlushCallback = void (*)(void *ctx, const uint8_t *data, size_t len);

  PacketBuffer(char *buf, size_t capacity, FlushCallback flush_cb = nullptr, void *flush_ctx = nullptr)
      : buf_(buf), capacity_(capacity), flush_cb_(flush_cb), flush_ctx_(flush_ctx) {}

  /// Current committed byte length in buffer.
  size_t size() const { return this->len_; }
  /// Maximum buffer capacity in bytes.
  size_t capacity() const { return this->capacity_; }
  /// True if no bytes are currently buffered.
  bool empty() const { return this->len_ == 0; }
  /// Pointer to buffered data.
  const char *data() const { return this->buf_; }
  /// Number of oversized lines dropped because a single line exceeded `capacity()`.
  uint32_t oversized_drops() const { return this->oversized_drops_; }

  /// Reset buffer state without flushing.
  void clear() {
    this->len_ = 0;
    this->line_start_ = 0;
    this->has_fields_ = false;
    this->in_fields_ = false;
    this->line_aborted_ = false;
    this->line_overflow_ = false;
  }

  /// Flush any committed data in the buffer via the registered FlushCallback.
  void flush() {
    if (this->len_ > 0 && this->flush_cb_ != nullptr) {
      this->flush_cb_(this->flush_ctx_, reinterpret_cast<const uint8_t *>(this->buf_), this->len_);
    }
    this->clear();
  }

  /**
   * Execute a line-building callable with automatic rollback and MTU overflow split/retry.
   *
   * @param skip_if_nan If true, abort and roll back the entire line if any tag or field is NaN/unavailable.
   * @param builder     Callable that invokes `begin_measurement()`, `add_tag_*()`, and `add_field_*()`.
   * @return true if the line was committed to the buffer, false if skipped/rolled back.
   */
  template<typename F> bool build_line(bool skip_if_nan, F &&builder) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      this->line_start_ = this->len_;
      this->has_fields_ = false;
      this->in_fields_ = false;
      this->line_aborted_ = false;
      this->line_overflow_ = false;
      this->skip_if_nan_ = skip_if_nan;

      if (this->line_start_ > 0) {
        this->append_raw_char_('\n');
      }

      builder();

      if (this->line_overflow_) {
        if (this->line_start_ > 0) {
          // Flush already-committed lines (0 .. line_start_ - 1) and retry this line at offset 0
          this->len_ = this->line_start_;
          this->flush();
          continue;
        }
        // Single line at offset 0 exceeded entire buffer capacity
        this->len_ = 0;
        this->oversized_drops_++;
        return false;
      }

      if (this->line_aborted_ || !this->has_fields_) {
        this->len_ = this->line_start_;
        return false;
      }

      return true;
    }
    return false;
  }

  /// Begin a measurement line with pre-escaped measurement name and optional pre-escaped static tags
  /// (where `static_tags` starts with `,` if non-empty, e.g. `,host=my_node`).
  void begin_measurement(const char *measurement, const char *static_tags = nullptr) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (measurement == nullptr || measurement[0] == '\0') {
      this->line_aborted_ = true;
      return;
    }
    this->append_raw_str_(measurement);
    if (static_tags != nullptr && static_tags[0] != '\0') {
      this->append_raw_str_(static_tags);
    }
  }

  /// Begin a measurement line and escape the measurement name at runtime.
  void begin_measurement_escaped(const char *measurement, const char *static_tags = nullptr) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (measurement == nullptr || measurement[0] == '\0') {
      this->line_aborted_ = true;
      return;
    }
    this->append_escaped_measurement_(measurement);
    if (static_tags != nullptr && static_tags[0] != '\0') {
      this->append_raw_str_(static_tags);
    }
  }

  /// Add a dynamic string tag. Must be called before any `add_field_*()` calls on the current line.
  void add_tag(const char *key, const char *val, std::initializer_list<const char *> ignore_values = {}) {
    if (this->line_aborted_ || this->line_overflow_ || this->in_fields_)
      return;
    if (key == nullptr || key[0] == '\0' || is_ignored_or_empty_(val, ignore_values)) {
      this->handle_missing_();
      return;
    }
    this->append_raw_char_(',');
    this->append_escaped_tag_(key);
    this->append_raw_char_('=');
    this->append_escaped_tag_(val);
  }

  void add_tag(const char *key, const std::string &val, std::initializer_list<const char *> ignore_values = {}) {
    this->add_tag(key, val.c_str(), ignore_values);
  }

  /// Add a dynamic numeric tag (e.g. from a sensor or integer expression).
  void add_tag_int(const char *key, int64_t val) {
    if (this->line_aborted_ || this->line_overflow_ || this->in_fields_)
      return;
    if (key == nullptr || key[0] == '\0') {
      this->handle_missing_();
      return;
    }
    this->append_raw_char_(',');
    this->append_escaped_tag_(key);
    this->append_raw_char_('=');
    this->append_printf_("%lld", static_cast<long long>(val));
  }

  void add_tag_float(const char *key, float val, int decimals = -1) {
    if (this->line_aborted_ || this->line_overflow_ || this->in_fields_)
      return;
    if (key == nullptr || key[0] == '\0' || std::isnan(val) || std::isinf(val)) {
      this->handle_missing_();
      return;
    }
    this->append_raw_char_(',');
    this->append_escaped_tag_(key);
    this->append_raw_char_('=');
    if (decimals >= 0) {
      this->append_printf_("%.*f", decimals, static_cast<double>(val));
    } else {
      this->append_printf_("%.4g", static_cast<double>(val));
    }
  }

  void add_tag_bool(const char *key, bool val) {
    this->add_tag(key, val ? "true" : "false");
  }

  template<typename T>
  void add_tag_auto(const char *key, const T &val, int decimals = -1,
                    std::initializer_list<const char *> ignore_values = {}) {
    using DecayT = std::decay_t<T>;
    if constexpr (std::is_same_v<DecayT, bool>) {
      this->add_tag_bool(key, val);
    } else if constexpr (std::is_floating_point_v<DecayT>) {
      this->add_tag_float(key, static_cast<float>(val), decimals);
    } else if constexpr (std::is_integral_v<DecayT>) {
      this->add_tag_int(key, static_cast<int64_t>(val));
    } else if constexpr (std::is_convertible_v<DecayT, const char *>) {
      this->add_tag(key, static_cast<const char *>(val), ignore_values);
    } else if constexpr (std::is_same_v<DecayT, std::string>) {
      this->add_tag(key, val.c_str(), ignore_values);
    }
  }

  /// Add a floating-point field. If `decimals >= 0`, formats with `%.*f`; if `decimals < 0`, formats with `%.4g`.
  void add_field_float(const char *key, float val, int decimals = -1) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (std::isnan(val) || std::isinf(val)) {
      this->handle_missing_();
      return;
    }
    this->begin_field_(key);
    if (decimals >= 0) {
      this->append_printf_("%.*f", decimals, static_cast<double>(val));
    } else {
      this->append_printf_("%.4g", static_cast<double>(val));
    }
  }

  /// Add a signed integer field (with InfluxDB `i` suffix).
  void add_field_int(const char *key, int64_t val) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    this->begin_field_(key);
    this->append_printf_("%lldi", static_cast<long long>(val));
  }

  /// Add a floating-point value rounded to a signed integer field (with InfluxDB `i` suffix).
  void add_field_float_as_int(const char *key, float val) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (std::isnan(val) || std::isinf(val)) {
      this->handle_missing_();
      return;
    }
    this->add_field_int(key, static_cast<int64_t>(std::llround(val)));
  }

  /// Add an unsigned integer field (with InfluxDB `i` suffix, or without suffix if `int_suffix == false`).
  void add_field_uint(const char *key, uint64_t val, bool int_suffix = true) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    this->begin_field_(key);
    if (int_suffix) {
      this->append_printf_("%llui", static_cast<unsigned long long>(val));
    } else {
      this->append_printf_("%llu", static_cast<unsigned long long>(val));
    }
  }

  /// Add a floating-point value rounded to an unsigned integer field. Skips if NaN or negative.
  void add_field_float_as_uint(const char *key, float val, bool int_suffix = true) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (std::isnan(val) || std::isinf(val) || val < 0.0f) {
      this->handle_missing_();
      return;
    }
    this->add_field_uint(key, static_cast<uint64_t>(std::llround(val)), int_suffix);
  }

  /// Add a boolean field (`T` or `F`).
  void add_field_bool(const char *key, bool val) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    this->begin_field_(key);
    this->append_raw_char_(val ? 'T' : 'F');
  }

  /// Extract a single bit (0..31) from a bitmask float sensor and emit as a boolean field (`T` or `F`).
  void add_field_bit(const char *key, float bitmask_val, uint8_t bit) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (std::isnan(bitmask_val) || std::isinf(bitmask_val) || bit > 31) {
      this->handle_missing_();
      return;
    }
    uint32_t mask = static_cast<uint32_t>(bitmask_val);
    this->add_field_bool(key, (mask & (1UL << bit)) != 0);
  }

  /// Add a string field (`"..."` with escaping). Skips if null, empty, or in `ignore_values`.
  void add_field_str(const char *key, const char *val, std::initializer_list<const char *> ignore_values = {}) {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    if (is_ignored_or_empty_(val, ignore_values)) {
      this->handle_missing_();
      return;
    }
    this->begin_field_(key);
    this->append_escaped_field_str_(val);
  }

  void add_field_str(const char *key, const std::string &val, std::initializer_list<const char *> ignore_values = {}) {
    this->add_field_str(key, val.c_str(), ignore_values);
  }

  /// Mark a field or tag as missing/unavailable (triggers rollback if `skip_if_nan` is enabled).
  void mark_field_unavailable() {
    if (this->line_aborted_ || this->line_overflow_)
      return;
    this->handle_missing_();
  }

  /// Auto-typed field helper for C++ lambda return values.
  template<typename T>
  void add_field_auto(const char *key, const T &val, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    using DecayT = std::decay_t<T>;
    if constexpr (std::is_same_v<DecayT, bool>) {
      this->add_field_bool(key, val);
    } else if constexpr (std::is_floating_point_v<DecayT>) {
      this->add_field_float(key, static_cast<float>(val), decimals);
    } else if constexpr (std::is_integral_v<DecayT> && std::is_unsigned_v<DecayT>) {
      this->add_field_uint(key, static_cast<uint64_t>(val));
    } else if constexpr (std::is_integral_v<DecayT>) {
      this->add_field_int(key, static_cast<int64_t>(val));
    } else if constexpr (std::is_convertible_v<DecayT, const char *>) {
      this->add_field_str(key, static_cast<const char *>(val), ignore_values);
    } else if constexpr (std::is_same_v<DecayT, std::string>) {
      this->add_field_str(key, val.c_str(), ignore_values);
    }
  }

  template<typename T>
  void add_field_auto(const char *key, const std::optional<T> &val, int decimals = -1,
                      std::initializer_list<const char *> ignore_values = {}) {
    if (!val.has_value()) {
      this->mark_field_unavailable();
      return;
    }
    this->add_field_auto(key, *val, decimals, ignore_values);
  }

 protected:
  static bool is_ignored_or_empty_(const char *val, std::initializer_list<const char *> ignore_values) {
    if (val == nullptr || val[0] == '\0')
      return true;
    for (const char *ign : ignore_values) {
      if (ign != nullptr && std::strcmp(val, ign) == 0)
        return true;
    }
    return false;
  }

  void handle_missing_() {
    if (this->skip_if_nan_) {
      this->line_aborted_ = true;
    }
  }

  void begin_field_(const char *key) {
    if (!this->in_fields_) {
      this->append_raw_char_(' ');
      this->in_fields_ = true;
    } else {
      this->append_raw_char_(',');
    }
    this->append_escaped_tag_(key);
    this->append_raw_char_('=');
    this->has_fields_ = true;
  }

  void append_raw_char_(char c) {
    if (this->line_overflow_)
      return;
    if (this->len_ >= this->capacity_) {
      this->line_overflow_ = true;
      return;
    }
    this->buf_[this->len_++] = c;
  }

  void append_raw_str_(const char *s) {
    if (this->line_overflow_ || s == nullptr)
      return;
    size_t n = std::strlen(s);
    if (this->len_ + n > this->capacity_) {
      this->line_overflow_ = true;
      return;
    }
    std::memcpy(this->buf_ + this->len_, s, n);
    this->len_ += n;
  }

  void append_escaped_measurement_(const char *s) {
    for (const char *p = s; *p != '\0'; ++p) {
      char c = *p;
      if (c == ',' || c == ' ' || c == '\\') {
        this->append_raw_char_('\\');
      }
      this->append_raw_char_(c);
    }
  }

  void append_escaped_tag_(const char *s) {
    for (const char *p = s; *p != '\0'; ++p) {
      char c = *p;
      if (c == ',' || c == '=' || c == ' ' || c == '\\') {
        this->append_raw_char_('\\');
      }
      this->append_raw_char_(c);
    }
  }

  void append_escaped_field_str_(const char *s) {
    this->append_raw_char_('"');
    for (const char *p = s; *p != '\0'; ++p) {
      char c = *p;
      if (c == '"' || c == '\\') {
        this->append_raw_char_('\\');
      }
      this->append_raw_char_(c);
    }
    this->append_raw_char_('"');
  }

  template<typename... Args> void append_printf_(const char *fmt, Args... args) {
    if (this->line_overflow_)
      return;
    size_t rem = (this->len_ < this->capacity_) ? (this->capacity_ - this->len_) : 0;
    char tmp[64];
    int n = std::snprintf(tmp, sizeof(tmp), fmt, args...);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) {
      this->line_aborted_ = true;
      return;
    }
    if (static_cast<size_t>(n) > rem) {
      this->line_overflow_ = true;
      return;
    }
    std::memcpy(this->buf_ + this->len_, tmp, static_cast<size_t>(n));
    this->len_ += static_cast<size_t>(n);
  }

  char *buf_{nullptr};
  size_t capacity_{0};
  size_t len_{0};
  size_t line_start_{0};
  uint32_t oversized_drops_{0};
  FlushCallback flush_cb_{nullptr};
  void *flush_ctx_{nullptr};
  bool has_fields_{false};
  bool in_fields_{false};
  bool line_aborted_{false};
  bool line_overflow_{false};
  bool skip_if_nan_{false};
};

}  // namespace esphome::influxdb_udp
