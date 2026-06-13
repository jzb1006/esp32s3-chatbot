import re
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SKETCH_PATH = PROJECT_ROOT / "esp32s3_radio_self_test" / "esp32s3_radio_self_test.ino"


def test_radio_self_test_sketch_exists():
    assert SKETCH_PATH.exists()


def test_radio_self_test_scans_wifi_without_credentials():
    sketch = SKETCH_PATH.read_text(encoding="utf-8")

    assert "#include <WiFi.h>" in sketch
    assert "WiFi.scanNetworks()" in sketch
    assert "WiFi.begin(" not in sketch


def test_radio_self_test_advertises_ble_device_name():
    sketch = SKETCH_PATH.read_text(encoding="utf-8")

    assert "#include <BLEDevice.h>" in sketch
    assert 'BLE_DEVICE_NAME = "ESP32S3-Radio-Test"' in sketch
    assert "BLEDevice::startAdvertising()" in sketch


def test_radio_self_test_uses_usb_serial_baud_rate():
    sketch = SKETCH_PATH.read_text(encoding="utf-8")

    baud_match = re.search(r"SERIAL_BAUD_RATE = (\d+);", sketch)
    assert baud_match is not None
    assert int(baud_match.group(1)) == 115200
    assert "Serial.begin(SERIAL_BAUD_RATE)" in sketch
