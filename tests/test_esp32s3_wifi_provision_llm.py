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

    def test_wifi_provision_supports_xiaozhi_websocket_hello(self):
        sketch = self.read_sketch()

        self.assertIn('"ws://203.195.202.54:8766/xiaozhi/v1"', sketch)
        self.assertIn('Serial.println("  wshello");', sketch)
        self.assertIn('line == "wshello"', sketch)
        self.assertIn("Authorization: Bearer ", sketch)
        self.assertIn("Protocol-Version: 1", sketch)
        self.assertIn("Device-Id: ", sketch)
        self.assertIn("Client-Id: ", sketch)
        self.assertIn('{\\"type\\":\\"hello\\"', sketch)

    def test_wifi_provision_supports_pdm_mic_self_test(self):
        sketch = self.read_sketch()

        self.assertIn("#include <ESP_I2S.h>", sketch)
        self.assertIn("PDM_MIC_CLK_PIN = 20", sketch)
        self.assertIn("PDM_MIC_DATA_PIN = 22", sketch)
        self.assertIn("setPinsPdmRx(PDM_MIC_CLK_PIN, PDM_MIC_DATA_PIN)", sketch)
        self.assertIn("I2S_MODE_PDM_RX", sketch)
        self.assertIn('Serial.println("  mictest");', sketch)
        self.assertIn('line == "mictest"', sketch)

    def test_wifi_provision_supports_speaker_self_test(self):
        sketch = self.read_sketch()

        self.assertIn("AUDIO_BCLK_PIN = 18", sketch)
        self.assertIn("AUDIO_LRCLK_PIN = 19", sketch)
        self.assertIn("AUDIO_SDATA_PIN = 21", sketch)
        self.assertIn("setPins(AUDIO_BCLK_PIN, AUDIO_LRCLK_PIN, AUDIO_SDATA_PIN)", sketch)
        self.assertIn("I2S_MODE_STD", sketch)
        self.assertIn('Serial.println("  speakertest");', sketch)
        self.assertIn('line == "speakertest"', sketch)

    def test_wifi_provision_keeps_api_key_out_of_firmware(self):
        sketch = self.read_sketch().lower()

        self.assertNotIn("api_key", sketch)
        self.assertNotIn("xiaozhi_websocket_token=", sketch)

if __name__ == "__main__":
    unittest.main()
