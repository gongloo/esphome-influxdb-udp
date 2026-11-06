# ESPHome InfluxDB UDP Component (`influxdb_udp`)

[![CI](https://github.com/gongloo/esphome-influxdb-udp/actions/workflows/ci.yml/badge.svg)](https://github.com/gongloo/esphome-influxdb-udp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A high-performance, zero-allocation **ESPHome external component** that serializes and transmits telemetry from ESPHome entities (`sensor`, `binary_sensor`, `switch`, `text_sensor`, `number`, `select`), ESPHome global variables (`globals`), and C++ lambdas over UDP using the [InfluxDB Line Protocol](https://docs.influxdata.com/influxdb/v2/reference/syntax/line-protocol/).

Designed for resource-constrained ESP32/ESP8266 nodes with dozens or hundreds of sensors (such as multi-pack BMS arrays, solar MPPT controllers, DC-DC converters, HVAC controllers, and relay banks), `influxdb_udp` replaces hundreds of lines of repetitive `interval:` C++ lambdas and `snprintf` boilerplate with a concise, declarative YAML schema that compiles directly into flat C++ statements.

---

## Major Features

- **Declarative YAML, Compiled to Flat C++:** Configure measurements, tags, and fields declaratively in YAML. At compile time, ESPHome generates direct C++ serialization calls with **zero** `std::function` wrappers, **zero** dynamic descriptor tables, and **zero** per-update heap allocations.
- **Single Shared MTU Packet Buffer with Auto-Batching:** Uses a single pre-allocated buffer (default `1472` bytes to fit within a standard 1500-byte Ethernet/Wi-Fi MTU without IP fragmentation). Measurements sharing the same `update_interval` are automatically packed `\n`-delimited into a single UDP datagram and automatically split on line boundaries if a batch exceeds `max_packet_size`.
- **Automatic `update_interval` Inference:** When `update_interval:` is omitted on a measurement or point, `influxdb_udp` automatically inspects all referenced entities—checking `throttle` / `throttle_average` sensor filters, `copy` sensor sources, direct entity polling intervals, and parent hub intervals (`modbus_controller`, `jbd_bms_ble`, `ina226`, etc.)—and selects the fastest (minimum) finite update interval.
- **First-Class `NaN` & Offline Handling:** Automatically omits individual fields or dynamic tags whose underlying entity is `NaN`, `!has_state()`, empty, or matches an `ignore_values` filter (e.g., `"Unavailable"`, `"Unknown"`). If a measurement has zero valid fields left, the entire line is cleanly rolled back in place without sending an invalid packet.
- **All-or-Nothing Measurement Mode (`skip_if_nan`):** Optional `skip_if_nan: true` per measurement or point aborts and rolls back the entire line if *any* referenced tag or field is `NaN` or unavailable.
- **Precision-Aware Float Formatting:** Floating-point sensors automatically format using their configured ESPHome `accuracy_decimals` (`%.*f`), with an optional per-field `decimals:` override when you want higher or lower precision in InfluxDB than in Home Assistant.
- **Bitmask Flag Extraction (`bit: 0..31`):** Extract individual boolean flags (`T`/`F`) directly from numeric bitmask sensors or lambdas without creating intermediate template binary sensors—automatically skipping the flag if the bitmask sensor itself is `NaN`/offline.
- **Live Runtime Host & Port Configuration (No Reboot Required):** Built directly on `esphome::socket`, allowing the destination IP address and UDP port to be changed **live at runtime** via auto-created NVS-backed `host_entity:` (`text`) and `port_entity:` (`number`) configuration entities (or linked `host_text_id:` / `port_number_id:` entities) without restarting the ESP node.
- **Event-Driven Publishing (`influxdb_udp.publish`):** Trigger any measurement or grouped point batch immediately from any ESPHome automation (`on_turn_on`, `on_value`, `on_boot`, etc.) in addition to—or instead of (`update_interval: never`)—periodic polling.

---

## Quick-Start Guide

### 1. Add the External Component

```yaml
external_components:
  - source: github://gongloo/esphome-influxdb-udp@main
    components: [influxdb_udp]
```

### 2. Configure Your Measurements

```yaml
sensor:
  - platform: uptime
    id: sys_uptime
    update_interval: 15s

  - platform: wifi_signal
    id: wifi_rssi
    update_interval: 15s

  - platform: template
    id: battery_voltage
    accuracy_decimals: 2
    update_interval: 5s
    lambda: return 13.84f;

  - platform: template
    id: battery_current
    accuracy_decimals: 1
    update_interval: 5s
    lambda: return -18.5f;

  - platform: template
    id: fault_bitmask
    accuracy_decimals: 0
    update_interval: 5s
    lambda: return 4.0f; # Bit 2 set

binary_sensor:
  - platform: template
    id: contactor_closed
    lambda: return true;

text_sensor:
  - platform: template
    id: charge_state
    update_interval: 5s
    lambda: return {"Bulk"};

influxdb_udp:
  host: "192.168.1.50"
  port: 8089

  # Optional: expose NVS-backed runtime config entities in Home Assistant
  # Changes apply immediately without rebooting!
  host_entity:
    name: "InfluxDB Host"
  port_entity:
    name: "InfluxDB Port"

  # Global tags attached to every measurement & point
  tags:
    host: my-esp32-node

  measurements:
    # Auto-infers update_interval: 15s from sys_uptime and wifi_rssi
    - measurement: system
      fields:
        uptime:
          sensor: sys_uptime
          type: uint
        rssi: wifi_rssi
        free_heap:
          type: uint
          lambda: return esp_get_free_heap_size();

    # Auto-infers update_interval: 5s from battery_voltage / battery_current
    - measurement: battery_bank
      tags:
        bank: house
        state:
          text_sensor: charge_state
          ignore_values: ["Unavailable", "Unknown"]
      fields:
        voltage: battery_voltage
        current: battery_current
        power:
          decimals: 1
          lambda: return id(battery_voltage).state * id(battery_current).state;
        contactor: contactor_closed
        overtemp_fault:
          sensor: fault_bitmask
          bit: 2
        undervolt_fault:
          sensor: fault_bitmask
          bit: 0
```

### 3. Expected UDP Line Protocol Output

Every **5 seconds**, `influxdb_udp` sends a single UDP datagram containing:

```text
battery_bank,host=my-esp32-node,bank=house,state=Bulk voltage=13.84,current=-18.5,power=-256.0,contactor=T,overtemp_fault=T,undervolt_fault=F
```

Every **15 seconds**, `influxdb_udp` sends a single UDP datagram containing:

```text
system,host=my-esp32-node uptime=45i,rssi=-62.0,free_heap=184320i
```

If multiple measurements share the `5s` or `15s` interval, they are packed together into the same UDP packet separated by `\n`:

```text
measurement_a,host=my-esp32-node field1=1.23,field2=45i
measurement_b,host=my-esp32-node status=T,temp=24.5
```

---

## Detailed Feature & Behavior Documentation

### 1. Entity Shorthand & Supported Entity Types

Any field in `fields:` (or inline in `points:`) can reference an ESPHome entity either by **bare ID shorthand** or by an **explicit dictionary**:

```yaml
fields:
  # Bare entity / global ID shorthand — C++ overload resolution automatically detects whether
  # the ID is a sensor, binary_sensor, switch, text_sensor, number, select, or global variable!
  input_voltage: orion_input_voltage
  charging: bms0_charging
  relay_state: galley_fan_switch
  operating_mode: mppt_charge_mode
  p_term: p_term_global

  # Explicit dictionary syntax when specifying formatting or type overrides:
  output_voltage:
    sensor: orion_output_voltage
    decimals: 3
  d_term:
    global: d_term_global
    decimals: 4
```

| ESPHome Entity Type | Default Line Protocol Field Type | Default Formatting Behavior | Unavailable / Skip Condition |
| :--- | :--- | :--- | :--- |
| `sensor` | Float (`13.84`) | Uses `sensor->get_accuracy_decimals()` (`%.*f`), or `decimals:` override | `!s->has_state()` or `std::isnan(s->state)` or `std::isinf(s->state)` |
| `binary_sensor` | Boolean (`T` / `F`) | `T` when true, `F` when false (or `1i`/`0i` if `type: int`) | `!b->has_state()` |
| `switch` | Boolean (`T` / `F`) | `T` when ON, `F` when OFF (or `1i`/`0i` if `type: int`) | `sw == nullptr` |
| `text_sensor` | String (`"Bulk"`) | Quoted & escaped string (`\"`, `\\`) | `!ts->has_state()`, empty `""`, or in `ignore_values` |
| `number` | Float (`80.0`) | `%.4g` (or `%.*f` if `decimals:` is set; `123i` if `type: int`) | `!n->has_state()` or `std::isnan(n->state)` |
| `select` | String (`"Eco"`) | Quoted & escaped `current_option()` | `!sel->has_state()`, empty `""`, or in `ignore_values` |
| `globals` | Auto-deduced from stored value | `bool` → `T`/`F`; signed int → `%lldi`; unsigned int → `%llui`; `float`/`double` → `%.4g` (or `%.*f` with `decimals:`); `std::string` → `"..."` | `g == nullptr`, `NaN`/`Inf` for floats, or in `ignore_values` |
| `!lambda` | Auto-deduced from C++ return type | `bool` → `T`/`F`; signed int → `%lldi`; unsigned int → `%llui`; `float`/`double` → `%.4g` (or `%.*f` with `decimals:`); `const char*`/`std::string` → `"..."`; `std::optional<T>` → unwraps or skips if `std::nullopt` | `NaN`/`Inf` for floats, empty/`ignore_values` for strings, `std::nullopt` for `std::optional` |

### 2. Field Type Overrides (`type:`) and Bitmask Extraction (`bit:`)

You can override how numeric or boolean entities and lambdas are serialized in Line Protocol:

- `type: float` — Formats as a Line Protocol float (`12.34`). Uses `decimals:` if specified, else `sensor->get_accuracy_decimals()` for sensors, or `%.4g` for numbers/lambdas.
- `type: int` — Rounds float sensors/numbers (`std::llround`) or casts integer lambdas to signed 64-bit integer with the InfluxDB `i` suffix (`42i`, `-10i`). Automatically skips if a float source is `NaN`/`Inf`.
- `type: uint` — Rounds float sensors/numbers or casts lambdas to unsigned 64-bit integer with the InfluxDB `i` suffix (`3600i`). Skips if a float source is `NaN`, `Inf`, or negative (`< 0.0f`). Set `int_suffix: false` if your downstream consumer expects bare unsigned integers without trailing `i`.
- `type: bool` — Emits `T` or `F`. For numeric sensors, `state != 0.0f` is `T` and `0.0f` is `F` (skipped if `NaN`).
- `type: string` — Emits a double-quoted Line Protocol string (`"value"`).
- `bit: <0..31>` — Casts a non-`NaN` float sensor or lambda return value to `uint32_t`, tests `(mask & (1UL << bit)) != 0`, and emits `T` or `F`. If the source sensor is `NaN` or `!has_state()`, the bit field is automatically omitted.

### 3. Automatic `update_interval` Inference

When `update_interval:` is omitted on a entry in `measurements:` or `points:`, `influxdb_udp` inspects every entity referenced in that entry's `fields:` and dynamic `tags:` during ESPHome's `FINAL_VALIDATE_SCHEMA` pass.

For each referenced entity, its effective interval is resolved using the following priority order:

1. **Sensor Throttle Filters:** If the entity defines `filters:` containing `throttle: <time>` or `throttle_average: <time>`, the period of the last throttle filter in the chain is used (since that determines how often the sensor's output state updates).
2. **`copy` Platform Delegation:** If the entity is a `platform: copy` sensor, resolution recursively follows its source `sensor:` ID.
3. **Direct Entity / Platform Interval:** If the entity or its enclosing platform block defines `throttle:` or `update_interval:`, that interval is used.
   - *Push-only Template Nuance:* In ESPHome, `platform: template` sensors and text sensors inherit a schema default of `update_interval: 60s` even when no `lambda:` is defined (i.e. when they are pushed to via `sensor.template.publish`). `influxdb_udp` specifically detects `platform: template` without `lambda:` and ignores the dummy `60s` default so push-only templates do not skew interval inference.
4. **Parent Hub Interval:** If the entity or its enclosing platform block references a parent hub via a `*_id` key (such as `jbd_bms_ble_id`, `modbus_controller_id`, `sungoldpower_lfp_id`, `ina226_id`, etc.), `influxdb_udp` looks up the hub component and uses its `throttle` or `update_interval`.

Once all referenced entities in a measurement are inspected:
- If one or more **finite** intervals are found, the **minimum (fastest)** interval is selected.
- If all referenced entities with intervals are explicitly set to `update_interval: never`, the measurement defaults to `never` (manual publishing via `- influxdb_udp.publish: <id>`).
- If **no** referenced entity defines a polling interval (for example, a measurement containing only `switch` entities, push-only `template` sensors, or raw C++ lambdas), ESPHome validation raises a clear error asking you to set `update_interval:` explicitly (e.g. `update_interval: 5s` or `update_interval: never`).

### 4. `NaN`, Offline, and `skip_if_nan` Behavior

Hardware sensors frequently go offline (e.g., a BLE BMS out of range, an RS485 inverter powered down, or a sensor warming up after boot). `influxdb_udp` provides two clean modes for handling unavailable data without writing `if (!std::isnan(...))` checks:

1. **Default Mode (`skip_if_nan: false`):**
   - Any individual field or dynamic tag whose value is `NaN`, `Inf`, `!has_state()`, `""` (empty string), or listed in `ignore_values` is **omitted** from the current line.
   - Other valid fields on the same measurement line are still emitted.
   - If **zero** valid fields remain on the line (for example, the entire device is offline), the entire line—including its measurement name, tags, and leading `\n` separator—is rolled back in place in the packet buffer so no malformed or empty line is ever transmitted.
2. **Strict Mode (`skip_if_nan: true`):**
   - If **any** dynamic tag or field in the measurement is `NaN`, `Inf`, `!has_state()`, empty, or in `ignore_values`, the **entire measurement line** is immediately aborted and rolled back in the packet buffer.
   - Useful for tightly coupled state vectors (such as heat-pump inlet/outlet temperatures or 3-phase power calculations) where partial records are undesirable.

### 5. Single-Buffer Batching & MTU Splitting

To minimize RAM usage, avoid heap fragmentation, and reduce lwIP UDP socket pressure:
- Each `InfluxDBUDP` component owns a **single pre-allocated buffer** (`1472` bytes by default, matching the maximum unfragmented UDP payload over a 1500-byte Ethernet/Wi-Fi MTU: `1500 - 20 (IPv4) - 8 (UDP) = 1472`).
- All measurements and points that resolve to the **same finite `update_interval`** are compiled into a single periodic callback.
- As each measurement line is serialized into the buffer:
  - If the line fits in the remaining buffer space, it is appended after a `\n` delimiter.
  - If appending the line would exceed `max_packet_size`, `PacketBuffer` automatically rolls back the partial line, **flushes the already-committed lines** as a UDP datagram, resets the buffer to offset `0`, and re-serializes the new line at the start of the next packet.
  - At the end of the batch callback, any remaining buffered lines are flushed in a single `sendto()` call.

### 6. Live Runtime Host & Port Configuration

Unlike ESPHome's built-in `udp` component (which binds fixed destination addresses at `setup()` and requires a full node reboot to apply changes), `influxdb_udp` manages its own non-blocking `esphome::socket` and `sockaddr_storage`.

You can expose live configuration entities in Home Assistant in either of two ways:

#### Option A: Auto-Created NVS-Backed Entities (`host_entity:` and `port_entity:`)

```yaml
influxdb_udp:
  host: "192.168.1.50"   # Used as default initial_value if NVS is empty
  port: 8089             # Used as default initial_value if NVS is empty
  host_entity:
    name: "InfluxDB Host"
    restore_value: true  # Persists to ESP flash (NVS) across reboots
  port_entity:
    name: "InfluxDB Port"
    restore_value: true
```

#### Option B: Link Existing `text` / `number` Entities (`host_text_id:` and `port_number_id:`)

```yaml
influxdb_udp:
  host_text_id: my_custom_influx_host_text
  port_number_id: my_custom_influx_port_number
```

Whenever the host or port changes at runtime, `influxdb_udp` immediately re-runs `socket::set_sockaddr()` and begins sending subsequent UDP packets to the new destination without rebooting.

### 7. Single-Field Points (`points:`) and Event-Driven Publishing (`influxdb_udp.publish`)

When you have many entities that each emit a single field under the same measurement name with different tags (such as 16 BMS cell voltages or 8 relay states), you can use `points:` either at the top level or grouped inside a `measurements:` entry:

```yaml
switch:
  - platform: gpio
    id: relay_1
    pin: GPIO4
    on_turn_on:
      - influxdb_udp.publish: relay_states
    on_turn_off:
      - influxdb_udp.publish: relay_states

influxdb_udp:
  host: "192.168.1.50"
  tags:
    host: relay-node

  measurements:
    # A named group of points that is both polled every 5s AND triggered immediately
    # via `- influxdb_udp.publish: relay_states` whenever a relay toggles!
    - id: relay_states
      measurement: relay
      update_interval: 5s
      points:
        - tags: { relay: relay_1, load: galley_fan }
          state: relay_1
        - tags: { relay: relay_2, load: water_pump }
          state: relay_2

  # Top-level points (update_interval auto-inferred per point if omitted)
  points:
    - measurement: bms_cell_voltage
      tags: { bms_id: 0, cell: 1 }
      voltage: bms0_cell_voltage_1
    - measurement: bms_cell_voltage
      tags: { bms_id: 0, cell: 2 }
      voltage: bms0_cell_voltage_2
```

Produces:

```text
relay,host=relay-node,relay=relay_1,load=galley_fan state=T
relay,host=relay-node,relay=relay_2,load=water_pump state=F
bms_cell_voltage,host=relay-node,bms_id=0,cell=1 voltage=3.342
bms_cell_voltage,host=relay-node,bms_id=0,cell=2 voltage=3.345
```

---

## Configuration Reference

### Top-Level `influxdb_udp:` Options

| Key | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `id` | ID | Auto-generated | Optional ID for this `InfluxDBUDP` component instance (supports `MULTI_CONF`). |
| `host` | `string` | *(Optional)* | Default destination IPv4/IPv6 address for UDP Line Protocol packets. Required unless `host_entity` or `host_text_id` is configured. |
| `port` | `port` (`1..65535`) | `8089` | Default destination UDP port. |
| `max_packet_size` | `int` (`64..65507`) | `1472` | Maximum UDP payload size in bytes before auto-flushing on a `\n` line boundary. |
| `tags` | `map` | `{}` | Global tags applied to every measurement and point in this instance. Keys are tag names; values can be static literals (`string`, `int`, `float`, `bool`), `!lambda`, or dynamic entity dicts. |
| `host_entity` | `text` schema | *(Optional)* | Auto-creates a Home Assistant `text` config entity (`ENTITY_CATEGORY_CONFIG`) to view and change the destination IP at runtime. Accepts all standard ESPHome `text` options plus `initial_value` and `restore_value` (default `true`). |
| `port_entity` | `number` schema | *(Optional)* | Auto-creates a Home Assistant `number` config entity (`ENTITY_CATEGORY_CONFIG`) to view and change the destination UDP port at runtime. Accepts all standard ESPHome `number` options plus `initial_value`, `min_value` (`1`), `max_value` (`65535`), `step` (`1`), `mode` (`BOX`), and `restore_value` (default `true`). |
| `host_text_id` | `ID<text::Text>` | *(Optional)* | Links an existing ESPHome `text` entity to update the destination host on state change. Mutually exclusive with `host_entity`. |
| `port_number_id` | `ID<number::Number>` | *(Optional)* | Links an existing ESPHome `number` entity to update the destination port on state change. Mutually exclusive with `port_entity`. |
| `measurements` | `list` | `[]` | List of measurement or point-group definitions. |
| `points` | `list` | `[]` | List of top-level point definitions. |

### `measurements:` Item Options

| Key | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `id` | ID | Auto-generated | Optional ID (`BatchTarget`) that can be triggered via `- influxdb_udp.publish: <id>`. |
| `measurement` | `string` | *(Required if `fields:` is set)* | InfluxDB measurement name (automatically escaped at compile time). Also serves as the default measurement name for any nested `points:`. |
| `tags` | `map` | `{}` | Tags merged with global `tags:` (per-measurement keys override global keys). |
| `fields` | `map` | *(Optional)* | Map of `<field_name>: <field_spec>`. |
| `points` | `list` | *(Optional)* | List of points belonging to this group (inherits group `measurement`, `tags`, `update_interval`, and `skip_if_nan` unless overridden per point). |
| `update_interval` | `time` or `never` | Auto-inferred | Polling interval for this measurement/group. Automatically inferred from referenced entities if omitted. |
| `skip_if_nan` | `boolean` | `false` | When `true`, drops the entire measurement line if *any* dynamic tag or field is `NaN` or unavailable. |

### Field Specification (`<field_spec>`) Options

Each entry under `fields:` (or inline field key on a point) can be a **bare entity ID**, a **bare `!lambda`**, or a **dictionary** with:

| Key | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `entity` | `ID` | *(One source required)* | Any ESPHome entity (`sensor`, `binary_sensor`, `switch`, `text_sensor`, `number`, `select`) or `globals` ID. |
| `global` | `ID<GlobalsComponent>` | — | Explicit ESPHome `globals` ID (also accepts `globals:`). |
| `sensor` | `ID<Sensor>` | — | Explicit `sensor` ID. |
| `binary_sensor` | `ID<BinarySensor>` | — | Explicit `binary_sensor` ID. |
| `switch` | `ID<Switch>` | — | Explicit `switch` ID. |
| `text_sensor` | `ID<TextSensor>` | — | Explicit `text_sensor` ID. |
| `number` | `ID<Number>` | — | Explicit `number` ID. |
| `select` | `ID<Select>` | — | Explicit `select` ID. |
| `lambda` | `lambda` | — | C++ lambda returning `float`, `double`, signed/unsigned integer, `bool`, `const char*`, `std::string`, or `std::optional<T>`. |
| `type` | `enum` | Entity default | Optional type override: `float`, `int`, `uint`, `bool`, or `string`. |
| `decimals` | `int` (`0..15`) | `accuracy_decimals` | Optional decimal precision override (`%.*f`) for floating-point fields. |
| `bit` | `int` (`0..31`) | *(Optional)* | Extracts bit `0..31` from a numeric bitmask sensor or lambda as a boolean (`T`/`F`) field. |
| `ignore_values` | `list[string]` | `[]` | For string/text/select fields, list of string values (e.g. `["Unavailable", "Unknown"]`) to treat as unavailable and omit. |
| `int_suffix` | `boolean` | `true` | When `type: uint`, whether to append the InfluxDB integer suffix `i`. |

---

## Receiving Data in Telegraf / InfluxDB

To ingest UDP Line Protocol packets with [Telegraf](https://docs.influxdata.com/telegraf/v1/plugins/#inputs-socket_listener), enable the `socket_listener` input plugin:

```toml
[[inputs.socket_listener]]
  service_address = "udp://:8089"
  data_format = "influx"
  read_buffer_size = "64KiB"
```

---

## Running Tests Locally

```bash
# Run C++ unit tests (with AddressSanitizer & UndefinedBehaviorSanitizer)
make -C tests/cpp run

# Run Python schema & interval-inference unit tests
pytest tests/python -v

# Validate and compile ESPHome test fixtures
esphome config tests/fixtures/test_minimal.yaml
esphome compile tests/fixtures/test_full.yaml
```

---

## Acknowledgements

- Designed and built by **Hasan Khalil** ([@gongloo](https://github.com/gongloo)) in collaboration with **Antigravity** (Google DeepMind).

## License

This project is licensed under the [MIT License](LICENSE).

