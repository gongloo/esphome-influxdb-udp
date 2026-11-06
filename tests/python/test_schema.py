import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "components"))

from esphome.const import KEY_CORE, KEY_TARGET_FRAMEWORK, KEY_TARGET_PLATFORM
from esphome.core import CORE, Lambda
import esphome.config_validation as cv
import influxdb_udp


class TestInfluxDBUDPSchema(unittest.TestCase):
    def setUp(self):
        CORE.data[KEY_CORE] = {
            KEY_TARGET_PLATFORM: "esp32",
            KEY_TARGET_FRAMEWORK: "esp-idf",
        }

    def test_minimal_valid_config(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "measurements": [
                    {
                        "measurement": "system",
                        "fields": {
                            "uptime": "sys_uptime",
                        },
                    }
                ],
            }
        )
        self.assertEqual(cfg["host"], "192.168.1.50")
        self.assertEqual(cfg["port"], 8089)
        self.assertEqual(cfg["max_packet_size"], 1472)
        self.assertEqual(len(cfg["measurements"]), 1)

    def test_points_shorthand_and_explicit_fields(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "points": [
                    {
                        "measurement": "relay",
                        "tags": {"relay": "relay_1", "switch": "galley_fan"},
                        "state": "relay_1_id",
                    },
                    {
                        "measurement": "bms_cell_voltage",
                        "tags": {"bms_id": 0, "cell": 1},
                        "fields": {"voltage": "bms0_cell_1"},
                    },
                ],
            }
        )
        self.assertEqual(len(cfg["points"]), 2)
        self.assertIn("state", cfg["points"][0]["fields"])
        self.assertIn("voltage", cfg["points"][1]["fields"])
        self.assertEqual(cfg["points"][1]["tags"]["bms_id"]["static_value"], "0")

    def test_all_field_options_and_types(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "10.0.0.10",
                "port": 8094,
                "max_packet_size": 1200,
                "tags": {
                    "host": "my_node",
                    "mode": {
                        "text_sensor": "mode_sensor",
                        "ignore_values": ["Unavailable", "Unknown"],
                    },
                },
                "measurements": [
                    {
                        "measurement": "victron_orion",
                        "skip_if_nan": True,
                        "update_interval": "15s",
                        "fields": {
                            "input_voltage": "orion_in_v",
                            "output_voltage": {"sensor": "orion_out_v", "decimals": 3},
                            "uptime": {"sensor": "sys_uptime", "type": "uint"},
                            "lact_bit": {"sensor": "orion_off_reason", "bit": 4},
                            "charging": {"binary_sensor": "orion_charging"},
                            "relay_on": {"switch": "relay_1"},
                            "state_str": {
                                "text_sensor": "orion_state",
                                "ignore_values": ["Unknown"],
                            },
                            "eff": Lambda("return 96.5f;"),
                        },
                    }
                ],
            }
        )
        m = cfg["measurements"][0]
        self.assertTrue(m["skip_if_nan"])
        self.assertEqual(m["fields"]["output_voltage"]["decimals"], 3)
        self.assertEqual(m["fields"]["uptime"]["type"], "uint")
        self.assertEqual(m["fields"]["lact_bit"]["bit"], 4)

    def test_host_entity_without_static_host(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host_entity": {"name": "InfluxDB Host"},
                "port_entity": {"name": "InfluxDB Port"},
                "measurements": [
                    {
                        "measurement": "system",
                        "update_interval": "10s",
                        "fields": {"uptime": "sys_uptime"},
                    }
                ],
            }
        )
        self.assertIn("host_entity", cfg)
        self.assertIn("port_entity", cfg)

    def test_invalid_missing_host_and_host_entity(self):
        with self.assertRaises(cv.Invalid):
            influxdb_udp.CONFIG_SCHEMA(
                {
                    "measurements": [
                        {
                            "measurement": "system",
                            "fields": {"uptime": "sys_uptime"},
                        }
                    ]
                }
            )

    def test_invalid_both_host_entity_and_host_text_id(self):
        with self.assertRaises(cv.Invalid):
            influxdb_udp.CONFIG_SCHEMA(
                {
                    "host_entity": {"name": "InfluxDB Host"},
                    "host_text_id": "existing_text",
                    "measurements": [
                        {
                            "measurement": "system",
                            "fields": {"uptime": "sys_uptime"},
                        }
                    ],
                }
            )

    def test_invalid_bit_with_non_bool_type(self):
        with self.assertRaises(cv.Invalid):
            influxdb_udp.CONFIG_SCHEMA(
                {
                    "host": "192.168.1.50",
                    "measurements": [
                        {
                            "measurement": "flags",
                            "fields": {
                                "b0": {
                                    "sensor": "mask_sensor",
                                    "bit": 0,
                                    "type": "int",
                                }
                            },
                        }
                    ],
                }
            )

    def test_invalid_multiple_source_keys_in_field(self):
        with self.assertRaises(cv.Invalid):
            influxdb_udp.CONFIG_SCHEMA(
                {
                    "host": "192.168.1.50",
                    "measurements": [
                        {
                            "measurement": "bad",
                            "fields": {
                                "f": {
                                    "sensor": "s1",
                                    "binary_sensor": "b1",
                                }
                            },
                        }
                    ],
                }
            )

    def test_global_fields_and_tags(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "tags": {
                    "mode": {"global": "global_mode_tag"},
                    "alt_mode": {"globals": "global_alt_mode_tag"},
                },
                "measurements": [
                    {
                        "measurement": "pid_stats",
                        "update_interval": "10s",
                        "fields": {
                            "p_term": "p_term_global",
                            "d_term": {"global": "d_term_global", "decimals": 4},
                            "ff_term": {"globals": "ff_term_global", "type": "float"},
                            "counter": {"global": "counter_global", "type": "uint"},
                            "flag": {"global": "flag_global", "type": "bool"},
                            "bit_val": {"global": "mask_global", "bit": 2},
                        },
                    }
                ],
            }
        )
        m = cfg["measurements"][0]
        fields = m["fields"]
        self.assertEqual(fields["p_term"]["entity"].id, "p_term_global")
        self.assertEqual(fields["d_term"]["global"].id, "d_term_global")
        self.assertEqual(fields["d_term"]["decimals"], 4)
        self.assertEqual(fields["ff_term"]["global"].id, "ff_term_global")
        self.assertEqual(fields["ff_term"]["type"], "float")
        self.assertEqual(fields["counter"]["type"], "uint")
        self.assertEqual(fields["flag"]["type"], "bool")
        self.assertEqual(fields["bit_val"]["bit"], 2)
        self.assertEqual(cfg["tags"]["mode"]["global"].id, "global_mode_tag")
        self.assertEqual(cfg["tags"]["alt_mode"]["global"].id, "global_alt_mode_tag")

    def test_invalid_multiple_sources_with_global(self):
        with self.assertRaises(cv.Invalid):
            influxdb_udp.CONFIG_SCHEMA(
                {
                    "host": "192.168.1.50",
                    "measurements": [
                        {
                            "measurement": "bad",
                            "fields": {
                                "f": {
                                    "global": "g1",
                                    "sensor": "s1",
                                }
                            },
                        }
                    ],
                }
            )

    def test_publish_all_boolean(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "publish_all": True,
            }
        )
        self.assertIn("publish_all", cfg)
        p = cfg["publish_all"]
        self.assertFalse(p["include_internal"])
        self.assertTrue(p["include_sensors"])
        self.assertTrue(p["include_binary_sensors"])
        self.assertTrue(p["include_text_sensors"])
        self.assertFalse(p["include_switches"])
        self.assertEqual(p["send_timeout"].total_milliseconds, 100)

    def test_publish_all_dictionary(self):
        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "publish_all": {
                    "include_internal": True,
                    "include_switches": True,
                    "send_timeout": "50ms",
                },
            }
        )
        p = cfg["publish_all"]
        self.assertTrue(p["include_internal"])
        self.assertTrue(p["include_switches"])
        self.assertEqual(p["send_timeout"].total_milliseconds, 50)

    def test_publish_all_disabled_fails_if_no_measurements(self):
        with self.assertRaises(cv.Invalid):
            influxdb_udp.CONFIG_SCHEMA(
                {
                    "host": "192.168.1.50",
                    "publish_all": False,
                }
            )


if __name__ == "__main__":
    unittest.main()

