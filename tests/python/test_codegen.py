import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "components"))

from esphome.const import (
    CONF_FILTERS,
    CONF_ID,
    CONF_LAMBDA,
    CONF_PLATFORM,
    CONF_SENSOR,
    CONF_UPDATE_INTERVAL,
    KEY_CORE,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    SCHEDULER_DONT_RUN,
)
from esphome.core import CORE, ID, TimePeriodMilliseconds
import esphome.config_validation as cv
import esphome.final_validate as fv
import influxdb_udp


class TestInfluxDBUDPCodegenAndIntervalInference(unittest.TestCase):
    def setUp(self):
        CORE.data[KEY_CORE] = {
            KEY_TARGET_PLATFORM: "esp32",
            KEY_TARGET_FRAMEWORK: "esp-idf",
        }

    def test_escape_helpers(self):
        self.assertEqual(
            influxdb_udp.escape_measurement("my,meas name\\1"),
            "my\\,meas\\ name\\\\1",
        )
        self.assertEqual(
            influxdb_udp.escape_tag("key,with=space and\\slash"),
            "key\\,with\\=space\\ and\\\\slash",
        )

    def test_auto_infer_minimum_interval_across_entities(self):
        s_fast = ID("s_fast")
        s_slow = ID("s_slow")
        fconf = {
            "sensor": [
                {
                    CONF_ID: s_fast,
                    CONF_PLATFORM: "uptime",
                    CONF_UPDATE_INTERVAL: TimePeriodMilliseconds(milliseconds=5000),
                },
                {
                    CONF_ID: s_slow,
                    CONF_PLATFORM: "wifi_signal",
                    CONF_UPDATE_INTERVAL: TimePeriodMilliseconds(milliseconds=15000),
                },
            ]
        }
        fv.full_config.set(fconf)

        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "measurements": [
                    {
                        "measurement": "sys",
                        "fields": {
                            "fast": "s_fast",
                            "slow": "s_slow",
                        },
                    }
                ],
            }
        )
        validated = influxdb_udp.FINAL_VALIDATE_SCHEMA(cfg)
        self.assertEqual(validated["measurements"][0][CONF_UPDATE_INTERVAL], 5000)

    def test_auto_infer_prefers_throttle_filter_over_sensor_interval(self):
        s_throttled = ID("s_throttled")
        fconf = {
            "sensor": [
                {
                    CONF_ID: s_throttled,
                    CONF_PLATFORM: "adc",
                    CONF_UPDATE_INTERVAL: TimePeriodMilliseconds(milliseconds=1000),
                    CONF_FILTERS: [
                        {"throttle_average": TimePeriodMilliseconds(milliseconds=15000)},
                    ],
                }
            ]
        }
        fv.full_config.set(fconf)

        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "measurements": [
                    {
                        "measurement": "adc_meas",
                        "fields": {"val": "s_throttled"},
                    }
                ],
            }
        )
        validated = influxdb_udp.FINAL_VALIDATE_SCHEMA(cfg)
        self.assertEqual(validated["measurements"][0][CONF_UPDATE_INTERVAL], 15000)

    def test_auto_infer_resolves_parent_hub_and_nested_platform_blocks(self):
        hub_id = ID("bms0")
        cell_id = ID("bms0_cell_1")
        fconf = {
            "jbd_bms_ble": [
                {
                    CONF_ID: hub_id,
                    CONF_UPDATE_INTERVAL: TimePeriodMilliseconds(milliseconds=2000),
                }
            ],
            "sensor": [
                {
                    CONF_PLATFORM: "jbd_bms_ble",
                    "jbd_bms_ble_id": hub_id,
                    "cell_voltage_1": {
                        CONF_ID: cell_id,
                    },
                }
            ],
        }
        fv.full_config.set(fconf)

        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "points": [
                    {
                        "measurement": "bms_cell_voltage",
                        "tags": {"bms_id": 0, "cell": 1},
                        "voltage": "bms0_cell_1",
                    }
                ],
            }
        )
        validated = influxdb_udp.FINAL_VALIDATE_SCHEMA(cfg)
        self.assertEqual(validated["points"][0][CONF_UPDATE_INTERVAL], 2000)

    def test_auto_infer_follows_copy_sensor_source(self):
        src_id = ID("src_sensor")
        copy_id = ID("copy_sensor")
        fconf = {
            "sensor": [
                {
                    CONF_ID: src_id,
                    CONF_PLATFORM: "uptime",
                    CONF_UPDATE_INTERVAL: TimePeriodMilliseconds(milliseconds=10000),
                },
                {
                    CONF_ID: copy_id,
                    CONF_PLATFORM: "copy",
                    CONF_SENSOR: src_id,
                },
            ]
        }
        fv.full_config.set(fconf)

        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "measurements": [
                    {
                        "measurement": "copied",
                        "fields": {"v": "copy_sensor"},
                    }
                ],
            }
        )
        validated = influxdb_udp.FINAL_VALIDATE_SCHEMA(cfg)
        self.assertEqual(validated["measurements"][0][CONF_UPDATE_INTERVAL], 10000)

    def test_auto_infer_ignores_push_only_template_default_60s_and_requires_explicit(self):
        push_tpl = ID("push_tpl")
        fconf = {
            "sensor": [
                {
                    CONF_ID: push_tpl,
                    CONF_PLATFORM: "template",
                    # No CONF_LAMBDA -> push-only template sensor, despite default 60s in ESPHome
                    CONF_UPDATE_INTERVAL: TimePeriodMilliseconds(milliseconds=60000),
                }
            ]
        }
        fv.full_config.set(fconf)

        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "measurements": [
                    {
                        "measurement": "push_only",
                        "fields": {"v": "push_tpl"},
                    }
                ],
            }
        )
        with self.assertRaises(cv.Invalid):
            influxdb_udp.FINAL_VALIDATE_SCHEMA(cfg)

    def test_auto_infer_never_when_all_entities_are_never(self):
        s_never = ID("s_never")
        fconf = {
            "sensor": [
                {
                    CONF_ID: s_never,
                    CONF_PLATFORM: "uptime",
                    CONF_UPDATE_INTERVAL: SCHEDULER_DONT_RUN,
                }
            ]
        }
        fv.full_config.set(fconf)

        cfg = influxdb_udp.CONFIG_SCHEMA(
            {
                "host": "192.168.1.50",
                "measurements": [
                    {
                        "measurement": "manual_meas",
                        "fields": {"v": "s_never"},
                    }
                ],
            }
        )
        validated = influxdb_udp.FINAL_VALIDATE_SCHEMA(cfg)
        self.assertEqual(validated["measurements"][0][CONF_UPDATE_INTERVAL], SCHEDULER_DONT_RUN)


if __name__ == "__main__":
    unittest.main()

