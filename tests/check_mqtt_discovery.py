"""Preveri dejanski C++ izpis Discovery in zdruzljivost uporabnikovega YAML."""
import json
from pathlib import Path
import re
import subprocess
import sys

from jinja2 import Environment
import yaml


def main():
    output = subprocess.check_output([sys.argv[1]], text=True, encoding="utf-8")
    configs = {}
    topics = set()
    identifiers = set()
    unique_ids = set()
    for line in output.splitlines():
        topic, payload = line.split("\t", 1)
        config = json.loads(payload)
        assert topic not in topics
        assert config["unique_id"] not in unique_ids
        assert topic.endswith("/" + config["unique_id"] + "/config")
        assert config["availability_topic"] == "tuya_rf_s11/availability"
        assert config["payload_available"] == "online"
        assert config["payload_not_available"] == "offline"
        assert config["device"]["name"] == "S11 RF Hub"
        identifiers.add(tuple(config["device"]["identifiers"]))
        topics.add(topic)
        unique_ids.add(config["unique_id"])
        configs[config.get("state_topic", config.get("command_topic"))] = config

    assert len(configs) == 8 and len(identifiers) == 1
    light = configs["tuya_rf_s11/led/state"]
    assert light["command_topic"] == "tuya_rf_s11/led/set"
    assert light["brightness_command_topic"] == "tuya_rf_s11/led/brightness/set"
    assert light["brightness_state_topic"] == "tuya_rf_s11/led/brightness/state"
    assert light["rgb_command_topic"] == "tuya_rf_s11/led/rgb/set"
    assert light["rgb_state_topic"] == "tuya_rf_s11/led/rgb/state"
    assert light["brightness_scale"] == 255
    assert light["entity_category"] == "config"
    assert light["retain"] is False and light["optimistic"] is False
    assert light["on_command_type"] == "last"
    restart = configs["tuya_rf_s11/restart/set"]
    assert restart["payload_press"] == "RESTART_12345678"
    assert restart["device_class"] == "restart" and restart["entity_category"] == "config"
    assert restart["retain"] is False and "state_topic" not in restart
    root = Path(__file__).resolve().parents[1]
    automation = yaml.safe_load((root / "homeassistant/s11_test_light.yaml").read_text(encoding="utf-8"))
    assert automation["actions"][0]["action"] == "light.toggle"
    assert automation["actions"][0]["target"]["entity_id"] == "light.minismartswitch_1"
    assert automation["mode"] == "queued"
    assert len(automation["triggers"]) == 2
    environment = Environment()
    for trigger in automation["triggers"]:
        assert trigger["trigger"] == "mqtt" and trigger["payload"] == "PRESS"
        config = configs[trigger["topic"]]
        assert config["event_types"] == ["press"]
        assert config["device_class"] == "button"
        template = environment.from_string(config["value_template"])
        assert json.loads(template.render(value="PRESS")) == {"event_type": "press"}
        for other in ("", "offline", "REPEAT", "PRESS\n", '{"event_type":"press"}'):
            assert json.loads(template.render(value=other)) == {}

    for key in ("ip", "ssid", "rssi", "uptime"):
        config = configs["tuya_rf_s11/diagnostics/" + key]
        assert config["entity_category"] == "diagnostic"
        assert config["expire_after"] == 90
    assert configs["tuya_rf_s11/diagnostics/rssi"]["unit_of_measurement"] == "dBm"
    uptime = configs["tuya_rf_s11/diagnostics/uptime"]
    assert uptime["unit_of_measurement"] is None
    assert uptime["device_class"] is None
    template = environment.from_string(uptime["value_template"])
    for seconds, expected in (
        (0, "0 d 00 h 00 min"),
        (59, "0 d 00 h 00 min"),
        (60, "0 d 00 h 01 min"),
        (3599, "0 d 00 h 59 min"),
        (3600, "0 d 01 h 00 min"),
        (86399, "0 d 23 h 59 min"),
        (86400, "1 d 00 h 00 min"),
        (191820, "2 d 05 h 17 min"),
        (4320000, "50 d 00 h 00 min"),
    ):
        assert template.render(value=str(seconds)) == expected
    source = (root / "src/mqtt_bridge.cpp").read_text(encoding="utf-8")
    press_topics = re.search(r"PRESS_TOPICS\[\]\[32\] = \{(.*?)\};", source, re.S).group(1)
    assert set(re.findall(r'"([^"]+)"', press_topics)) == {
        trigger["topic"] for trigger in automation["triggers"]
    }
    assert 'client.publish(PRESS_TOPICS[event.switch_id - 1], "PRESS", false)' in source
    assert 'client.publish(topic, payload, true)' in source
    print("PASS: 8 C++ Discovery payloadov, RGB/restart, paketne meje, predloge in YAML spalnicne luci.")


if __name__ == "__main__":
    main()
