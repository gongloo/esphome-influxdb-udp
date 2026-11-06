from collections import OrderedDict
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import binary_sensor, number, select, sensor, switch, text, text_sensor
from esphome.const import (
    CONF_ENTITY_CATEGORY,
    CONF_FILTERS,
    CONF_ICON,
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_LAMBDA,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_MODE,
    CONF_PLATFORM,
    CONF_PORT,
    CONF_RESTORE_VALUE,
    CONF_SENSOR,
    CONF_STEP,
    CONF_SWITCH,
    CONF_UPDATE_INTERVAL,
    ENTITY_CATEGORY_CONFIG,
    SCHEDULER_DONT_RUN,
)
import esphome.final_validate as fv

CODEOWNERS = ["@gongloo"]
DEPENDENCIES = ["network"]
MULTI_CONF = True


def AUTO_LOAD(config=None):
    loads = ["socket"]
    if config is not None:
        entries = config if isinstance(config, list) else [config]
        for entry in entries:
            if isinstance(entry, dict):
                if "host_entity" in entry and "text" not in loads:
                    loads.append("text")
                if "port_entity" in entry and "number" not in loads:
                    loads.append("number")
    return loads

CONF_HOST = "host"
CONF_MAX_PACKET_SIZE = "max_packet_size"
CONF_TAGS = "tags"
CONF_HOST_ENTITY = "host_entity"
CONF_PORT_ENTITY = "port_entity"
CONF_HOST_TEXT_ID = "host_text_id"
CONF_PORT_NUMBER_ID = "port_number_id"
CONF_MEASUREMENTS = "measurements"
CONF_POINTS = "points"
CONF_MEASUREMENT = "measurement"
CONF_FIELDS = "fields"
CONF_SKIP_IF_NAN = "skip_if_nan"

CONF_ENTITY = "entity"
CONF_BINARY_SENSOR = "binary_sensor"
CONF_TEXT_SENSOR = "text_sensor"
CONF_NUMBER = "number"
CONF_SELECT = "select"
CONF_TYPE = "type"
CONF_DECIMALS = "decimals"
CONF_BIT = "bit"
CONF_IGNORE_VALUES = "ignore_values"
CONF_INT_SUFFIX = "int_suffix"
CONF_STATIC_VALUE = "static_value"

ENTITY_SOURCE_KEYS = (
    CONF_ENTITY,
    CONF_SENSOR,
    CONF_BINARY_SENSOR,
    CONF_SWITCH,
    CONF_TEXT_SENSOR,
    CONF_NUMBER,
    CONF_SELECT,
)
ALL_SOURCE_KEYS = ENTITY_SOURCE_KEYS + (CONF_LAMBDA,)

FIELD_TYPES = ("float", "int", "uint", "bool", "string")

influxdb_udp_ns = cg.esphome_ns.namespace("influxdb_udp")
InfluxDBUDP = influxdb_udp_ns.class_("InfluxDBUDP", cg.Component)
BatchTarget = influxdb_udp_ns.class_("BatchTarget")
InfluxDBHostText = influxdb_udp_ns.class_(
    "InfluxDBHostText", text.Text, cg.Component, cg.Parented.template(InfluxDBUDP)
)
InfluxDBPortNumber = influxdb_udp_ns.class_(
    "InfluxDBPortNumber", number.Number, cg.Component, cg.Parented.template(InfluxDBUDP)
)
PublishAction = influxdb_udp_ns.class_("PublishAction", automation.Action)


def escape_measurement(val: str) -> str:
    """Escape measurement name per InfluxDB Line Protocol rules (escape backslash, comma, space)."""
    out = []
    for ch in str(val):
        if ch in ("\\", ",", " "):
            out.append("\\")
        out.append(ch)
    return "".join(out)


def escape_tag(val: str) -> str:
    """Escape tag key or tag value per InfluxDB Line Protocol rules (escape backslash, comma, equals, space)."""
    out = []
    for ch in str(val):
        if ch in ("\\", ",", "=", " "):
            out.append("\\")
        out.append(ch)
    return "".join(out)


def _validate_tag_or_field_key(value):
    value = cv.string_strict(value)
    if not value:
        raise cv.Invalid("Key cannot be empty")
    return value


def _normalize_field_spec(value):
    """Allow shorthand `<field>: <entity_id>`, `<field>: !lambda ...`, or full dictionary."""
    if isinstance(value, cv.Lambda):
        return {CONF_LAMBDA: value}
    if isinstance(value, dict):
        return value
    return {CONF_ENTITY: value}


def _validate_single_source(config):
    present = [k for k in ALL_SOURCE_KEYS if k in config]
    if len(present) == 0:
        raise cv.Invalid(
            f"Must specify one source key ({', '.join(ALL_SOURCE_KEYS)})"
        )
    if len(present) > 1:
        raise cv.Invalid(
            f"Cannot specify multiple source keys ({', '.join(present)}); choose only one"
        )
    if CONF_BIT in config and CONF_TYPE in config and config[CONF_TYPE] != "bool":
        raise cv.Invalid("'bit' extraction produces a boolean field and cannot be combined with non-bool 'type'")
    return config


FIELD_SPEC_SCHEMA = cv.All(
    _normalize_field_spec,
    cv.Schema(
        {
            cv.Optional(CONF_ENTITY): cv.use_id(cg.EntityBase),
            cv.Optional(CONF_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_BINARY_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_TEXT_SENSOR): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_NUMBER): cv.use_id(number.Number),
            cv.Optional(CONF_SELECT): cv.use_id(select.Select),
            cv.Optional(CONF_LAMBDA): cv.returning_lambda,
            cv.Optional(CONF_TYPE): cv.one_of(*FIELD_TYPES, lower=True),
            cv.Optional(CONF_DECIMALS): cv.int_range(min=0, max=15),
            cv.Optional(CONF_BIT): cv.int_range(min=0, max=31),
            cv.Optional(CONF_IGNORE_VALUES, default=[]): cv.ensure_list(cv.string),
            cv.Optional(CONF_INT_SUFFIX, default=True): cv.boolean,
        }
    ),
    _validate_single_source,
)


def _normalize_tag_spec(value):
    """Allow static string/number/bool tag values, `!lambda`, or dynamic entity/lambda dict."""
    if isinstance(value, cv.Lambda):
        return {CONF_LAMBDA: value}
    if isinstance(value, dict):
        return value
    if isinstance(value, bool):
        return {CONF_STATIC_VALUE: "true" if value else "false"}
    if isinstance(value, (int, float, str)):
        s = str(value)
        if not s:
            raise cv.Invalid("Static tag value cannot be empty")
        return {CONF_STATIC_VALUE: s}
    raise cv.Invalid(f"Invalid tag value {value!r}; expected static value, !lambda, or entity dictionary")


def _validate_tag_spec(config):
    if CONF_STATIC_VALUE in config:
        if any(k in config for k in ALL_SOURCE_KEYS):
            raise cv.Invalid("Cannot combine static tag value with dynamic entity/lambda source")
        return config
    return _validate_single_source(config)


TAG_SPEC_SCHEMA = cv.All(
    _normalize_tag_spec,
    cv.Schema(
        {
            cv.Optional(CONF_STATIC_VALUE): cv.string_strict,
            cv.Optional(CONF_ENTITY): cv.use_id(cg.EntityBase),
            cv.Optional(CONF_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_BINARY_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_TEXT_SENSOR): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_NUMBER): cv.use_id(number.Number),
            cv.Optional(CONF_SELECT): cv.use_id(select.Select),
            cv.Optional(CONF_LAMBDA): cv.returning_lambda,
            cv.Optional(CONF_DECIMALS): cv.int_range(min=0, max=15),
            cv.Optional(CONF_IGNORE_VALUES, default=[]): cv.ensure_list(cv.string),
        }
    ),
    _validate_tag_spec,
)

TAGS_MAP_SCHEMA = cv.Schema({_validate_tag_or_field_key: TAG_SPEC_SCHEMA})
FIELDS_MAP_SCHEMA = cv.Schema({_validate_tag_or_field_key: FIELD_SPEC_SCHEMA})

RESERVED_POINT_KEYS = {
    CONF_ID,
    CONF_MEASUREMENT,
    CONF_TAGS,
    CONF_FIELDS,
    CONF_UPDATE_INTERVAL,
    CONF_SKIP_IF_NAN,
}


def _normalize_point_item(value):
    """Allow points to specify fields either under `fields:` or inline as top-level keys on the point dict."""
    if not isinstance(value, dict):
        raise cv.Invalid("Point entry must be a dictionary")
    value = dict(value)
    inline_fields = OrderedDict()
    for k in list(value.keys()):
        if k not in RESERVED_POINT_KEYS:
            inline_fields[k] = value.pop(k)
    if inline_fields:
        existing = OrderedDict(value.get(CONF_FIELDS, {}))
        existing.update(inline_fields)
        value[CONF_FIELDS] = existing
    if not value.get(CONF_FIELDS):
        raise cv.Invalid("Point entry must define at least one field (inline or under 'fields:')")
    return value


POINT_ITEM_SCHEMA = cv.All(
    _normalize_point_item,
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(BatchTarget),
            cv.Optional(CONF_MEASUREMENT): _validate_tag_or_field_key,
            cv.Optional(CONF_TAGS, default={}): TAGS_MAP_SCHEMA,
            cv.Required(CONF_FIELDS): FIELDS_MAP_SCHEMA,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
            cv.Optional(CONF_SKIP_IF_NAN): cv.boolean,
        }
    ),
)


def _validate_measurement_entry(config):
    has_fields = bool(config.get(CONF_FIELDS))
    has_points = bool(config.get(CONF_POINTS))
    if not has_fields and not has_points:
        raise cv.Invalid("Each measurement entry must define 'fields:' and/or 'points:'")
    if has_fields and CONF_MEASUREMENT not in config:
        raise cv.Invalid("Measurement entry with 'fields:' requires 'measurement:' name")
    if has_points:
        for idx, pt in enumerate(config[CONF_POINTS]):
            if CONF_MEASUREMENT not in pt and CONF_MEASUREMENT not in config:
                raise cv.Invalid(
                    f"Point #{idx + 1} in measurement group is missing 'measurement:' "
                    "(and no default 'measurement:' is set on the group)"
                )
    return config


MEASUREMENT_ENTRY_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(BatchTarget),
            cv.Optional(CONF_MEASUREMENT): _validate_tag_or_field_key,
            cv.Optional(CONF_TAGS, default={}): TAGS_MAP_SCHEMA,
            cv.Optional(CONF_FIELDS): FIELDS_MAP_SCHEMA,
            cv.Optional(CONF_POINTS): cv.ensure_list(POINT_ITEM_SCHEMA),
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
            cv.Optional(CONF_SKIP_IF_NAN, default=False): cv.boolean,
        }
    ),
    _validate_measurement_entry,
)

TOP_LEVEL_POINT_SCHEMA = cv.All(
    POINT_ITEM_SCHEMA,
    cv.Schema(
        {
            cv.Required(CONF_MEASUREMENT): _validate_tag_or_field_key,
        },
        extra=cv.ALLOW_EXTRA,
    ),
)

HOST_ENTITY_SCHEMA = text.text_schema(
    InfluxDBHostText,
    entity_category=ENTITY_CATEGORY_CONFIG,
    icon="mdi:ip-network",
    mode="TEXT",
).extend(
    {
        cv.Optional(CONF_INITIAL_VALUE): cv.string,
        cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
    }
)

PORT_ENTITY_SCHEMA = number.number_schema(
    InfluxDBPortNumber,
    entity_category=ENTITY_CATEGORY_CONFIG,
    icon="mdi:ear-hearing",
).extend(
    {
        cv.Optional(CONF_INITIAL_VALUE): cv.port,
        cv.Optional(CONF_MIN_VALUE, default=1): cv.port,
        cv.Optional(CONF_MAX_VALUE, default=65535): cv.port,
        cv.Optional(CONF_STEP, default=1): cv.positive_int,
        cv.Optional(CONF_MODE, default="BOX"): cv.enum(number.NUMBER_MODES, upper=True),
        cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
    }
)


def _validate_root_config(config):
    if (
        CONF_HOST not in config
        and CONF_HOST_ENTITY not in config
        and CONF_HOST_TEXT_ID not in config
    ):
        raise cv.Invalid(
            "Must specify at least one of 'host:', 'host_entity:', or 'host_text_id:'"
        )
    if CONF_HOST_ENTITY in config and CONF_HOST_TEXT_ID in config:
        raise cv.Invalid("Cannot specify both 'host_entity:' and 'host_text_id:'")
    if CONF_PORT_ENTITY in config and CONF_PORT_NUMBER_ID in config:
        raise cv.Invalid("Cannot specify both 'port_entity:' and 'port_number_id:'")
    if not config.get(CONF_MEASUREMENTS) and not config.get(CONF_POINTS):
        raise cv.Invalid("Must specify at least one entry in 'measurements:' or 'points:'")
    from esphome.components import socket

    if hasattr(socket, "consume_sockets") and hasattr(socket, "SocketType"):
        socket.consume_sockets(1, "influxdb_udp", socket.SocketType.UDP)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(InfluxDBUDP),
            cv.Optional(CONF_HOST): cv.string,
            cv.Optional(CONF_PORT, default=8089): cv.port,
            cv.Optional(CONF_MAX_PACKET_SIZE, default=1472): cv.int_range(min=64, max=65507),
            cv.Optional(CONF_TAGS, default={}): TAGS_MAP_SCHEMA,
            cv.Optional(CONF_HOST_ENTITY): HOST_ENTITY_SCHEMA,
            cv.Optional(CONF_PORT_ENTITY): PORT_ENTITY_SCHEMA,
            cv.Optional(CONF_HOST_TEXT_ID): cv.use_id(text.Text),
            cv.Optional(CONF_PORT_NUMBER_ID): cv.use_id(number.Number),
            cv.Optional(CONF_MEASUREMENTS, default=[]): cv.ensure_list(MEASUREMENT_ENTRY_SCHEMA),
            cv.Optional(CONF_POINTS, default=[]): cv.ensure_list(TOP_LEVEL_POINT_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_root_config,
)


# -----------------------------------------------------------------------------
# Final Validation & Automatic Update Interval Inference
# -----------------------------------------------------------------------------


def _get_spec_entity_id(spec):
    for key in ENTITY_SOURCE_KEYS:
        if key in spec:
            return spec[key]
    return None


def _extract_ms(val):
    if val is None:
        return None
    if hasattr(val, "total_milliseconds"):
        return int(val.total_milliseconds)
    if isinstance(val, int):
        return val
    return None


def _find_component_with_ancestors(fconf, target_id):
    """Locate `(target_dict, parent_dicts)` for a component/entity or hub with `id == target_id`."""
    visited = set()

    def _walk(node, dict_ancestors):
        if id(node) in visited:
            return None
        if isinstance(node, dict):
            visited.add(id(node))
            if node.get(CONF_ID) == target_id:
                return node, list(dict_ancestors)
            next_ancestors = [node] + dict_ancestors
            for v in node.values():
                found = _walk(v, next_ancestors)
                if found is not None:
                    return found
        elif isinstance(node, list):
            visited.add(id(node))
            for item in node:
                found = _walk(item, dict_ancestors)
                if found is not None:
                    return found
        return None

    return _walk(fconf, [])


def _find_component_dict_by_id(fconf, target_id):
    res = _find_component_with_ancestors(fconf, target_id)
    return res[0] if res is not None else None


def _extract_throttle_filter_ms(entity_conf):
    """If an entity has `throttle` or `throttle_average` filters, return the last one's period in ms."""
    filters = entity_conf.get(CONF_FILTERS)
    if not isinstance(filters, list):
        return None
    last_throttle_ms = None
    for flt in filters:
        if isinstance(flt, dict):
            for key in ("throttle", "throttle_average"):
                if key in flt:
                    ms = _extract_ms(flt[key])
                    if ms is not None and ms != SCHEDULER_DONT_RUN and ms > 0:
                        last_throttle_ms = ms
    return last_throttle_ms


def _resolve_entity_interval_ms(fconf, entity_id, _depth=0):
    """Resolve the effective update interval (in ms, or SCHEDULER_DONT_RUN, or None) for an entity ID."""
    if _depth > 8 or entity_id is None:
        return None
    found = _find_component_with_ancestors(fconf, entity_id)
    if found is None:
        return None
    entity_conf, ancestors = found
    if not isinstance(entity_conf, dict):
        return None

    # 1. Check sensor/binary_sensor/text_sensor filters for `throttle` / `throttle_average` first
    throttle_ms = _extract_throttle_filter_ms(entity_conf)
    if throttle_ms is not None:
        return throttle_ms

    # Candidate dicts to inspect: the entity dict itself, followed by enclosing platform/component dicts
    chain = [entity_conf] + [a for a in ancestors if isinstance(a, dict) and a is not fconf]

    # 2. If this is a `copy` platform entity referencing another `sensor:`, follow the source entity
    for d in chain:
        if d.get(CONF_PLATFORM) == "copy" and CONF_SENSOR in d:
            src_ms = _resolve_entity_interval_ms(fconf, d[CONF_SENSOR], _depth + 1)
            if src_ms is not None:
                return src_ms

    # 3. Check direct `throttle` or `update_interval` on the entity or its enclosing platform block
    # Note: `platform: template` entities in ESPHome inherit a schema default of `update_interval: 60s`
    # even when `lambda:` is omitted (push-only via `publish_state`). Ignore default 60s on push-only templates.
    for d in chain:
        if "throttle" in d:
            ms = _extract_ms(d["throttle"])
            if ms is not None:
                return ms
        if CONF_UPDATE_INTERVAL in d:
            platform = d.get(CONF_PLATFORM)
            if not (platform == "template" and CONF_LAMBDA not in d):
                ms = _extract_ms(d[CONF_UPDATE_INTERVAL])
                if ms is not None:
                    return ms

    # 4. Check parent hub references (`*_id` keys such as `jbd_bms_ble_id`, `modbus_controller_id`, `ina226_id`, etc.)
    for d in chain:
        for key, val in d.items():
            if key != CONF_ID and (key.endswith("_id") or key in ("parent_id", "hub_id")) and hasattr(val, "id"):
                hub_conf = _find_component_dict_by_id(fconf, val)
                if isinstance(hub_conf, dict):
                    for hub_interval_key in ("throttle", CONF_UPDATE_INTERVAL):
                        if hub_interval_key in hub_conf:
                            ms = _extract_ms(hub_conf[hub_interval_key])
                            if ms is not None:
                                return ms

    return None


def _collect_specs_entity_ids(tags_map, fields_map):
    ids = []
    for spec in tags_map.values():
        eid = _get_spec_entity_id(spec)
        if eid is not None:
            ids.append(eid)
    for spec in fields_map.values():
        eid = _get_spec_entity_id(spec)
        if eid is not None:
            ids.append(eid)
    return ids


def _infer_interval_ms(fconf, entity_ids, label):
    finite_intervals = []
    saw_never = False
    for eid in entity_ids:
        ms = _resolve_entity_interval_ms(fconf, eid)
        if ms is None:
            continue
        if ms == SCHEDULER_DONT_RUN:
            saw_never = True
        elif ms > 0:
            finite_intervals.append(ms)

    if finite_intervals:
        return min(finite_intervals)
    if saw_never:
        return SCHEDULER_DONT_RUN
    raise cv.Invalid(
        f"Could not automatically determine 'update_interval' for {label} because none of its referenced "
        f"entities define a polling interval or throttle filter. Please specify 'update_interval:' explicitly "
        f"(e.g., 'update_interval: 5s' or 'update_interval: never')."
    )


def _final_validate(config):
    fconf = fv.full_config.get()
    global_tags = config.get(CONF_TAGS, {})

    for m_idx, entry in enumerate(config.get(CONF_MEASUREMENTS, [])):
        m_name = entry.get(CONF_MEASUREMENT, f"measurements[{m_idx}]")
        entry_tags = OrderedDict(global_tags)
        entry_tags.update(entry.get(CONF_TAGS, {}))

        # Resolve any per-point intervals if points have their own update_interval
        points = entry.get(CONF_POINTS, [])
        if CONF_UPDATE_INTERVAL in entry:
            entry[CONF_UPDATE_INTERVAL] = _extract_ms(entry[CONF_UPDATE_INTERVAL])
        else:
            entity_ids = []
            if entry.get(CONF_FIELDS):
                entity_ids.extend(_collect_specs_entity_ids(entry_tags, entry[CONF_FIELDS]))
            for pt in points:
                if CONF_UPDATE_INTERVAL not in pt:
                    pt_tags = OrderedDict(entry_tags)
                    pt_tags.update(pt.get(CONF_TAGS, {}))
                    entity_ids.extend(_collect_specs_entity_ids(pt_tags, pt[CONF_FIELDS]))
            if entity_ids or not points:
                entry[CONF_UPDATE_INTERVAL] = _infer_interval_ms(
                    fconf, entity_ids, f"measurement '{m_name}'"
                )
            else:
                entry[CONF_UPDATE_INTERVAL] = SCHEDULER_DONT_RUN

        for p_idx, pt in enumerate(points):
            if CONF_UPDATE_INTERVAL in pt:
                pt[CONF_UPDATE_INTERVAL] = _extract_ms(pt[CONF_UPDATE_INTERVAL])

    for p_idx, pt in enumerate(config.get(CONF_POINTS, [])):
        p_name = pt.get(CONF_MEASUREMENT, f"points[{p_idx}]")
        if CONF_UPDATE_INTERVAL in pt:
            pt[CONF_UPDATE_INTERVAL] = _extract_ms(pt[CONF_UPDATE_INTERVAL])
        else:
            pt_tags = OrderedDict(global_tags)
            pt_tags.update(pt.get(CONF_TAGS, {}))
            entity_ids = _collect_specs_entity_ids(pt_tags, pt[CONF_FIELDS])
            pt[CONF_UPDATE_INTERVAL] = _infer_interval_ms(
                fconf, entity_ids, f"point '{p_name}'"
            )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


# -----------------------------------------------------------------------------
# C++ Code Generation
# -----------------------------------------------------------------------------


def _c_escape(s: str) -> str:
    """Format a Python string as a C++ string literal content (escaping backslash and double-quote)."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _format_ignore_list_cpp(ignore_values):
    if not ignore_values:
        return "{}"
    items = ", ".join(f'"{_c_escape(v)}"' for v in ignore_values)
    return f"{{{items}}}"


async def _generate_tag_stmt(key: str, spec: dict) -> str:
    key_lit = f'"{_c_escape(key)}"'
    decimals = spec.get(CONF_DECIMALS, -1)
    ignore_cpp = _format_ignore_list_cpp(spec.get(CONF_IGNORE_VALUES, []))

    if CONF_LAMBDA in spec:
        lam = await cg.process_lambda(spec[CONF_LAMBDA], [])
        return f"__influx->buffer().add_tag_auto({key_lit}, ({lam})(), {decimals}, {ignore_cpp});"

    eid = _get_spec_entity_id(spec)
    entity_var = await cg.get_variable(eid)
    return f"__influx->add_entity_tag({key_lit}, {entity_var}, {decimals}, {ignore_cpp});"


async def _generate_field_stmt(key: str, spec: dict) -> str:
    key_lit = f'"{_c_escape(key)}"'
    field_type = spec.get(CONF_TYPE)
    decimals = spec.get(CONF_DECIMALS, -1)
    bit = spec.get(CONF_BIT)
    ignore_values = spec.get(CONF_IGNORE_VALUES, [])
    ignore_cpp = _format_ignore_list_cpp(ignore_values)
    int_suffix = "true" if spec.get(CONF_INT_SUFFIX, True) else "false"

    if CONF_LAMBDA in spec:
        lam = await cg.process_lambda(spec[CONF_LAMBDA], [])
        if bit is not None:
            return f"__influx->buffer().add_field_bit({key_lit}, static_cast<float>(({lam})()), {bit});"
        if field_type == "float":
            return f"__influx->buffer().add_field_float({key_lit}, static_cast<float>(({lam})()), {decimals});"
        if field_type == "int":
            return f"__influx->buffer().add_field_int({key_lit}, static_cast<int64_t>(({lam})()));"
        if field_type == "uint":
            return f"__influx->buffer().add_field_uint({key_lit}, static_cast<uint64_t>(({lam})()), {int_suffix});"
        if field_type == "bool":
            return f"__influx->buffer().add_field_bool({key_lit}, static_cast<bool>(({lam})()));"
        if field_type == "string":
            return f"__influx->buffer().add_field_str({key_lit}, ({lam})(), {ignore_cpp});"
        return f"__influx->buffer().add_field_auto({key_lit}, ({lam})(), {decimals}, {ignore_cpp});"

    eid = _get_spec_entity_id(spec)
    entity_var = await cg.get_variable(eid)
    if bit is not None:
        return f"__influx->add_entity_field_bit({key_lit}, {entity_var}, {bit});"
    if field_type == "int":
        return f"__influx->add_entity_field_int({key_lit}, {entity_var});"
    if field_type == "uint":
        return f"__influx->add_entity_field_uint({key_lit}, {entity_var}, {int_suffix});"
    if field_type == "bool":
        return f"__influx->add_entity_field_bool({key_lit}, {entity_var});"
    if ignore_values:
        return f"__influx->add_entity_field({key_lit}, {entity_var}, {decimals}, {ignore_cpp});"
    return f"__influx->add_entity_field({key_lit}, {entity_var}, {decimals});"


async def _generate_line_code(
    measurement_name: str,
    merged_tags: OrderedDict,
    fields_map: dict,
    skip_if_nan: bool,
) -> str:
    escaped_meas = _c_escape(escape_measurement(measurement_name))
    static_tag_parts = []
    dynamic_tag_stmts = []

    for t_key, t_spec in merged_tags.items():
        if CONF_STATIC_VALUE in t_spec:
            static_tag_parts.append(f",{escape_tag(t_key)}={escape_tag(t_spec[CONF_STATIC_VALUE])}")
        else:
            dynamic_tag_stmts.append(await _generate_tag_stmt(t_key, t_spec))

    static_tags_lit = f'"{_c_escape("".join(static_tag_parts))}"' if static_tag_parts else "nullptr"
    skip_cpp = "true" if skip_if_nan else "false"

    field_stmts = []
    for f_key, f_spec in fields_map.items():
        field_stmts.append(await _generate_field_stmt(f_key, f_spec))

    inner_lines = [f'__influx->buffer().begin_measurement("{escaped_meas}", {static_tags_lit});']
    inner_lines.extend(dynamic_tag_stmts)
    inner_lines.extend(field_stmts)
    inner_body = "\n    ".join(inner_lines)

    return (
        f"__influx->buffer().build_line({skip_cpp}, [&]() {{\n"
        f"    {inner_body}\n"
        f"  }});"
    )


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_max_packet_size(config[CONF_MAX_PACKET_SIZE]))
    if CONF_HOST in config:
        cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))

    if CONF_HOST_ENTITY in config:
        he_conf = config[CONF_HOST_ENTITY]
        he_var = await text.new_text(he_conf)
        await cg.register_component(he_var, he_conf)
        await cg.register_parented(he_var, var)
        initial_host = he_conf.get(CONF_INITIAL_VALUE, config.get(CONF_HOST, ""))
        cg.add(he_var.set_initial_value(initial_host))
        cg.add(he_var.set_restore_value(he_conf[CONF_RESTORE_VALUE]))

    if CONF_PORT_ENTITY in config:
        pe_conf = config[CONF_PORT_ENTITY]
        pe_var = await number.new_number(
            pe_conf,
            min_value=pe_conf[CONF_MIN_VALUE],
            max_value=pe_conf[CONF_MAX_VALUE],
            step=pe_conf[CONF_STEP],
        )
        await cg.register_component(pe_var, pe_conf)
        await cg.register_parented(pe_var, var)
        initial_port = pe_conf.get(CONF_INITIAL_VALUE, config[CONF_PORT])
        cg.add(pe_var.set_initial_value(initial_port))
        cg.add(pe_var.set_restore_value(pe_conf[CONF_RESTORE_VALUE]))

    if CONF_HOST_TEXT_ID in config:
        host_text = await cg.get_variable(config[CONF_HOST_TEXT_ID])
        cg.add(
            host_text.add_on_state_callback(
                cg.RawExpression(f"[](std::string state) {{ {var}->set_host(state); }}")
            )
        )

    if CONF_PORT_NUMBER_ID in config:
        port_num = await cg.get_variable(config[CONF_PORT_NUMBER_ID])
        cg.add(
            port_num.add_on_state_callback(
                cg.RawExpression(
                    f"[](float state) {{ if (!std::isnan(state)) {var}->set_port(static_cast<uint16_t>(state)); }}"
                )
            )
        )

    global_tags = config.get(CONF_TAGS, {})
    interval_groups = OrderedDict()

    def _add_to_interval_group(interval_ms, line_code):
        if interval_ms is None or interval_ms == SCHEDULER_DONT_RUN or interval_ms <= 0:
            return
        interval_groups.setdefault(int(interval_ms), []).append(line_code)

    for entry in config.get(CONF_MEASUREMENTS, []):
        entry_target = cg.new_Pvariable(entry[CONF_ID])
        cg.add(entry_target.set_parent(var))

        entry_tags = OrderedDict(global_tags)
        entry_tags.update(entry.get(CONF_TAGS, {}))
        entry_interval = _extract_ms(entry.get(CONF_UPDATE_INTERVAL))
        entry_skip_nan = entry.get(CONF_SKIP_IF_NAN, False)

        entry_line_codes = []

        if entry.get(CONF_FIELDS):
            line_code = await _generate_line_code(
                entry[CONF_MEASUREMENT],
                entry_tags,
                entry[CONF_FIELDS],
                entry_skip_nan,
            )
            entry_line_codes.append(line_code)
            _add_to_interval_group(entry_interval, line_code)

        for pt in entry.get(CONF_POINTS, []):
            pt_target = cg.new_Pvariable(pt[CONF_ID])
            cg.add(pt_target.set_parent(var))

            pt_meas = pt.get(CONF_MEASUREMENT, entry.get(CONF_MEASUREMENT))
            pt_tags = OrderedDict(entry_tags)
            pt_tags.update(pt.get(CONF_TAGS, {}))
            pt_skip_nan = pt.get(CONF_SKIP_IF_NAN, entry_skip_nan)
            pt_interval = _extract_ms(pt.get(CONF_UPDATE_INTERVAL, entry_interval))

            pt_line_code = await _generate_line_code(
                pt_meas,
                pt_tags,
                pt[CONF_FIELDS],
                pt_skip_nan,
            )
            entry_line_codes.append(pt_line_code)
            _add_to_interval_group(pt_interval, pt_line_code)

            pt_fn_expr = cg.RawExpression(
                f"[](esphome::influxdb_udp::InfluxDBUDP *__influx) {{\n"
                f"  {pt_line_code}\n"
                f"  __influx->flush();\n"
                f"}}"
            )
            cg.add(pt_target.set_publish_fn(pt_fn_expr))

        if entry_line_codes:
            joined_entry_lines = "\n  ".join(entry_line_codes)
            entry_fn_expr = cg.RawExpression(
                f"[](esphome::influxdb_udp::InfluxDBUDP *__influx) {{\n"
                f"  {joined_entry_lines}\n"
                f"  __influx->flush();\n"
                f"}}"
            )
            cg.add(entry_target.set_publish_fn(entry_fn_expr))

    for pt in config.get(CONF_POINTS, []):
        pt_target = cg.new_Pvariable(pt[CONF_ID])
        cg.add(pt_target.set_parent(var))

        pt_tags = OrderedDict(global_tags)
        pt_tags.update(pt.get(CONF_TAGS, {}))
        pt_skip_nan = pt.get(CONF_SKIP_IF_NAN, False)
        pt_interval = _extract_ms(pt.get(CONF_UPDATE_INTERVAL))

        pt_line_code = await _generate_line_code(
            pt[CONF_MEASUREMENT],
            pt_tags,
            pt[CONF_FIELDS],
            pt_skip_nan,
        )
        _add_to_interval_group(pt_interval, pt_line_code)

        pt_fn_expr = cg.RawExpression(
            f"[](esphome::influxdb_udp::InfluxDBUDP *__influx) {{\n"
            f"  {pt_line_code}\n"
            f"  __influx->flush();\n"
            f"}}"
        )
        cg.add(pt_target.set_publish_fn(pt_fn_expr))

    for interval_ms, line_codes in interval_groups.items():
        joined_lines = "\n  ".join(line_codes)
        batch_fn_expr = cg.RawExpression(
            f"[](esphome::influxdb_udp::InfluxDBUDP *__influx) {{\n"
            f"  {joined_lines}\n"
            f"  __influx->flush();\n"
            f"}}"
        )
        cg.add(var.register_periodic_batch(interval_ms, batch_fn_expr))


# -----------------------------------------------------------------------------
# Automation Action (`influxdb_udp.publish`)
# -----------------------------------------------------------------------------

PUBLISH_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(BatchTarget),
    }
)


import inspect

_REGISTER_ACTION_KWARGS = (
    {"synchronous": True}
    if "synchronous" in inspect.signature(automation.register_action).parameters
    else {}
)


@automation.register_action(
    "influxdb_udp.publish", PublishAction, PUBLISH_ACTION_SCHEMA, **_REGISTER_ACTION_KWARGS
)
async def influxdb_udp_publish_to_code(config, action_id, template_arg, args):
    target = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, target)
