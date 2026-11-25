import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import button, sensor, text_sensor
from esphome.components import time as time_
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_INVERTED,
    CONF_NAME,
    CONF_NUMBER,
    CONF_RX_PIN,
    CONF_SENSORS,
    CONF_TIME_ID,
    CONF_TX_PIN,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_VOLT,
    UNIT_WATT,
    UNIT_WATT_HOURS,
)
from esphome.core import CORE

CODEOWNERS = ["@wergio"]
DEPENDENCIES = ["time"]
AUTO_LOAD = ["sensor", "text_sensor", "button"]

aurora_monitor_ns = cg.esphome_ns.namespace("aurora_monitor")
AuroraMonitor = aurora_monitor_ns.class_("AuroraMonitor", cg.PollingComponent)
ResetButton = aurora_monitor_ns.class_(
    "ResetButton", button.Button, cg.Parented.template(AuroraMonitor)
)

CONF_INVERTERS = "inverters"
CONF_PREFIX = "prefix"
CONF_EXCLUDE = "exclude"
CONF_MPPT_INPUTS = "mppt_inputs"
CONF_RESET_PIN = "gpio_reset_pin"
CONF_RESET_BUTTON = "reset_button"
CONF_TX_CONTROL_PIN = "gpio_tx_control_pin"
CONF_ACTIVITY_LED = "activity_led"
CONF_ACTIVITY_LED_PIN = "activity_led_pin"
CONF_TX_ENABLE_DELAY = "tx_enable_delay"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_RETRIES = "retries"
CONF_POWER_IN_TOTAL = "power_in_total"
CONF_EFFICIENCY = "efficiency"
CONF_CONNECTION_STATUS = "connection_status"
CONF_GLOBAL_STATE = "global_state"
CONF_ALARM_STATE = "alarm_state"
CONF_CLOCK_OFFSET = "clock_offset"
CONF_INVERTER_STATE = "inverter_state"

# Default UART pins: on ESP8266 the swapped UART0 pins, which are not connected
# to the USB adapter.
DEFAULT_UART_PINS = {
    PLATFORM_ESP8266: (13, 15),
    PLATFORM_ESP32: (14, 27),
}
# On ESP8266, UART0 can only use its default pins or the swapped ones.
ESP8266_UART_PINS = [(3, 1), (13, 15)]
# Default RS485 direction (DE/RE) pin. On ESP32, GPIO12 is a strapping pin
# (flash voltage) and GPIO14 is the default RX pin.
DEFAULT_TX_CONTROL_PIN = {
    PLATFORM_ESP8266: 12,
    PLATFORM_ESP32: 26,
}
RESET_BUTTON_NAME = "Reset RS485 adapter"

# Device and state classes let Home Assistant keep long-term statistics and
# use the energy counters in its Energy dashboard. Sensors with a device class
# use the icon Home Assistant gives to that class.
VOLTAGE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_VOLTAGE,
    state_class=STATE_CLASS_MEASUREMENT,
)
CURRENT_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
)
POWER_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
GRID_POWER_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
FREQUENCY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_HERTZ,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_FREQUENCY,
    state_class=STATE_CLASS_MEASUREMENT,
)
EFFICIENCY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_PERCENT,
    accuracy_decimals=1,
    icon="mdi:gauge",
    state_class=STATE_CLASS_MEASUREMENT,
)
TEMPERATURE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)
# Counters that restart from zero at the start of their period (or never, for
# the lifetime total): Home Assistant treats a decrease as a new cycle.
ENERGY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT_HOURS,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)
STATUS_SCHEMA = text_sensor.text_sensor_schema(icon="mdi:connection")
GLOBAL_STATE_SCHEMA = text_sensor.text_sensor_schema(icon="mdi:information-outline")
ALARM_STATE_SCHEMA = text_sensor.text_sensor_schema(icon="mdi:alert-circle-outline")
CLOCK_OFFSET_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_SECOND,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_DURATION,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)


def _diagnostic_text(icon):
    return text_sensor.text_sensor_schema(
        icon=icon, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    )


STATE_TEXT_SCHEMA = text_sensor.text_sensor_schema(icon="mdi:state-machine")
ISOLATION_RESISTANCE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="MΩ",
    accuracy_decimals=2,
    icon="mdi:omega",
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)
LEAKAGE_CURRENT_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

# Text sensor key -> (C++ InverterExtra, schema). These values are read only
# when the inverter connects (and the alarms when the alarm state changes).
EXTRA_TEXTS = {
    "model": ("EXTRA_MODEL", _diagnostic_text("mdi:solar-power-variant")),
    "part_number": ("EXTRA_PART_NUMBER", _diagnostic_text("mdi:barcode")),
    "serial_number": ("EXTRA_SERIAL_NUMBER", _diagnostic_text("mdi:barcode")),
    "manufacturing_date": (
        "EXTRA_MANUFACTURING",
        _diagnostic_text("mdi:calendar-month"),
    ),
    "firmware": ("EXTRA_FIRMWARE", _diagnostic_text("mdi:chip")),
    "last_alarms": ("EXTRA_LAST_ALARMS", _diagnostic_text("mdi:alert-outline")),
}
# Numeric sensor key -> (C++ InverterExtra, schema). The isolation resistance
# and the leakage currents are read when the inverter connects and every hour.
EXTRA_VALUES = {
    CONF_CLOCK_OFFSET: ("EXTRA_CLOCK", CLOCK_OFFSET_SCHEMA),
    "isolation_resistance": (
        "EXTRA_ISOLATION_RESISTANCE",
        ISOLATION_RESISTANCE_SCHEMA,
    ),
    "leakage_current_dc_dc": (
        "EXTRA_LEAKAGE_CURRENT_DC_DC",
        LEAKAGE_CURRENT_SCHEMA,
    ),
    "leakage_current_inverter": (
        "EXTRA_LEAKAGE_CURRENT_INVERTER",
        LEAKAGE_CURRENT_SCHEMA,
    ),
}
# DC/DC state of each input, decoded from the state request: key -> input.
INPUT_STATES = {"input_1_state": 0, "input_2_state": 1}
RESET_BUTTON_SCHEMA = button.button_schema(
    ResetButton, icon="mdi:restart", entity_category=ENTITY_CATEGORY_CONFIG
)

# Sensor key -> (C++ InverterReading, schema). The key is also the default
# entity name, after the inverter prefix.
READINGS = {
    "v_in_1": ("READING_V_IN_1", VOLTAGE_SCHEMA),
    "v_in_2": ("READING_V_IN_2", VOLTAGE_SCHEMA),
    "i_in_1": ("READING_I_IN_1", CURRENT_SCHEMA),
    "i_in_2": ("READING_I_IN_2", CURRENT_SCHEMA),
    "power_in_1": ("READING_POWER_IN_1", POWER_SCHEMA),
    "power_in_2": ("READING_POWER_IN_2", POWER_SCHEMA),
    "grid_power": ("READING_GRID_POWER", GRID_POWER_SCHEMA),
    "grid_voltage": ("READING_GRID_VOLTAGE", VOLTAGE_SCHEMA),
    "grid_current": ("READING_GRID_CURRENT", CURRENT_SCHEMA),
    "grid_frequency": ("READING_GRID_FREQUENCY", FREQUENCY_SCHEMA),
    "power_peak_today": ("READING_POWER_PEAK_TODAY", POWER_SCHEMA),
    "power_peak_max": ("READING_POWER_PEAK_MAX", POWER_SCHEMA),
    "temp_inverter": ("READING_TEMPERATURE_INVERTER", TEMPERATURE_SCHEMA),
    "temp_booster": ("READING_TEMPERATURE_BOOSTER", TEMPERATURE_SCHEMA),
    "cumulated_energy_today": ("READING_ENERGY_TODAY", ENERGY_SCHEMA),
    "cumulated_energy_week": ("READING_ENERGY_WEEK", ENERGY_SCHEMA),
    "cumulated_energy_month": ("READING_ENERGY_MONTH", ENERGY_SCHEMA),
    "cumulated_energy_year": ("READING_ENERGY_YEAR", ENERGY_SCHEMA),
    "cumulated_energy_total": ("READING_ENERGY_TOTAL", ENERGY_SCHEMA),
}
# Every entity created for each inverter, with its schema.
ENTITY_SCHEMAS = {
    **{key: schema for key, (_, schema) in READINGS.items()},
    CONF_POWER_IN_TOTAL: POWER_SCHEMA,
    CONF_EFFICIENCY: EFFICIENCY_SCHEMA,
    CONF_CONNECTION_STATUS: STATUS_SCHEMA,
    CONF_GLOBAL_STATE: GLOBAL_STATE_SCHEMA,
    CONF_ALARM_STATE: ALARM_STATE_SCHEMA,
    CONF_INVERTER_STATE: STATE_TEXT_SCHEMA,
    **{key: STATE_TEXT_SCHEMA for key in INPUT_STATES},
    **{key: schema for key, (_, schema) in EXTRA_TEXTS.items()},
    **{key: schema for key, (_, schema) in EXTRA_VALUES.items()},
}

# Sensors of the second MPPT input (channel 2).
SECOND_INPUT_KEYS = {"v_in_2", "i_in_2", "power_in_2", "input_2_state"}


def _entity_name(prefix, key):
    return f"{prefix}_{key}" if prefix else key


def _build_inverter_entities(inverter):
    """Create the configuration of every entity that is not excluded.

    Each entity gets a default name (prefix + key) and can be customised under
    `sensors:` with any option of its schema.
    """
    excluded = set(inverter[CONF_EXCLUDE])
    overrides = inverter[CONF_SENSORS]
    for key in excluded & set(overrides):
        raise cv.Invalid(f"'{key}' is both excluded and customised under 'sensors'")
    if inverter[CONF_MPPT_INPUTS] == 1:
        for key in SECOND_INPUT_KEYS & set(overrides):
            raise cv.Invalid(f"'{key}' does not exist with a single MPPT input")
        excluded |= SECOND_INPUT_KEYS

    entities = {}
    for key, schema in ENTITY_SCHEMAS.items():
        if key in excluded:
            continue
        entity_config = {CONF_NAME: _entity_name(inverter.get(CONF_PREFIX), key)}
        entity_config.update(overrides.get(key) or {})
        with cv.prepend_path([CONF_SENSORS, key]):
            entities[key] = schema(entity_config)
    inverter[CONF_SENSORS] = entities
    return inverter


INVERTER_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_ADDRESS, default=2): cv.int_range(min=0, max=255),
            cv.Optional(CONF_MPPT_INPUTS, default=2): cv.int_range(min=1, max=2),
            cv.Optional(CONF_PREFIX): cv.All(
                cv.string_strict, cv.Length(min=1, max=20)
            ),
            cv.Optional(CONF_EXCLUDE, default=[]): cv.ensure_list(
                cv.one_of(*ENTITY_SCHEMAS, lower=True)
            ),
            cv.Optional(CONF_SENSORS, default={}): cv.Schema(
                {
                    cv.Optional(key): cv.Any(None, cv.Schema({}, extra=cv.ALLOW_EXTRA))
                    for key in ENTITY_SCHEMAS
                }
            ),
        }
    ),
    _build_inverter_entities,
)


def _validate_inverters(inverters):
    """Check that addresses and prefixes identify each inverter."""
    if len(inverters) > 1 and any(CONF_PREFIX not in inv for inv in inverters):
        raise cv.Invalid(
            "With more than one inverter, give each one a 'prefix' to keep the "
            "entity names apart"
        )
    for key in (CONF_ADDRESS, CONF_PREFIX):
        values = [inv[key] for inv in inverters if key in inv]
        if len(values) != len(set(values)):
            raise cv.Invalid(f"Each inverter needs a different '{key}'")
    return inverters


def _set_defaults(config):
    """Fill the platform-dependent defaults and check the pins."""
    default_rx, default_tx = DEFAULT_UART_PINS[CORE.target_platform]
    config.setdefault(CONF_RX_PIN, default_rx)
    config.setdefault(CONF_TX_PIN, default_tx)
    if (
        CORE.is_esp8266
        and (config[CONF_RX_PIN], config[CONF_TX_PIN]) not in ESP8266_UART_PINS
    ):
        raise cv.Invalid(
            "On ESP8266 the RS485 bus uses UART0: set rx_pin/tx_pin to 13/15 "
            "(swapped pins) or 3/1 (default pins)"
        )

    if CONF_TX_CONTROL_PIN not in config:
        config[CONF_TX_CONTROL_PIN] = pins.gpio_output_pin_schema(
            DEFAULT_TX_CONTROL_PIN[CORE.target_platform]
        )

    # The reset circuit comes with its button (internal entry, not a YAML option).
    if CONF_RESET_PIN in config:
        config[CONF_RESET_BUTTON] = RESET_BUTTON_SCHEMA({CONF_NAME: RESET_BUTTON_NAME})

    # Use the on-board LED (GPIO2) when no activity LED pin is given. The LED
    # of ESP8266 boards such as the D1 mini is active low.
    if config[CONF_ACTIVITY_LED] and CONF_ACTIVITY_LED_PIN not in config:
        config[CONF_ACTIVITY_LED_PIN] = pins.gpio_output_pin_schema(
            {CONF_NUMBER: 2, CONF_INVERTED: CORE.is_esp8266}
        )

    # The UART pins are plain numbers, so ESPHome does not check them against
    # the other pins: make sure no pin is used twice.
    used = {
        CONF_RX_PIN: config[CONF_RX_PIN],
        CONF_TX_PIN: config[CONF_TX_PIN],
        CONF_TX_CONTROL_PIN: config[CONF_TX_CONTROL_PIN][CONF_NUMBER],
    }
    if CONF_RESET_PIN in config:
        used[CONF_RESET_PIN] = config[CONF_RESET_PIN][CONF_NUMBER]
    if config[CONF_ACTIVITY_LED]:
        used[CONF_ACTIVITY_LED_PIN] = config[CONF_ACTIVITY_LED_PIN][CONF_NUMBER]
    seen = {}
    for key, number in used.items():
        if number in seen:
            raise cv.Invalid(f"{key} and {seen[number]} both use GPIO{number}")
        seen[number] = key
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AuroraMonitor),
            cv.Required(CONF_INVERTERS): cv.All(
                cv.ensure_list(INVERTER_SCHEMA),
                cv.Length(min=1),
                _validate_inverters,
            ),
            cv.Optional(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_TX_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            # Needed for the period resets, the restore after a reboot and
            # clock_offset.
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_ACTIVITY_LED, default=True): cv.boolean,
            cv.Optional(CONF_ACTIVITY_LED_PIN): pins.gpio_output_pin_schema,
            cv.Optional(
                CONF_TX_ENABLE_DELAY, default="50ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RESPONSE_TIMEOUT, default="1000ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RETRIES, default=1): cv.int_range(min=0, max=5),
        }
    ).extend(cv.polling_component_schema("15s")),
    cv.only_on([PLATFORM_ESP8266, PLATFORM_ESP32]),
    cv.only_with_arduino,
    _set_defaults,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CORE.is_esp8266:
        # Keep the Arduino Serial object (UART0) available to the component.
        from esphome.components.esp8266.const import enable_serial

        enable_serial()

    cg.add(var.set_uart_pins(config[CONF_RX_PIN], config[CONF_TX_PIN]))
    tx_control_pin = await cg.gpio_pin_expression(config[CONF_TX_CONTROL_PIN])
    cg.add(var.set_tx_control_pin(tx_control_pin))
    cg.add(var.set_tx_enable_delay(config[CONF_TX_ENABLE_DELAY]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_retries(config[CONF_RETRIES]))

    if CONF_RESET_PIN in config:
        reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
        cg.add(var.set_reset_pin(reset_pin))
        reset_button = await button.new_button(config[CONF_RESET_BUTTON])
        await cg.register_parented(reset_button, var)

    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))

    if config[CONF_ACTIVITY_LED]:
        led_pin = await cg.gpio_pin_expression(config[CONF_ACTIVITY_LED_PIN])
        cg.add(var.set_activity_led_pin(led_pin))

    for index, inverter in enumerate(config[CONF_INVERTERS]):
        cg.add(
            var.add_inverter(
                inverter[CONF_ADDRESS],
                inverter.get(CONF_PREFIX, ""),
                inverter[CONF_MPPT_INPUTS],
            )
        )
        entities = inverter[CONF_SENSORS]

        for key, (reading, _) in READINGS.items():
            if key in entities:
                sens = await sensor.new_sensor(entities[key])
                cg.add(var.set_sensor(index, getattr(aurora_monitor_ns, reading), sens))

        for key, (extra, _) in EXTRA_TEXTS.items():
            if key in entities:
                sens = await text_sensor.new_text_sensor(entities[key])
                extra_id = getattr(aurora_monitor_ns, extra)
                cg.add(var.set_extra_text_sensor(index, extra_id, sens))

        for key, (extra, _) in EXTRA_VALUES.items():
            if key in entities:
                sens = await sensor.new_sensor(entities[key])
                extra_id = getattr(aurora_monitor_ns, extra)
                cg.add(var.set_extra_value_sensor(index, extra_id, sens))

        for key, input_index in INPUT_STATES.items():
            if key in entities:
                sens = await text_sensor.new_text_sensor(entities[key])
                cg.add(var.set_input_state_sensor(index, input_index, sens))

        for key in (CONF_POWER_IN_TOTAL, CONF_EFFICIENCY):
            if key in entities:
                sens = await sensor.new_sensor(entities[key])
                cg.add(getattr(var, f"set_{key}_sensor")(index, sens))

        for key in (
            CONF_CONNECTION_STATUS,
            CONF_GLOBAL_STATE,
            CONF_ALARM_STATE,
            CONF_INVERTER_STATE,
        ):
            if key in entities:
                sens = await text_sensor.new_text_sensor(entities[key])
                cg.add(getattr(var, f"set_{key}_sensor")(index, sens))
