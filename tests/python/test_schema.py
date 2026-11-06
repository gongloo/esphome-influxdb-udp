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


if __name__ == "__main__":
    unittest.main()

