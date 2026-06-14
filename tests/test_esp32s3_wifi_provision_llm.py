from pathlib import Path
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SKETCH_PATH = PROJECT_ROOT / "esp32s3_wifi_provision" / "esp32s3_wifi_provision.ino"


class WifiProvisionLlmSketchTest(unittest.TestCase):
    def read_sketch(self):
        return SKETCH_PATH.read_text(encoding="utf-8")

    def test_wifi_provision_adds_http_client_for_admin_backend(self):
        sketch = self.read_sketch()

        self.assertIn("#include <HTTPClient.h>", sketch)
        self.assertIn('"/api/chat"', sketch)
        self.assertIn("http.POST(body)", sketch)

    def test_wifi_provision_supports_serial_admin_and_ask_commands(self):
        sketch = self.read_sketch()

        self.assertIn("void handleSerialLine(String line)", sketch)
        self.assertIn('line.startsWith("admin ")', sketch)
        self.assertIn('line.startsWith("ask ")', sketch)
        self.assertIn("processSerialCommands();", sketch)

    def test_wifi_provision_supports_streaming_ask_command(self):
        sketch = self.read_sketch()

        self.assertIn('Serial.println("  askstream <prompt>");', sketch)
        self.assertIn('line.startsWith("askstream ")', sketch)
        self.assertIn('"/api/chat/stream"', sketch)
        self.assertIn('"Accept", "text/event-stream"', sketch)
        self.assertIn("http.getStreamPtr()", sketch)
        self.assertIn("response.output_text.delta", sketch)

    def test_wifi_provision_keeps_api_key_out_of_firmware(self):
        sketch = self.read_sketch().lower()

        self.assertNotIn("api_key", sketch)
        self.assertNotIn("authorization", sketch)
        self.assertNotIn("bearer", sketch)

if __name__ == "__main__":
    unittest.main()
