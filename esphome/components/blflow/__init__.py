import gzip
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, output, sensor
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    idf_version,
    include_builtin_idf_component,
    request_tls,
)
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)
DEPENDENCIES = ["wifi", "esp32"]
AUTO_LOAD = ["web_server_base", "json", "sensor", "binary_sensor"]

CONF_FAN_OUTPUTS = "fan_outputs"
CONF_NOZZLE_TEMPERATURE = "nozzle_temperature"
CONF_FAN_SPEED = "fan_speed"
CONF_PRINTER_CONNECTED = "printer_connected"
CONF_FIRMWARE_VERSION = "firmware_version"

# Shipped with the component so it also works when pulled via github:// external_components
WEB_PAGE = Path(__file__).parent / "index.html"

blflow_ns = cg.esphome_ns.namespace("blflow")
BLFlow = blflow_ns.class_("BLFlow", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BLFlow),
        cv.Required(CONF_FAN_OUTPUTS): cv.ensure_list(cv.use_id(output.FloatOutput)),
        cv.Optional(CONF_FIRMWARE_VERSION, default="dev"): cv.string,
        cv.Optional(CONF_NOZZLE_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_FAN_SPEED): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            icon="mdi:fan",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PRINTER_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    from esphome.components import socket

    # 1 TLS connection to the printer, plus parallel browser requests to the dashboard
    socket.consume_sockets(1, "blflow_mqtt")(config)
    socket.consume_sockets(4, "blflow_web")(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for out_id in config[CONF_FAN_OUTPUTS]:
        out = await cg.get_variable(out_id)
        cg.add(var.add_fan_output(out))

    cg.add(var.set_firmware_version(config[CONF_FIRMWARE_VERSION]))

    if CONF_NOZZLE_TEMPERATURE in config:
        s = await sensor.new_sensor(config[CONF_NOZZLE_TEMPERATURE])
        cg.add(var.set_nozzle_sensor(s))
    if CONF_FAN_SPEED in config:
        s = await sensor.new_sensor(config[CONF_FAN_SPEED])
        cg.add(var.set_fan_sensor(s))
    if CONF_PRINTER_CONNECTED in config:
        s = await binary_sensor.new_binary_sensor(config[CONF_PRINTER_CONNECTED])
        cg.add(var.set_connected_sensor(s))

    gz = gzip.compress(WEB_PAGE.read_bytes(), compresslevel=9, mtime=0)
    body = ",".join(str(b) for b in gz)
    cg.add_global(
        cg.RawStatement(
            f"namespace esphome {{ namespace blflow {{ "
            f"const uint8_t INDEX_HTML_GZ[] = {{{body}}}; "
            f"const size_t INDEX_HTML_GZ_SIZE = {len(gz)}; }} }}"
        )
    )

    # Bambu MQTT: TLS against a self-signed printer CA, same trust model as the original firmware
    if idf_version() >= cv.Version(6, 0, 0):
        add_idf_component(name="espressif/mqtt", ref="1.0.0")
    else:
        include_builtin_idf_component("mqtt")
    request_tls()
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", True)

    # Read-only access to the legacy firmware's LittleFS config for one-time migration
    add_idf_component(name="joltwallet/littlefs", ref="1.20.4")
