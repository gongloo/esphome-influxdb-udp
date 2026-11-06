#include <doctest/doctest.h>
#include "influxdb_udp/line_protocol.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using esphome::influxdb_udp::DEFAULT_MAX_PACKET_SIZE;
using esphome::influxdb_udp::PacketBuffer;

namespace {

struct CaptureSink {
  std::vector<std::string> packets;

  static void callback(void *ctx, const uint8_t *data, size_t len) {
    auto *self = static_cast<CaptureSink *>(ctx);
    self->packets.emplace_back(reinterpret_cast<const char *>(data), len);
  }
};

}  // namespace

TEST_CASE("Single measurement formatting with static tags and typed fields") {
  CaptureSink sink;
  char raw[DEFAULT_MAX_PACKET_SIZE];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);

  bool committed = pb.build_line(false, [&]() {
    pb.begin_measurement("victron_orion", ",device=orion_xs,host=8xrelaybank");
    pb.add_field_float("input_voltage", 13.846f, 2);
    pb.add_field_float("output_current", 24.5f, 1);
    pb.add_field_int("charge_mode_code", -3);
    pb.add_field_uint("uptime", 3600ULL, true);
    pb.add_field_uint("raw_counter", 42ULL, false);
    pb.add_field_bool("charging", true);
    pb.add_field_bool("fault", false);
    pb.add_field_str("state", "Bulk");
  });

  REQUIRE(committed);
  pb.flush();

  REQUIRE(sink.packets.size() == 1);
  CHECK(sink.packets[0] ==
        "victron_orion,device=orion_xs,host=8xrelaybank "
        "input_voltage=13.85,output_current=24.5,charge_mode_code=-3i,uptime=3600i,"
        "raw_counter=42,charging=T,fault=F,state=\"Bulk\"");
}

TEST_CASE("Default NaN behavior omits invalid fields and drops line if all fields are NaN") {
  CaptureSink sink;
  char raw[DEFAULT_MAX_PACKET_SIZE];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);
  const float nan_val = std::numeric_limits<float>::quiet_NaN();
  const float inf_val = std::numeric_limits<float>::infinity();

  SUBCASE("Partial NaN fields are omitted while valid fields are preserved") {
    bool ok = pb.build_line(false, [&]() {
      pb.begin_measurement("bms", ",bms_id=0");
      pb.add_field_float("total_voltage", nan_val, 2);
      pb.add_field_float("current", 12.3f, 1);
      pb.add_field_float("power", inf_val, 0);
      pb.add_field_str("errors", "");
      pb.add_field_str("status", "Unavailable", {"Unavailable", "Unknown"});
    });
    REQUIRE(ok);
    pb.flush();
    REQUIRE(sink.packets.size() == 1);
    CHECK(sink.packets[0] == "bms,bms_id=0 current=12.3");
  }

  SUBCASE("Line is completely rolled back if every field is NaN or unavailable") {
    pb.build_line(false, [&]() {
      pb.begin_measurement("system", ",host=node1");
      pb.add_field_uint("uptime", 100ULL);
    });

    bool second_ok = pb.build_line(false, [&]() {
      pb.begin_measurement("offline_device", ",host=node1");
      pb.add_field_float("voltage", nan_val, 2);
      pb.add_field_float_as_int("soc", nan_val);
      pb.add_field_float_as_uint("cycles", -1.0f);
      pb.add_field_bit("bit0", nan_val, 0);
      pb.add_field_str("mode", "Unknown", {"Unknown"});
      pb.mark_field_unavailable();
    });
    CHECK_FALSE(second_ok);

    pb.flush();
    REQUIRE(sink.packets.size() == 1);
    // Ensure the rolled-back second line left no trailing '\n' on the first line
    CHECK(sink.packets[0] == "system,host=node1 uptime=100i");
  }
}

TEST_CASE("skip_if_nan drops the entire measurement when any tag or field is NaN") {
  CaptureSink sink;
  char raw[DEFAULT_MAX_PACKET_SIZE];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);
  const float nan_val = std::numeric_limits<float>::quiet_NaN();

  // First line valid
  REQUIRE(pb.build_line(true, [&]() {
    pb.begin_measurement("hyheat", ",unit=1");
    pb.add_field_float("inlet_c", 40.5f, 1);
    pb.add_field_float("outlet_c", 45.2f, 1);
  }));

  // Second line has one valid field followed by one NaN field -> must roll back completely
  CHECK_FALSE(pb.build_line(true, [&]() {
    pb.begin_measurement("hyheat", ",unit=2");
    pb.add_field_float("inlet_c", 38.0f, 1);
    pb.add_field_float("outlet_c", nan_val, 1);
    pb.add_field_float("delta_c", 5.0f, 1);
  }));

  // Third line has an ignored dynamic tag -> must roll back completely
  CHECK_FALSE(pb.build_line(true, [&]() {
    pb.begin_measurement("hyheat");
    pb.add_tag("mode", "Unavailable", {"Unavailable"});
    pb.add_field_float("inlet_c", 39.0f, 1);
  }));

  pb.flush();
  REQUIRE(sink.packets.size() == 1);
  CHECK(sink.packets[0] == "hyheat,unit=1 inlet_c=40.5,outlet_c=45.2");
}

TEST_CASE("Bitmask extraction (add_field_bit) extracts individual bits as booleans") {
  CaptureSink sink;
  char raw[DEFAULT_MAX_PACKET_SIZE];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);

  // Bitmask 0b10100101 = 165 (bits 0, 2, 5, 7 set)
  float mask = 165.0f;
  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("orion_flags");
    pb.add_field_bit("bit0", mask, 0);
    pb.add_field_bit("bit1", mask, 1);
    pb.add_field_bit("bit2", mask, 2);
    pb.add_field_bit("bit5", mask, 5);
    pb.add_field_bit("bit6", mask, 6);
    pb.add_field_bit("bit7", mask, 7);
    pb.add_field_bit("invalid_bit", mask, 32);  // out of range -> omitted
  }));
  pb.flush();

  REQUIRE(sink.packets.size() == 1);
  CHECK(sink.packets[0] == "orion_flags bit0=T,bit1=F,bit2=T,bit5=T,bit6=F,bit7=T");
}

TEST_CASE("Line Protocol special character escaping in measurements, tags, and strings") {
  CaptureSink sink;
  char raw[DEFAULT_MAX_PACKET_SIZE];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);

  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement_escaped("my,meas name\\1");
    pb.add_tag("tag,key=1", "val with space,and=equals\\slash");
    pb.add_field_str("msg", "hello \"world\" \\ path");
  }));
  pb.flush();

  REQUIRE(sink.packets.size() == 1);
  CHECK(sink.packets[0] ==
        "my\\,meas\\ name\\\\1,tag\\,key\\=1=val\\ with\\ space\\,and\\=equals\\\\slash "
        "msg=\"hello \\\"world\\\" \\\\ path\"");
}

TEST_CASE("Batching multiple lines and automatic MTU boundary splitting") {
  CaptureSink sink;
  // Use a small 64-byte packet buffer to test multi-line packing and MTU boundary flush
  char raw[64];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);

  // Line 1: "m,id=1 v=10i" (12 bytes)
  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("m", ",id=1");
    pb.add_field_int("v", 10);
  }));
  // Line 2: "\nm,id=2 v=20i" (+13 bytes = 25 bytes)
  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("m", ",id=2");
    pb.add_field_int("v", 20);
  }));
  // Line 3: "\nm,id=3 v=300000000000000000i" (+29 bytes = 54 bytes)
  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("m", ",id=3");
    pb.add_field_int("v", 300000000000000000LL);
  }));
  // Line 4: "\nm,id=4 v=400000000000000000i" (would exceed 64 bytes -> auto-flushes lines 1..3 and starts packet 2!)
  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("m", ",id=4");
    pb.add_field_int("v", 400000000000000000LL);
  }));

  pb.flush();

  REQUIRE(sink.packets.size() == 2);
  CHECK(sink.packets[0] == "m,id=1 v=10i\nm,id=2 v=20i\nm,id=3 v=300000000000000000i");
  CHECK(sink.packets[1] == "m,id=4 v=400000000000000000i");
}

TEST_CASE("Single oversized line exceeding entire buffer capacity is safely dropped") {
  CaptureSink sink;
  char raw[32];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);

  CHECK_FALSE(pb.build_line(false, [&]() {
    pb.begin_measurement("very_long_measurement_name_that_exceeds_32_bytes");
    pb.add_field_int("value", 12345);
  }));
  CHECK(pb.oversized_drops() == 1);
  CHECK(pb.empty());

  // Subsequent normal line still succeeds
  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("ok");
    pb.add_field_int("v", 1);
  }));
  pb.flush();
  REQUIRE(sink.packets.size() == 1);
  CHECK(sink.packets[0] == "ok v=1i");
}

TEST_CASE("Auto-typed lambda return values (add_field_auto and add_tag_auto)") {
  CaptureSink sink;
  char raw[DEFAULT_MAX_PACKET_SIZE];
  PacketBuffer pb(raw, sizeof(raw), &CaptureSink::callback, &sink);

  REQUIRE(pb.build_line(false, [&]() {
    pb.begin_measurement("lambda_test");
    pb.add_tag_auto("t_str", std::string("alpha"));
    pb.add_tag_auto("t_int", 7);
    pb.add_tag_auto("t_bool", true);
    pb.add_field_auto("f_bool", false);
    pb.add_field_auto("f_int", -42);
    pb.add_field_auto("f_uint", 99U);
    pb.add_field_auto("f_float_default", 3.14159f);
    pb.add_field_auto("f_float_fixed", 3.14159f, 2);
    pb.add_field_auto("f_cstr", "hello");
    pb.add_field_auto("f_stdstr", std::string("world"));
    pb.add_field_auto("f_opt_some", std::optional<int>(123));
    pb.add_field_auto("f_opt_none", std::optional<float>());
  }));
  pb.flush();

  REQUIRE(sink.packets.size() == 1);
  CHECK(sink.packets[0] ==
        "lambda_test,t_str=alpha,t_int=7,t_bool=true "
        "f_bool=F,f_int=-42i,f_uint=99i,f_float_default=3.142,f_float_fixed=3.14,"
        "f_cstr=\"hello\",f_stdstr=\"world\",f_opt_some=123i");
}

