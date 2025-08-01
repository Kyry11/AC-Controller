const char* CONTROLLER_VERSION = "1.3";

#include <Arduino.h>
#define ARRAY_SIZE(arr)   (sizeof(arr) / sizeof((arr)[0]))

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include "OTA/OTA.h"
#include <ArduinoJson.h>
#include "AC/FujitsuAC.h"
#include <FastLED.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <SPIFFS.h>

#include "melody_player/melody_player.h"
#include "melody_player/melody_factory.h"
#include "StaticWebServer.h"

unsigned long lastMqttConnectionAttempt = 0; // Track last MQTT connection attempt time
const int mqttReconnectInterval = 5000; // 5 seconds between connection attempts

// Preferences keys for eeprom config
const char* PREF_KEY_SSID = "wifi_ssid";
const char* PREF_KEY_PASS = "wifi_pass";
const char* PREF_KEY_MQTT_BROKER = "mqtt_broker";
const char* PREF_KEY_MQTT_PORT = "mqtt_port";
const char* PREF_KEY_MQTT_USER = "mqtt_user";
const char* PREF_KEY_MQTT_PASS = "mqtt_pass";
const char* PREF_KEY_MQTT_TOPIC = "mqtt_topic";

// Preferences keys for pin configuration
const char* PREF_KEY_AC_RX_PIN = "ac_rx_pin";
const char* PREF_KEY_AC_TX_PIN = "ac_tx_pin";
const char* PREF_KEY_OUTPUT_PINS = "output_pins";
const char* PREF_KEY_INPUT_PINS = "input_pins";
const char* PREF_KEY_ZONES = "zones";

// AP Mode settings for WiFi configuration
const char* AP_CONFIG_SSID = "Baums AC Controller"; // Unique name for config AP
const char* AP_CONFIG_PASSWORD = NULL;              // No password for config AP
const byte DNS_PORT = 53;

Preferences preferences;
DNSServer dnsServer;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
char mqttBroker[64] = "";
int mqttPort = 1883;
char mqttUser[32] = "";
char mqttPassword[64] = "";
char mqttBaseTopic[32] = "";

const int apiPort = 80;

#define LEDS_COUNT        1
#define LEDS_PIN          16
#define CHANNEL           0

#define BUZZER_PIN 4
uint8_t INITIAL_BUZZER_VOLUME = 64; // 0-255, 32 default
MelodyPlayer player(BUZZER_PIN, 0U, true);

CRGB leds[LEDS_COUNT];
FujitsuAC fujitsu;
AsyncWebServer server(apiPort);
AsyncWebSocket ws("/ws");
StaticWebServer staticWebServer(&server);

bool colourLEDState = true;
uint8_t colourLEDBrightness = 30;

// Default pin configurations that can be overridden by preferences
uint8_t acRxPin = 25;
uint8_t acTxPin = 32;

// Store previous AC state to detect changes
ControlFrame previousACState;
bool acStateInitialized = false;

// Maximum number of pins we'll support
#define MAX_OUTPUT_PINS 8
#define MAX_INPUT_PINS 8
#define MAX_ZONES 8

// Zone structure to map input and output pins
struct Zone {
  String id;
  uint8_t inputPin;
  uint8_t outputPin;
};

uint8_t outputPins[MAX_OUTPUT_PINS] = { 2, 19, 21, 22, 23, 0, 0, 0 };
uint8_t outputPinCount = 5; // Default number of output pins
bool outputStates[MAX_OUTPUT_PINS] = {};

uint8_t inputPins[MAX_INPUT_PINS] = { 14, 26, 27, 33, 0, 0, 0, 0 };
uint8_t inputPinCount = 4; // Default number of input pins
bool inputStates[MAX_INPUT_PINS] = {};

// Zone configuration
Zone zones[MAX_ZONES] = {};
uint8_t zoneCount = 0;

const uint8_t STATUS_LED_PIN = outputPins[0];

const uint8_t resetButtonPin = 0;
const int resetButtonSamplingFrequency = 300;
unsigned long resetButtonLastPressMillis = millis();

const int colourCycleProcessFrequency = 300;
unsigned long colourCycleLastProcessedMillis = millis();
int colourCycleOffset = 0;

const int pinStateCheckInterval = 250;
unsigned long pinStateCheckLastMillis = millis();

const int fujitsuConnectionTimeout = 2000;
unsigned long fujitsuLastConnected = millis();

String htmlWiFiConfigCaptivePortal = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Baum's AC Module Config</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f4f4f4; }
  .container { background-color: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 0 10px rgba(0,0,0,0.1); max-width: 500px; margin: auto; }
  h2 { color: #333; text-align: center; }
  label { display: block; margin-bottom: 8px; color: #555; }
  input[type="text"], input[type="password"] {
    width: calc(100% - 22px); padding: 10px; margin-bottom: 20px; border: 1px solid #ddd; border-radius: 4px;
  }
  input[type="submit"] {
    background-color: #007bff; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; width: 100%;
  }
  input[type="submit"]:hover { background-color: #0056b3; }
</style>
</head><body>
<div class="container">
  <h2>WiFi Configuration</h2>
  <form action="/savewificonfig" method="POST">
    <label for="ssid">SSID:</label>
    <input type="text" id="ssid" name="ssid" required><br>
    <label for="pass">Password:</label>
    <input type="password" id="pass" name="pass"><br>
    <input type="submit" value="Save & Connect">
  </form>
</div>
</body></html>
)rawliteral";

// --- Forward declarations for functions used in setup/AP mode ---
void processRootRoute(AsyncWebServerRequest *request);
void processApiStatusRoute(AsyncWebServerRequest *request);
void processColourLEDControl(AsyncWebServerRequest *request, String setting, String value);
void processBuzzerControl(AsyncWebServerRequest *request, String setting, String value);
void processOutputPinControl(AsyncWebServerRequest *request, String pinStr, String valueStr);
void processACControl(AsyncWebServerRequest *request, String setting, String value);
void process404(AsyncWebServerRequest *request);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
String buildHtmlPage(); // Keep existing HTML builder
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
// --- End Forward declarations ---


void initOutputPin(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

void initInputPin(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
}

String ACModeToString(ACMode mode) {
  switch (mode) {
    case ACMode::FAN: return "Fan";
    case ACMode::DRY: return "Dry";
    case ACMode::COOL: return "Cool";
    case ACMode::HEAT: return "Heat";
    case ACMode::AUTO: return "Auto";
    default: return "Unknown";
  }
}

String ACFanModeToString(ACFanMode mode) {
  switch (mode) {
    case ACFanMode::FAN_AUTO: return "Auto";
    case ACFanMode::FAN_QUIET: return "Quiet";
    case ACFanMode::FAN_LOW: return "Low";
    case ACFanMode::FAN_MEDIUM: return "Medium";
    case ACFanMode::FAN_HIGH: return "High";
    default: return "Unknown";
  }
}

int findOutputIndexByPin(uint8_t pin) {
  for (int i = 0; i < ARRAY_SIZE(outputPins); i++) {
    if (pin == outputPins[i]) return i;
  }
  return -1;
}

int findInputIndexByPin(uint8_t pin) {
  for (int i = 0; i < ARRAY_SIZE(inputPins); i++) {
    if (pin == inputPins[i]) return i;
  }
  return -1;
}

int findZoneById(String id) {
  for (int i = 0; i < zoneCount; i++) {
    if (zones[i].id == id) return i;
  }
  return -1;
}

int findZoneByInputPin(uint8_t pin) {
  for (int i = 0; i < zoneCount; i++) {
    if (zones[i].inputPin == pin) return i;
  }
  return -1;
}

int findZoneByOutputPin(uint8_t pin) {
  for (int i = 0; i < zoneCount; i++) {
    if (zones[i].outputPin == pin) return i;
  }
  return -1;
}

bool getZoneState(int zoneIndex) {
  if (zoneIndex < 0 || zoneIndex >= zoneCount) return false;

  int inputIndex = findInputIndexByPin(zones[zoneIndex].inputPin);
  if (inputIndex == -1) return false;

  return inputStates[inputIndex];
}

void simulateButtonPressWithNegation(uint8_t pin, int delayMs) {
  int idx = findOutputIndexByPin(pin);
  if (idx == -1) return;
  bool level = outputStates[idx];
  digitalWrite(pin, level ? LOW : HIGH);
  delay(delayMs);
  digitalWrite(pin, level ? HIGH : LOW);
}

void toggleZone(int zoneIndex) {
  if (zoneIndex < 0 || zoneIndex >= zoneCount) return;

  simulateButtonPressWithNegation(zones[zoneIndex].outputPin, 350);
}

String buildACControlHTML() {
  String acState = fujitsu.getOnOff() ? "On" : "Off";
  String acMode = ACModeToString(static_cast<ACMode>(fujitsu.getMode()));
  String acFanMode = ACFanModeToString(static_cast<ACFanMode>(fujitsu.getFanMode()));
  String acTemp = String((int)fujitsu.getTemp());
  String html = "";
  html += "<label id=\"acstatelabel\">Power: " + acState + "</label>&nbsp;<input id=\"acstatetoggle\" type=\"button\" value=\"Toggle\" data-state=\"" + String(fujitsu.getOnOff() ? "1" : "0") + "\" onclick=\"submitApiRequest('/api/ac/power/' + (this.dataset.state === '1' ? '0' : '1')); return false;\">";
  html += "<br/>&nbsp;<br/><label for=\"acmodeselect\">AC Mode:</label>&nbsp;";
  html += "<select id=\"acmodeselect\" onchange=\"submitApiRequest('/api/ac/mode/' + this.value.toLowerCase()); return false;\">";
  html += String("<option value=\"Fan\"") + (acMode == "Fan" ? " selected" : "") + ">Fan</option>";
  html += String("<option value=\"Heat\"") + (acMode == "Heat" ? " selected" : "") + ">Heat</option>";
  html += String("<option value=\"Dry\"") + (acMode == "Dry" ? " selected" : "") + ">Dry</option>";
  html += String("<option value=\"Cool\"") + (acMode == "Cool" ? " selected" : "") + ">Cool</option>";
  html += String("<option value=\"Auto\"") + (acMode == "Auto" ? " selected" : "") + ">Auto</option></select>";
  html += "<br/>&nbsp;<br/><label for=\"acfanmodeselect\">Fan Mode:</label>&nbsp;";
  html += "<select id=\"acfanmodeselect\" onchange=\"submitApiRequest('/api/ac/fan/' + this.value.toLowerCase()); return false;\">";
  html += String("<option value=\"Quiet\"") + (acFanMode == "Quiet" ? " selected" : "") + ">Quiet</option>";
  html += String("<option value=\"Low\"") + (acFanMode == "Low" ? " selected" : "") + ">Low</option>";
  html += String("<option value=\"Medium\"") + (acFanMode == "Medium" ? " selected" : "") + ">Medium</option>";
  html += String("<option value=\"High\"") + (acFanMode == "High" ? " selected" : "") + ">High</option>";
  html += String("<option value=\"Auto\"") + (acFanMode == "Auto" ? " selected" : "") + ">Auto</option></select>";
  html += "<br/>&nbsp;<br/><label for=\"actempinput\">Temperature:" + acTemp + "</label>&nbsp;";
  html += "<input type=\"text\" id=\"actempinput\" value=\"" + acTemp + "\" /><input type=\"button\" value=\"Set\" onclick=\"submitApiRequest('/api/ac/temp/' + getElementById('actempinput').value); return false;\">";
  return html;
}

String buildColourLEDControlHTML() {
  String ledState = colourLEDState ? "ON" : "OFF";
  String buttonClass = colourLEDState ? "button" : "button button-off";
  String html = "";
  html += "<div><a id=\"colourled\" data-state=\"" + String(colourLEDState ? "1" : "0");
  html += "\" data-brightness=\"" + String(colourLEDBrightness) + "\" href=\"javascript:void(0)\" onclick=\"submitApiRequest('/api/colourled/state/' + (parseInt(this.dataset.state) ? 0 : 1));return false;\" class=\"";
  html += buttonClass + "\">Pin Colour LED: " + ledState + "</a></div>";
  html += "<div><label>Brightness:&nbsp;</label><input id=\"colourLEDBrightness\" type=\"number\" value=\"" + String(colourLEDBrightness) + "\"><input type=\"button\" value=\"Set\" onclick=\"submitApiRequest('/api/colourled/brightness/' + getElementById('colourLEDBrightness').value);return false;\"></div>";
  return html;
}

String buildBuzzerControlHTML() {
  String html = "";
  html += "<div><label>Volume:&nbsp;</label><input id=\"buzzerVolume\" type=\"number\" min=\"0\" max=\"255\" value=\"" + String(INITIAL_BUZZER_VOLUME) + "\"><input type=\"button\" value=\"Set\" onclick=\"submitApiRequest('/api/buzzer/volume/' + getElementById('buzzerVolume').value);return false;\"></div>";
  html += "<br/>";
  html += "<div><label>Test Tune:&nbsp;</label><input id=\"buzzerTune\" type=\"number\" min=\"0\" max=\"13\"\"><input type=\"button\" value=\"Test Buzzer\" onclick=\"submitApiRequest('/api/buzzer/test/' + getElementById('buzzerTune').value);return false;\" class=\"button button-off\"></div>";
  return html;
}

String buildMQTTControlHTML() {
    String html = "<div>";
    html += "<label for=\"mqtt_broker\">MQTT Broker:</label><input type=\"text\" id=\"mqtt_broker\" name=\"mqtt_broker\" value=\"" + String(mqttBroker) + "\"><br>";
    html += "<label for=\"mqtt_port\">MQTT Port:</label><input type=\"number\" id=\"mqtt_port\" name=\"mqtt_port\" value=\"" + String(mqttPort) + "\"><br>";
    html += "<label for=\"mqtt_user\">MQTT User:</label><input type=\"text\" id=\"mqtt_user\" name=\"mqtt_user\" value=\"" + String(mqttUser) + "\"><br>";
    html += "<label for=\"mqtt_pass\">MQTT Password:</label><input type=\"password\" id=\"mqtt_pass\" name=\"mqtt_pass\" value=\"" + String(mqttPassword) + "\"><br>";
    html += "<label for=\"mqtt_topic\">MQTT Topic:</label><input type=\"text\" id=\"mqtt_topic\" name=\"mqtt_topic\" value=\"" + String(mqttBaseTopic) + "\"><br>";
    html += "<input type=\"button\" value=\"Save MQTT Config\" onclick=\"saveMqttConfig()\">";
    html += "</div>";
    html += "<script>";
    html += "function saveMqttConfig() {";
    html += "  const broker = document.getElementById('mqtt_broker').value;";
    html += "  const port = document.getElementById('mqtt_port').value;";
    html += "  const user = document.getElementById('mqtt_user').value;";
    html += "  const pass = document.getElementById('mqtt_pass').value;";
    html += "  const topic = document.getElementById('mqtt_topic').value;";
    html += "  const url = `/api/mqtt/save?broker=${encodeURIComponent(broker)}&port=${port}&user=${encodeURIComponent(user)}&pass=${encodeURIComponent(pass)}&topic=${encodeURIComponent(topic)}`;";
    html += "  fetch(url, { method: 'POST' })";
    html += "    .then(response => response.json())";
    html += "    .then(data => { console.log('MQTT Config Saved:', data); alert('MQTT Config Saved. The device will now restart.'); ESP.restart(); })";
    html += "    .catch(error => { console.error('Error saving MQTT config:', error); });";
    html += "}";
    html += "</script>";
    return html;
}

String buildOutputPinsHTML() {
  String buttons = "";
  for (int i = 0; i < outputPinCount; i++) {
    String pinState = outputStates[i] ? "ON" : "OFF";
    String buttonClass = outputStates[i] ? "button" : "button button-off";
    buttons += "<div><a id=\"outPin" + String(outputPins[i]) + "\" data-state=\"" + String(outputStates[i] ? "1" : "0");
    buttons += "\" href=\"javascript:void(0)\" onclick=\"submitApiRequest('/api/out/" + String(outputPins[i]) + "/' + (parseInt(this.dataset.state) ? 0 : 1)); return false;\" class=\"";
    buttons += buttonClass + "\">Pin " + String(outputPins[i]) + ": " + pinState + "</a>&nbsp;-&nbsp;";
    buttons += "<a href=\"javascript:void(0)\" onclick=\"submitApiRequest('/api/out/" + String(outputPins[i]) + "/press'); return false;\" class=\"button button-off\">Pin ";
    buttons += String(outputPins[i]) + ": PRESS</a></div>";
  }
  return buttons;
}

String buildInputPinsHTML() {
  String buttons = "";
  for (int i = 0; i < inputPinCount; i++) {
    bool state = digitalRead(inputPins[i]) == HIGH;
    String buttonClass = state ? "button" : "button button-off";
    buttons += "<div><a id=\"inPin" + String(inputPins[i]) + "\" href=\"javascript:void(0)\" class=\"" + buttonClass + "\">Pin " +
      String(inputPins[i]) + ": " + (state ? "ON" : "OFF") + "</a></div>";
  }
  return buttons;
}

String buildZoneControlHTML() {
  String buttons = "";
  for (int i = 0; i < zoneCount; i++) {
    bool state = getZoneState(i);
    String buttonClass = state ? "button" : "button button-off";
    buttons += "<div><a id=\"zone" + zones[i].id + "\" data-id=\"" + zones[i].id + "\" data-state=\"" + String(state ? "1" : "0");
    buttons += "\" href=\"javascript:void(0)\" onclick=\"submitApiRequest('/api/zone/" + zones[i].id + "/toggle'); return false;\" class=\"";
    buttons += buttonClass + "\">Zone " + zones[i].id + ": " + (state ? "ON" : "OFF") + "</a></div>";
  }
  return buttons;
}

String buildZoneConfigHTML() {
  String html = "<div>";
  html += "<h4>Zone Configuration</h4>";
  html += "<div id=\"zones_container\">";
  for (int i = 0; i < MAX_ZONES; i++) {
    String zoneId = (i < zoneCount) ? zones[i].id : "";
    String inputPin = (i < zoneCount) ? String(zones[i].inputPin) : "";
    String outputPin = (i < zoneCount) ? String(zones[i].outputPin) : "";

    html += "<div class=\"zone-input\">";
    html += "<label for=\"zone_id_" + String(i) + "\">Zone " + String(i+1) + " ID:</label>";
    html += "<input type=\"text\" id=\"zone_id_" + String(i) + "\" name=\"zone_id_" + String(i) + "\" value=\"" + zoneId + "\">";
    html += "<label for=\"zone_input_pin_" + String(i) + "\">Input Pin:</label>";
    html += "<input type=\"number\" id=\"zone_input_pin_" + String(i) + "\" name=\"zone_input_pin_" + String(i) + "\" value=\"" + inputPin + "\">";
    html += "<label for=\"zone_output_pin_" + String(i) + "\">Output Pin:</label>";
    html += "<input type=\"number\" id=\"zone_output_pin_" + String(i) + "\" name=\"zone_output_pin_" + String(i) + "\" value=\"" + outputPin + "\">";
    html += "</div>";
  }
  html += "</div>";
  html += "<br><input type=\"button\" value=\"Save Zone Configuration\" onclick=\"saveZoneConfig()\">";
  html += "</div>";

  html += "<script>";
  html += "function saveZoneConfig() {";
  html += "  let zones = [];";
  html += "  for (let i = 0; i < " + String(MAX_ZONES) + "; i++) {";
  html += "    const zoneId = document.getElementById('zone_id_' + i).value;";
  html += "    const inputPin = document.getElementById('zone_input_pin_' + i).value;";
  html += "    const outputPin = document.getElementById('zone_output_pin_' + i).value;";
  html += "    if (zoneId && zoneId.trim() !== '' && inputPin && inputPin.trim() !== '' && outputPin && outputPin.trim() !== '') {";
  html += "      zones.push({";
  html += "        id: zoneId,";
  html += "        inputPin: parseInt(inputPin),";
  html += "        outputPin: parseInt(outputPin)";
  html += "      });";
  html += "    }";
  html += "  }";
  html += "  const url = `/api/zones/save`;";
  html += "  fetch(url, {";
  html += "    method: 'POST',";
  html += "    headers: { 'Content-Type': 'application/json' },";
  html += "    body: JSON.stringify({ zones: zones })";
  html += "  })";
  html += "  .then(response => response.json())";
  html += "  .then(data => { console.log('Zone Config Saved:', data); alert('Zone Configuration Saved. The device will now restart.'); })";
  html += "  .catch(error => { console.error('Error saving zone config:', error); });";
  html += "}";
  html += "</script>";

  return html;
}

String buildPinConfigHTML() {
  String html = "<div>";

  // AC RX/TX Pins
  html += "<h4>AC Communication Pins</h4>";
  html += "<label for=\"ac_rx_pin\">AC RX Pin:</label><input type=\"number\" id=\"ac_rx_pin\" name=\"ac_rx_pin\" value=\"" + String(acRxPin) + "\"><br>";
  html += "<label for=\"ac_tx_pin\">AC TX Pin:</label><input type=\"number\" id=\"ac_tx_pin\" name=\"ac_tx_pin\" value=\"" + String(acTxPin) + "\"><br>";

  // Output Pins
  html += "<h4>Output Pins</h4>";
  html += "<div id=\"output_pins_container\">";
  for (int i = 0; i < MAX_OUTPUT_PINS; i++) {
    String pinValue = (i < outputPinCount) ? String(outputPins[i]) : "";
    html += "<div class=\"pin-input\">";
    html += "<label for=\"output_pin_" + String(i) + "\">Output Pin " + String(i+1) + ":</label>";
    html += "<input type=\"number\" id=\"output_pin_" + String(i) + "\" name=\"output_pin_" + String(i) + "\" value=\"" + pinValue + "\">";
    html += "</div>";
  }
  html += "</div>";

  // Input Pins
  html += "<h4>Input Pins</h4>";
  html += "<div id=\"input_pins_container\">";
  for (int i = 0; i < MAX_INPUT_PINS; i++) {
    String pinValue = (i < inputPinCount) ? String(inputPins[i]) : "";
    html += "<div class=\"pin-input\">";
    html += "<label for=\"input_pin_" + String(i) + "\">Input Pin " + String(i+1) + ":</label>";
    html += "<input type=\"number\" id=\"input_pin_" + String(i) + "\" name=\"input_pin_" + String(i) + "\" value=\"" + pinValue + "\">";
    html += "</div>";
  }
  html += "</div>";

  html += "<br><input type=\"button\" value=\"Save Pin Configuration\" onclick=\"savePinConfig()\">";
  html += "</div>";

  html += "<script>";
  html += "function savePinConfig() {";
  html += "  const acRxPin = document.getElementById('ac_rx_pin').value;";
  html += "  const acTxPin = document.getElementById('ac_tx_pin').value;";

  html += "  let outputPins = [];";
  html += "  for (let i = 0; i < " + String(MAX_OUTPUT_PINS) + "; i++) {";
  html += "    const pinValue = document.getElementById('output_pin_' + i).value;";
  html += "    if (pinValue && pinValue.trim() !== '') {";
  html += "      outputPins.push(parseInt(pinValue));";
  html += "    }";
  html += "  }";

  html += "  let inputPins = [];";
  html += "  for (let i = 0; i < " + String(MAX_INPUT_PINS) + "; i++) {";
  html += "    const pinValue = document.getElementById('input_pin_' + i).value;";
  html += "    if (pinValue && pinValue.trim() !== '') {";
  html += "      inputPins.push(parseInt(pinValue));";
  html += "    }";
  html += "  }";

  html += "  const url = `/api/pins/save`;";
  html += "  fetch(url, {";
  html += "    method: 'POST',";
  html += "    headers: { 'Content-Type': 'application/json' },";
  html += "    body: JSON.stringify({";
  html += "      acRxPin: parseInt(acRxPin),";
  html += "      acTxPin: parseInt(acTxPin),";
  html += "      outputPins: outputPins,";
  html += "      inputPins: inputPins";
  html += "    })";
  html += "  })";
  html += "  .then(response => response.json())";
  html += "  .then(data => { console.log('Pin Config Saved:', data); alert('Pin Configuration Saved. The device will now restart.'); })";
  html += "  .catch(error => { console.error('Error saving pin config:', error); });";
  html += "}";
  html += "</script>";

  return html;
}

String buildHtmlPage() {
  String html = "<!DOCTYPE html><html>";
  html += "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<link rel=\"icon\" href=\"data:,\">";
  html += "<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center; }";
  html += ".button { background-color: yellow; border: none; color: grey; padding: 8px 8px;";
  html += "text-decoration: none; font-size: 11px; margin: 2px; cursor: pointer; }";
  html += ".button-off { background-color: #efefef; }";
  html += ".button-container { display: inline-grid; grid-template-columns: auto auto auto; }</style>";
  html += "<script>";
  html += "async function reloadCurrentStateAsync() {";
  html += "  const state = await getCurrentState();";
  html += "  updateUIFromState(state);";
  html += "}";
  html += "function submitApiRequest(endpoint, method='POST') {";
  html += "  const baseUrl = window.location.origin;";
  html += "  const url = baseUrl + endpoint;";
  html += "  fetch(url, { method: method })";
  html += "    .then(response => response.json())";
  html += "    .then(data => {";
  html += "      /* console.log('Success:', data); */";
  html += "      /* reloadCurrentStateAsync(); */ /* rely on ws instead */";
  html += "    })";
  html += "    .catch(error => { console.error('Error:', error); reloadCurrentStateAsync(); });";
  html += "  return false;";
  html += "}";
  html += "function getCurrentState() {";
  html += "  const baseUrl = window.location.origin;";
  html += "  const apiUrl = baseUrl + \"/api/status\";";
  html += "  return fetch(apiUrl, { method: 'GET' })";
  html += "    .then(response => response.json())";
  html += "    .catch(error => { console.error('Error getting state:', error); });";
  html += "}";
  html += "let socket;";
  html += "function connectWebSocket() {";
  html += "  const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';";
  html += "  socket = new WebSocket(wsProtocol + '//' + window.location.host + '/ws');";
  html += "  socket.onopen = function(event) { console.log('WebSocket connected'); };";
  html += "  socket.onmessage = function(event) {";
  html += "    try {";
  html += "      const data = JSON.parse(event.data);";
  html += "      if (data.type === 'log') {";
  html += "        console.log('Processing received ws log:', data);";
  html += "        const logContainer = document.getElementById('log-container');";
  html += "        const logEntry = document.createElement('div');";
  html += "        logEntry.textContent = data.message;";
  html += "        logContainer.appendChild(logEntry);";
  html += "        logContainer.scrollTop = logContainer.scrollHeight;";
  html += "      } else if (data.type === 'mqtt_log') {";
  html += "        console.log('Processing received ws mqtt_log:', data);";
  html += "        const mqttLogContainer = document.getElementById('mqtt-log-container');";
  html += "        const logEntry = document.createElement('div');";
  html += "        logEntry.textContent = data.message;";
  html += "        mqttLogContainer.appendChild(logEntry);";
  html += "        mqttLogContainer.scrollTop = mqttLogContainer.scrollHeight;";
  html += "      } else {";
  html += "        console.log('Processing received ws state:', data);";
  html += "        updateUIFromState(data);";
  html += "      }";
  html += "    } catch (e) { console.error('Error parsing WebSocket message:', e); }";
  html += "  };";
  html += "  socket.onclose = function(event) {";
  html += "    console.log('WebSocket disconnected, attempting to reconnect...');";
  html += "    if (!document.hidden) {";
  html += "      setTimeout(connectWebSocket, 3000);";
  html += "    }";
  html += "  };";
  html += "  socket.onerror = function(error) { console.error('WebSocket error:', error); socket.close(); };";
  html += "}";
  html += "function updateUIFromState(state) {";
  html += "  if (!state) { console.error('updateUIFromState called with null state'); return; }";
  html += "  if (state.outputs) {";
  html += "    for (var i = 0; i < state.outputs.length; i++) {";
  html += "      const outPinElement = document.getElementById('outPin' + state.outputs[i].pin);";
  html += "      if (outPinElement) {";
  html += "        outPinElement.dataset.state = state.outputs[i].state ? '1' : '0';";
  html += "        outPinElement.innerText = 'Pin ' + state.outputs[i].pin + ': ' + (state.outputs[i].state ? 'ON' : 'OFF');";
  html += "        if (state.outputs[i].state) { outPinElement.classList.remove('button-off'); }";
  html += "        else { outPinElement.classList.add('button-off'); }";
  html += "      }";
  html += "    }";
  html += "  }";
  html += "  if (state.colourled) {";
  html += "    const colourLedElement = document.getElementById('colourled');";
  html += "    if (colourLedElement) {";
  html += "      colourLedElement.dataset.state = state.colourled.state ? '1' : '0';";
  html += "      colourLedElement.dataset.brightness = state.colourled.brightness;";
  html += "      colourLedElement.innerText = 'Pin Colour LED: ' + (state.colourled.state ? 'ON' : 'OFF');";
  html += "      if (state.colourled.state) { colourLedElement.classList.remove('button-off'); }";
  html += "      else { colourLedElement.classList.add('button-off'); }";
  html += "    }";
  html += "    const brightnessInput = document.getElementById('colourLEDBrightness');";
  html += "    if (brightnessInput) brightnessInput.value = state.colourled.brightness;";
  html += "  }";
  html += "  if (state.buzzer) {";
  html += "    const buzzerVolumeInput = document.getElementById('buzzerVolume');";
  html += "    if (buzzerVolumeInput) buzzerVolumeInput.value = state.buzzer.volume;";
  html += "  }";
  html += "  if (state.ac) {";
  html += "    const acStateToggle = document.getElementById('acstatetoggle');";
  html += "    if (acStateToggle) {";
  html += "      acStateToggle.dataset.state = state.ac.power ? '1' : '0';";
  html += "      const acStateLabel = document.getElementById('acstatelabel');";
  html += "      if (acStateLabel) { acStateLabel.innerHTML = 'Power: ' + (state.ac.power ? 'On' : 'Off'); }";
  html += "    }";
  html += "    const acModeSelect = document.getElementById('acmodeselect');";
  html += "    if (acModeSelect) acModeSelect.value = state.ac.mode;";
  html += "    const acFanModeSelect = document.getElementById('acfanmodeselect');";
  html += "    if (acFanModeSelect) acFanModeSelect.value = state.ac.fanMode;";
  html += "    const acTempInput = document.getElementById('actempinput');";
  html += "    if (acTempInput) acTempInput.value = state.ac.temp;";
  html += "    const acTempLabel = document.querySelector('label[for=\"actempinput\"]');";
  html += "    if (acTempLabel) acTempLabel.innerHTML = 'Temperature: ' + state.ac.temp;";
  html += "  }";
  html += "  if (state.inputs) {";
  html += "    for (var i = 0; i < state.inputs.length; i++) {";
  html += "      const inPinElement = document.getElementById('inPin' + state.inputs[i].pin);";
  html += "      if (inPinElement) {";
  html += "        inPinElement.innerText = 'Pin ' + state.inputs[i].pin + ': ' + (state.inputs[i].state ? 'ON' : 'OFF');";
  html += "        if (state.inputs[i].state) { inPinElement.classList.remove('button-off'); }";
  html += "        else { inPinElement.classList.add('button-off'); }";
  html += "      }";
  html += "    }";
  html += "  }";
  html += "  if (state.zones) {";
  html += "    for (var i = 0; i < state.zones.length; i++) {";
  html += "      const zoneElement = document.getElementById('zone' + state.zones[i].id);";
  html += "      if (zoneElement) {";
  html += "        zoneElement.dataset.state = state.zones[i].state ? '1' : '0';";
  html += "        zoneElement.innerText = 'Zone ' + state.zones[i].id + ': ' + (state.zones[i].state ? 'ON' : 'OFF');";
  html += "        if (state.zones[i].state) { zoneElement.classList.remove('button-off'); }";
  html += "        else { zoneElement.classList.add('button-off'); }";
  html += "      }";
  html += "    }";
  html += "  }";
  html += "}";
  html += "document.addEventListener('DOMContentLoaded', function() {";
  html += "  reloadCurrentStateAsync().then(() => connectWebSocket());";
  html += "});";
  html += "</script></head>";
  html += String("<body><h1>Fujitsu AC & Zone Controller</h1><h3>") + CONTROLLER_VERSION + String("</h3>");
  html += "<h3>Update Current State</h3><div class=\"button-container\"><input type=\"button\" value=\"Refresh\" onclick=\"reloadCurrentStateAsync();return false;\"></div>";
  html += "<h3>Fujitsu AC Controller Status</h3><div>" + buildACControlHTML() + "</div>";
  html += "<h3>MQTT Configuration</h3><div>" + buildMQTTControlHTML() + "</div>";
  html += "<h3>Pin Configuration</h3><div>" + buildPinConfigHTML() + "</div>";
  html += "<h3>Colour Cycling LED Control</h3><div class=\"button-container\">" + buildColourLEDControlHTML() + "</div>";
  html += "<h3>Buzzer Control</h3><div class=\"button-container\">" + buildBuzzerControlHTML() + "</div>";
  html += "<h3>Zone Configuration</h3><div>" + buildZoneConfigHTML() + "</div>";
  html += "<h3>Zone Control</h3><div class=\"button-container\">" + buildZoneControlHTML() + "</div>";
  html += "<h3>Available GPIO Output Control Pins</h3><div class=\"button-container\">" + buildOutputPinsHTML() + "</div>";
  html += "<h3>Available Input Control Pins</h3><div class=\"button-container\">" + buildInputPinsHTML() + "</div>";
  html += "<h3><a href=\"/update\">Upload new firmware</a></h3>";
  html += "<h3>MQTT Messages</h3><div id=\"mqtt-log-container\" style=\"height: 200px; overflow-y: scroll; border: 1px solid #ccc; padding: 10px; text-align: left; background-color: #f9f9f9;\"></div>";
  html += "<input type=\"button\" value=\"Clear logs\" onclick=\"getElementById('mqtt-log-container').innerHTML = '';return false;\"/>";
  html += "<h3>Device Logs</h3><div id=\"log-container\" style=\"height: 200px; overflow-y: scroll; border: 1px solid #ccc; padding: 10px; text-align: left; background-color: #f9f9f9;\"></div>";
  html += "</body></html>";
  return html;
}

String getStaticWebApp() {
  // Check if SPIFFS is available and has the index.html file
  if (SPIFFS.begin(true)) {
    if (SPIFFS.exists("/index.html")) {
      // Redirect to the static file instead of serving it directly
      // This allows the browser to cache the file and load resources properly
      return "<html><head><meta http-equiv=\"refresh\" content=\"0;url=/index.html\"></head><body>Redirecting...</body></html>";
    }
  }

  // If SPIFFS is not available or index.html doesn't exist, use the error page
  return "<html><head>AC Controller UI Is Not Available</head><body>And error occured while trying to serve AC Controller's UI using on chip file system.</body></html>";
}

void processRootRoute(AsyncWebServerRequest *request) {
  request->send(200, "text/html", getStaticWebApp());
}

String buildCurrentStatePayload(bool includeConfigs = false) {
  JsonDocument doc;
  JsonArray outputsConfig;
  JsonArray inputsConfig;
  JsonArray zonesConfig;
  doc["version"] = CONTROLLER_VERSION;
  if (includeConfigs) {
    doc["config"]["ac"]["rxPin"] = acRxPin;
    doc["config"]["ac"]["txPin"] = acTxPin;
    doc["config"]["mqtt"]["brokerUrl"] = mqttBroker;
    doc["config"]["mqtt"]["brokerPort"] = mqttPort;
    doc["config"]["mqtt"]["username"] = mqttUser;
    doc["config"]["mqtt"]["password"] = mqttPassword;
    doc["config"]["mqtt"]["baseTopic"] = mqttBaseTopic;
    outputsConfig = doc["config"]["outputs"].to<JsonArray>();
    inputsConfig = doc["config"]["inputs"].to<JsonArray>();
    zonesConfig = doc["config"]["zones"].to<JsonArray>();
  }
  doc["ac"]["power"] = fujitsu.getOnOff();
  doc["ac"]["mode"] = ACModeToString(static_cast<ACMode>(fujitsu.getMode()));
  doc["ac"]["fanMode"] = ACFanModeToString(static_cast<ACFanMode>(fujitsu.getFanMode()));
  doc["ac"]["temp"] = fujitsu.getTemp();

  JsonArray outputs = doc["outputs"].to<JsonArray>();
  for (int i = 0; i < outputPinCount; i++) {
    if (includeConfigs) outputsConfig.add(String(outputPins[i]));
    JsonObject output = outputs.add<JsonObject>();
    output["pin"] = String(outputPins[i]);
    output["state"] = outputStates[i];
  }

  JsonArray inputs = doc["inputs"].to<JsonArray>();
  for (int i = 0; i < inputPinCount; i++) {
    if (includeConfigs) inputsConfig.add(String(inputPins[i]));
    JsonObject input = inputs.add<JsonObject>();
    input["pin"] = String(inputPins[i]);
    input["state"] = inputStates[i];
  }

  JsonArray zonesArray = doc["zones"].to<JsonArray>();
  for (int i = 0; i < zoneCount; i++) {
    if (includeConfigs) {
      JsonObject zoneConfig = zonesConfig.add<JsonObject>();
      zoneConfig["id"] = zones[i].id;
      zoneConfig["inputPin"] = zones[i].inputPin;
      zoneConfig["outputPin"] = zones[i].outputPin;
    }
    JsonObject zone = zonesArray.add<JsonObject>();
    zone["id"] = zones[i].id;
    zone["state"] = getZoneState(i);
  }
  JsonObject colourled = doc["colourled"].to<JsonObject>();
  colourled["state"] = colourLEDState;
  colourled["brightness"] = colourLEDBrightness; // Already uint8_t, no need for String()
  JsonObject buzzer = doc["buzzer"].to<JsonObject>();
  buzzer["volume"] = INITIAL_BUZZER_VOLUME;
  String payload;
  serializeJson(doc, payload);
  return payload;
}

void processApiStatusRoute(AsyncWebServerRequest *request) {
  bool includeConfig = false;
  if (request->hasParam("includeConfig")) includeConfig = request->getParam("includeConfig")->value() == "true";
  request->send(200, "application/json", buildCurrentStatePayload(includeConfig));
}

void processSaveMqttConfigRoute(AsyncWebServerRequest *request) {
    preferences.begin("mqtt-config", false);
    if (request->hasParam("broker")) preferences.putString(PREF_KEY_MQTT_BROKER, request->getParam("broker")->value());
    if (request->hasParam("port")) preferences.putInt(PREF_KEY_MQTT_PORT, request->getParam("port")->value().toInt());
    if (request->hasParam("user")) preferences.putString(PREF_KEY_MQTT_USER, request->getParam("user")->value());
    if (request->hasParam("pass")) preferences.putString(PREF_KEY_MQTT_PASS, request->getParam("pass")->value());
    if (request->hasParam("topic")) preferences.putString(PREF_KEY_MQTT_TOPIC, request->getParam("topic")->value());
    preferences.end();
    request->send(200, "application/json", "{\"success\":true}");
    delay(1000);
    ESP.restart();
}

void processSaveZoneConfigRoute(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (len + index == total) {
        // Parse the JSON data
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        // Extract the zone configurations
        JsonArray zonesArray = doc["zones"];
        uint8_t newZoneCount = zonesArray.size();
        if (newZoneCount > MAX_ZONES) {
            newZoneCount = MAX_ZONES;
        }

        Zone newZones[MAX_ZONES];
        for (int i = 0; i < newZoneCount; i++) {
            newZones[i].id = zonesArray[i]["id"].as<String>();
            newZones[i].inputPin = zonesArray[i]["inputPin"];
            newZones[i].outputPin = zonesArray[i]["outputPin"];
        }

        // Save the zone configurations to preferences
        preferences.begin("zone-config", false);

        // Save zones as a JSON string
        JsonDocument zonesDoc;
        JsonArray zonesJsonArray = zonesDoc.to<JsonArray>();
        for (int i = 0; i < newZoneCount; i++) {
            JsonObject zoneObj = zonesJsonArray.add<JsonObject>();
            zoneObj["id"] = newZones[i].id;
            zoneObj["inputPin"] = newZones[i].inputPin;
            zoneObj["outputPin"] = newZones[i].outputPin;
        }

        String zonesStr;
        serializeJson(zonesDoc, zonesStr);
        preferences.putString(PREF_KEY_ZONES, zonesStr);
        preferences.end();

        // Update the current zones
        zoneCount = newZoneCount;
        for (int i = 0; i < zoneCount; i++) {
            zones[i] = newZones[i];
        }

        request->send(200, "application/json", "{\"success\":true}");
    }
}

void processSavePinConfigRoute(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (len + index == total) {
        // Parse the JSON data
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        // Extract the pin configurations
        uint8_t newAcRxPin = doc["acRxPin"];
        uint8_t newAcTxPin = doc["acTxPin"];

        // Extract output pins
        JsonArray outputPinsArray = doc["outputPins"];
        uint8_t newOutputPinCount = outputPinsArray.size();
        if (newOutputPinCount > MAX_OUTPUT_PINS) {
            newOutputPinCount = MAX_OUTPUT_PINS;
        }

        uint8_t newOutputPins[MAX_OUTPUT_PINS] = {0};
        for (int i = 0; i < newOutputPinCount; i++) {
            newOutputPins[i] = outputPinsArray[i];
        }

        // Extract input pins
        JsonArray inputPinsArray = doc["inputPins"];
        uint8_t newInputPinCount = inputPinsArray.size();
        if (newInputPinCount > MAX_INPUT_PINS) {
            newInputPinCount = MAX_INPUT_PINS;
        }

        uint8_t newInputPins[MAX_INPUT_PINS] = {0};
        for (int i = 0; i < newInputPinCount; i++) {
            newInputPins[i] = inputPinsArray[i];
        }

        // Save the pin configurations to preferences
        preferences.begin("pin-config", false);
        preferences.putUChar(PREF_KEY_AC_RX_PIN, newAcRxPin);
        preferences.putUChar(PREF_KEY_AC_TX_PIN, newAcTxPin);

        // Save output pins as a string of comma-separated values
        String outputPinsStr = "";
        for (int i = 0; i < newOutputPinCount; i++) {
            if (i > 0) outputPinsStr += ",";
            outputPinsStr += String(newOutputPins[i]);
        }
        preferences.putString(PREF_KEY_OUTPUT_PINS, outputPinsStr);

        // Save input pins as a string of comma-separated values
        String inputPinsStr = "";
        for (int i = 0; i < newInputPinCount; i++) {
            if (i > 0) inputPinsStr += ",";
            inputPinsStr += String(newInputPins[i]);
        }
        preferences.putString(PREF_KEY_INPUT_PINS, inputPinsStr);

        preferences.end();

        request->send(200, "application/json", "{\"success\":true}");
        delay(1000);
        ESP.restart();
    }
}

// Global variable to track which melody to play next
uint8_t currentMelodyIndex = 0;

Melody getMelodyByIndex(int index) {
  switch(index) {
    case 0: {
      // Twinkle Twinkle Little Star
      const int nNotes = 14;
      String notes[nNotes] = { "C4", "C4", "G4", "G4", "A4", "A4", "G4", "F4", "F4", "E4", "E4", "D4", "D4", "C4" };
      return MelodyFactory.load("Twinkle Twinkle", 400, notes, nNotes);
    }
    case 1: {
      // Take On Me (using RTTTL format)
      String rtttl = "TakeOnMe:d=4,o=4,b=160:8f#5,8f#5,8f#5,8d5,8p,8b,8p,8e5,8p,8e5,8p,8e5,8g#5,8g#5,8a5,8b5,8a5,8a5,8a5,8e5,8p,8d5,8p,8f#5,8p,8f#5,8p,8f#5,8e5,8e5,8f#5,8e5";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 2: {
      // Tetris Theme (simplified)
      const int nNotes = 20;
      String notes[nNotes] = { "E5", "B4", "C5", "D5", "C5", "B4", "A4", "A4", "C5", "E5", "D5", "C5", "B4", "B4", "C5", "D5", "E5", "C5", "A4", "A4" };
      return MelodyFactory.load("Tetris Theme", 150, notes, nNotes);
    }
    case 3: {
      // Nokia Ringtone
      const int nNotes = 13;
      String notes[nNotes] = { "E5", "D5", "F#4", "G#4", "C#5", "B4", "D4", "E4", "B4", "A4", "C#4", "E4", "A4" };
      return MelodyFactory.load("Nokia Ringtone", 180, notes, nNotes);
    }
    case 4: {
      // Half Double
      String rtttl = "HalfDouble:d=16,o=5,b=200:c6,f6,c7,c6,f6,c7,c6,f6,c7";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 5: {
      // Tripple swirl
      String rtttl = "Triiple:d=8,o=5,b=635:c,e,g,c,e,g,c,e,g,c6,e6,g6,c6,e6,g6,c6,e6,g6,c7,e7,g7,c7,e7,g7,c7,e7,g7";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 6: {
      // Pling 2
      String rtttl = "Pling2:d=16,o=7,b=140:f#7,32p,e7";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 7: {
      // Ravel - Bolero
      String rtttl = "Bolero:o=5,d=16,b=80,b=80:c6,8c6,b,c6,d6,c6,b,a,8c6,c6,a,4c6,8c6,b,c6,a,g,e,f,2g,g,f,e,d,e,f,g,a,4g,4g,g,a,b,a,g,f,e,d,e,d,8c,8c,c,d,8e,8f,4d,2g";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 8: {
      // Scale
      String rtttl = "Scale:o=5,d=32,b=160,b=160:c,d,e,f,g,a,b,c6,b,a,g,f,e,d,c";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 9: {
      // Verve (Bitter Sweet Symphony)
      String rtttl = "Verve:o=5,d=8,b=80,b=80:b4,d,b4,c,a4,c,p,f,c,f,p,e,c,e,p,b4,d,b4,c,a4,c,p,f,c,f,p,e,c,e";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 10: {
      // Stevie Wonder - I just called
      String rtttl = "IJustCalled:d=4,o=5,b=160:8c6,c6,2a,8c6,2b,8g,b,2c6,8p,8c6,c6,2a,8c6,b.,8a,g,a,2e,8p,8c6,c6,2a,8c6,2b,8g,b,2c6,8c6,c6,a.,8g,f,d,e,d,c,d,2c";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 11: {
      // Scooter - How much is that fish
      String rtttl = "HowMuchI:d=4,o=6,b=125:8g,8g,16f,16e,f,8d.,16c,8d,8g,8g,8f,8e,8g,8g,16f,16e,f,d,8e,8c,2d,8p,8d,8f,8g,a,a,8a_,8g,2a,8g,8g,16f,16e,f,d,8f,8g,8g,8f,8e,8g,8g,16f,16e,f,d,8e,8c,2d";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 12: {
      // Sonic Green Hill Zone
      String rtttl = "SonicGreenHill:d=4,o=5,b=125:16c7,32p,8a6,32p,16c7,32p,8b6,32p,16c7,32p,8b6,32p,g6,p,16a6,32p,16e7,32p,8d7,32p,16c7,32p,8b6,32p,16c7,32p,8b6,32p,g6,p,16c7,32p,8a6,32p,16c7,32p,8b6,32p,16c7,32p,8b6,32p,g6,p,16a6,32p,8f6,32p,16a6,32p,8g6,32p,16a6,32p,8g6,32p,c6";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    case 13: {
      // Sonic Chemical Plant Zone (Hydro Zone)
      String rtttl = "SonicChemicalPlant:d=4,o=5,b=125:16f#,16a,8c#6,16b,16a,16b,8a,p,16f#,16a,8c#6,16b,16a,16b,8a,16b,16p,16a,16p,16b,16p,8c#6,16a,16f#,16p,8f#,p,16f#,16a,8c#6,16b,16a,16b,8a,p,16f#,16a,8c#6,16b,16a,16b,8a,16b,16p,16a,16p,16b,16p,8c#6,16a,16f#,16p,8f#,p,16b,16p,8b,16a,16b,16p,16b,32p,16a,8b,16a,16p,16c#6,16a,16f#,p,16f#,16p,16b,16p,8b,16a,16b,16p,8b,16a,16b,16p,16a,32p,16c#6";
      return MelodyFactory.loadRtttlString(rtttl.c_str());
    }
    default: {
      // Default back to original melody
      const int nNotes = 8;
      String notes[nNotes] = { "C4", "G3", "G3", "A3", "G3", "SILENCE", "B3", "C4" };
      return MelodyFactory.load("Nice Melody", 175, notes, nNotes);
    }
  }
}

void notifyAudibleTone(uint8_t index = currentMelodyIndex) {
  // Cycle through different melodies each time this function is called
  Melody melody = getMelodyByIndex(index);

  // Move to next melody for next time (cycle through 0-13)
  currentMelodyIndex = (currentMelodyIndex + 1) % 14;

  Serial.println(String(" Title: ") + melody.getTitle());
  Serial.println(String(" Time unit: ") + melody.getTimeUnit());
  Serial.println("Start playing in non-blocking mode...");

  player.playAsync(melody);
  Serial.println("Melody is playing!");
}

void notifyWSSubscribers(String message = buildCurrentStatePayload()) {
  ws.textAll(message);
}

void notifyMqttTopics(String message = buildCurrentStatePayload()) {
    if (mqttClient.connected()) {
        mqttClient.publish((String(mqttBaseTopic) + String("/status")).c_str(), message.c_str(), true);
        Serial.println("Published message to " + String(mqttBaseTopic) + String("/status") + " with " + message);
    }
}

void notifyObservers() {
    String message = buildCurrentStatePayload();
    notifyWSSubscribers(message);
    notifyAudibleTone(4);
    notifyMqttTopics(message);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT: Serial.printf("WebSocket client #%u disconnected\n", client->id()); break;
    case WS_EVT_DATA: Serial.printf("WebSocket client #%u sent data: %s\n", client->id(), (char*)data); break;
    case WS_EVT_PONG: case WS_EVT_ERROR: break;
  }
}

void processColourLEDControl(AsyncWebServerRequest *request, String setting, String value) {
  bool changed = false;
  if (setting == "state") {
    bool newState = (value == "1");
    if (colourLEDState != newState) { colourLEDState = newState; changed = true; }
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"" + setting + "\",\"value\":" + value + "}");
  } else if (setting == "brightness") {
    uint8_t newBrightness = value.toInt();
    if (colourLEDBrightness != newBrightness) { colourLEDBrightness = newBrightness; changed = true; }
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"" + setting + "\",\"value\":\"" + value + "\"}");
  } else {
    request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown setting\"}"); return;
  }
  if (changed) notifyObservers();
}

void processBuzzerControl(AsyncWebServerRequest *request, String setting, String value) {
  bool changed = false;
  if (setting == "volume") {
    uint8_t newVolume = value.toInt();
    if (newVolume > 255) newVolume = 255; // Clamp to valid range
    if (INITIAL_BUZZER_VOLUME != newVolume) {
      INITIAL_BUZZER_VOLUME = newVolume;
      player.setVolume(INITIAL_BUZZER_VOLUME);
      changed = true;
    }
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"" + setting + "\",\"value\":\"" + value + "\"}");
  } else if (setting == "test") {
    // Play a test tone to demonstrate the current volume
    if (value == "" || value == NULL) {
      notifyAudibleTone(12);
    }
    else {
      notifyAudibleTone(value.toInt());
    }
    request->send(200, "application/json", "{\"success\":true,\"action\":\"test\"}");
  } else {
    request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown setting\"}"); return;
  }
  if (changed) notifyObservers();
}

void processZoneControl(AsyncWebServerRequest *request, String zoneId, String action) {
  int zoneIndex = findZoneById(zoneId);
  bool changed = false;

  if (zoneIndex == -1) {
    if (request) request->send(404, "application/json", "{\"success\":false,\"error\":\"Zone not found\"}");
    return;
  }

  if (action == "toggle") {
    toggleZone(zoneIndex);
    changed = true;
    if (request) request->send(200, "application/json", "{\"success\":true,\"action\":\"toggle\",\"zone\":\"" + zoneId + "\"}");
  } else if (action == "on" || action == "1") {
    bool currentState = getZoneState(zoneIndex);
    if (!currentState) {
      toggleZone(zoneIndex);
      changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"action\":\"on\",\"zone\":\"" + zoneId + "\"}");
  } else if (action == "off" || action == "0") {
    bool currentState = getZoneState(zoneIndex);
    if (currentState) {
      toggleZone(zoneIndex);
      changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"action\":\"off\",\"zone\":\"" + zoneId + "\"}");
  } else {
    if (request) request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown action\"}");
    return;
  }

  if (changed) notifyObservers();
}

void processOutputPinControl(AsyncWebServerRequest *request, String pinStr, String valueStr) {
  int pin = pinStr.toInt();
  bool changed = false;
  if (valueStr == "press") {
    simulateButtonPressWithNegation(pin, 350);
    changed = true; // Assume change for notification
    if (request) request->send(200, "application/json", "{\"success\":true,\"action\":\"press\",\"pin\":" + pinStr + "}");
  } else {
    bool newState = (valueStr.toInt() > 0);
    int outputIndex = findOutputIndexByPin(pin);
    if (outputIndex != -1 && outputStates[outputIndex] != newState) {
      digitalWrite(pin, newState ? HIGH : LOW);
      outputStates[outputIndex] = newState;
      changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"pin\":" + pinStr + ",\"value\":" + valueStr + "}");
  }
  if (changed) notifyObservers();
}

void processACControl(AsyncWebServerRequest *request, String setting, String value) {

  bool changed = false;

  if (setting == "temp") {

    int temp = value.toInt();
    if (static_cast<int>(fujitsu.getTemp()) != temp) {
        fujitsu.setTemp(temp);
        changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"setting\":\"temp\",\"value\":" + value + "}");

  } else if (setting == "mode") {

    byte newModeByte = static_cast<byte>(ACMode::AUTO);// (ACMode::UNKNOWN);
    if (value == "fan") newModeByte = static_cast<byte>(ACMode::FAN);
    else if (value == "dry") newModeByte = static_cast<byte>(ACMode::DRY);
    else if (value == "cool") newModeByte = static_cast<byte>(ACMode::COOL);
    else if (value == "heat") newModeByte = static_cast<byte>(ACMode::HEAT);
    else if (value == "auto") newModeByte = static_cast<byte>(ACMode::AUTO);
    else {
        Serial.println("Unknown mode string received: " + value + ". Using default AUTO.");
    }
    if (static_cast<byte>(fujitsu.getMode()) != newModeByte) {
        fujitsu.setMode(newModeByte);
        changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"setting\":\"mode\",\"value\":\"" + value + "\"}");

  } else if (setting == "fan") {

    byte newFanMode = static_cast<byte>(ACFanMode::FAN_AUTO);

    if (value == "auto") newFanMode = static_cast<byte>(ACFanMode::FAN_AUTO);
    else if (value == "quiet") newFanMode = static_cast<byte>(ACFanMode::FAN_QUIET);
    else if (value == "low") newFanMode = static_cast<byte>(ACFanMode::FAN_LOW);
    else if (value == "medium") newFanMode = static_cast<byte>(ACFanMode::FAN_MEDIUM);
    else if (value == "high") newFanMode = static_cast<byte>(ACFanMode::FAN_HIGH);
    else {
        Serial.println("Unknown fan mode string received: " + value + ". Using default FAN_AUTO.");
    }
    if (static_cast<byte>(fujitsu.getFanMode()) != newFanMode) {
        fujitsu.setFanMode(newFanMode);
        changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"setting\":\"fan\",\"value\":\"" + value + "\"}");

  } else if (setting == "power") {

    bool newPower = (value == "on" || value == "1");
    if (static_cast<bool>(fujitsu.getOnOff()) != newPower) {
        fujitsu.setOnOff(newPower);
        changed = true;
    }
    if (request) request->send(200, "application/json", "{\"success\":true,\"setting\":\"power\",\"value\":\"" + value + "\"}");

  } else {

    if (request) request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown AC setting\"}"); return;

  }

  if (changed) notifyObservers();
}

void process404(AsyncWebServerRequest *request) {
  String message = "Path Not Found\n\nURI: " + request->url() + "\nMethod: " + request->methodToString() + "\nArguments: " + String(request->args()) + "\n";
  for (uint8_t i = 0; i < request->args(); i++) {
    message += " " + request->argName(i) + ": " + request->arg(i) + "\n";
  }
  request->send(404, "text/plain", message);
}

// --- WiFi Configuration and AP Mode Functions ---
bool connectToWiFi() {
  preferences.begin("wifi-creds", true); // Open read-only
  String storedSsid = preferences.getString(PREF_KEY_SSID, "");
  String storedPass = preferences.getString(PREF_KEY_PASS, "");
  preferences.end();

  if (storedSsid.length() == 0) {
    Serial.println("No stored WiFi credentials found.");
    return false;
  }

  Serial.print("Attempting to connect to WiFi: ");
  Serial.println(storedSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSsid.c_str(), storedPass.c_str());

  unsigned long startTime = millis();
  int countConnectionStatusPoll = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (countConnectionStatusPoll++ % 10 == 0) {
        Serial.print("\nConnection status: ");
        Serial.println(WiFi.status());
    }
    if (millis() - startTime > 20000) { // 20 second timeout
      Serial.println("\nFailed to connect to WiFi.");
      WiFi.disconnect(true); // Disconnect and clear any partial config
      return false;
    }
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  return true;
}

void startAPMode() {
  Serial.println("\nStarting AP Mode for WiFi Configuration...");
  WiFi.softAP(AP_CONFIG_SSID, AP_CONFIG_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(apIP);

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", htmlWiFiConfigCaptivePortal);
    Serial.println("Served WiFi config page.");
  });

  server.on("/savewificonfig", HTTP_POST, [](AsyncWebServerRequest *request){
    String newSsid = "";
    String newPass = "";
    if(request->hasParam("ssid", true)) newSsid = request->getParam("ssid", true)->value();
    if(request->hasParam("pass", true)) newPass = request->getParam("pass", true)->value();

    Serial.println("Received new WiFi credentials via AP Mode:");
    Serial.print("SSID: ");
    Serial.println(newSsid);

    if (newSsid.length() > 0) {
      preferences.begin("wifi-creds", false); // Open for writing
      preferences.putString(PREF_KEY_SSID, newSsid);
      preferences.putString(PREF_KEY_PASS, newPass);
      preferences.end();
      Serial.println("New WiFi credentials saved. Restarting...");

      String responseHtml = R"rawliteral(
        <!DOCTYPE HTML><html><head><title>ESP32 WiFi Config</title><meta name="viewport" content="width=device-width, initial-scale=1"></head><body>
        <div style='text-align:center;margin-top:50px;'><h2>Credentials Saved!</h2>
        <p>The ESP32 will now restart and attempt to connect to: <strong>%SSID%</strong></p>
        <p>If connection fails, the ESP32 Config AP will restart.</p></div></body></html>)rawliteral";
      responseHtml.replace("%SSID%", newSsid);
      request->send(200, "text/html", responseHtml);
      delay(3000);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "SSID cannot be empty.");
    }
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->host() != WiFi.softAPIP().toString() && request->host() != (String(AP_CONFIG_SSID) + ".local")) {
        Serial.println("Captive portal redirect for host: " + request->host());
        request->redirect("http://" + WiFi.softAPIP().toString());
    } else {
        request->send(404, "text/plain", "Not found (AP Config Mode)");
    }
  });

  server.begin(); // Start server with AP mode handlers
  Serial.println("HTTP server started for AP configuration. Connect to " + String(AP_CONFIG_SSID));
}
// --- End WiFi Configuration and AP Mode Functions ---

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String payloadBuffer;
    for (unsigned int i = 0; i < length; i++) {
        payloadBuffer += (char)payload[i];
    }
    String topicStr(topic);
    Serial.println("Processing event on topic '" + topicStr + "' with payload: " + payloadBuffer);

    // For debug puposes share processing mqtt message with ws observers
    JsonDocument docWS;
    docWS["type"] = "mqtt_log";
    docWS["message"] = String("Processing event on topic '") + String(topicStr) + String("' with payload: ") + String(payloadBuffer);
    String json;
    serializeJson(docWS, json);
    ws.textAll(json);
    // End debug mqtt

    if (topicStr == String(mqttBaseTopic) + String("/status")) return; // Ignore our own updates to the world

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payloadBuffer);
    if (error) {
      Serial.print(F("Deserialisation of payload failed: "));
      Serial.println(error.c_str());
      return;
    }

    if (topicStr == String(mqttBaseTopic) + String("/ac/set")) {
        String setting = doc["setting"];
        String value = doc["value"];
        processACControl(nullptr, setting, value);
    }

    if (topicStr == String(mqttBaseTopic) + String("/pin/set")) {
        String pin = doc["pin"];
        String value = doc["value"];
        processOutputPinControl(nullptr, pin, value);
    }

    if (topicStr == String(mqttBaseTopic) + String("/zone/set")) {
        String zoneId = doc["id"];
        String action = doc["action"];
        processZoneControl(nullptr, zoneId, action);
    }
}

void reconnectMQTT() {
    // Only attempt to reconnect if not connected and enough time has passed since last attempt
    unsigned long currentMillis = millis();
    if (!mqttClient.connected() && (currentMillis - lastMqttConnectionAttempt >= mqttReconnectInterval)) {
        lastMqttConnectionAttempt = currentMillis; // Update the last attempt time

        Serial.print("Attempting MQTT connection...");
        String clientId = "ACController" + WiFi.localIP().toString();
        String topic = String(mqttBaseTopic) + String("/#");
        if (mqttClient.connect(clientId.c_str(), mqttUser, mqttPassword)) {
            Serial.print("connected.. ");
            mqttClient.subscribe(topic.c_str());
            Serial.println("and subscribed to " + topic);
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" will try again in " + String(mqttReconnectInterval) + " milliseconds");
        }
    }
}

void setup() {

  Serial.begin(115200);
  Serial.println("\n\nBooting Baum's Fujitsu AC & Zone Controller...");

  player.setVolume(INITIAL_BUZZER_VOLUME);

  if (connectToWiFi()) {
    // --- STA Mode: WiFi Connected - Initialize full functionality ---
    Serial.println("STA Mode: Initializing full device functionality...");

    // Load pin configurations from preferences
    preferences.begin("pin-config", true);
    acRxPin = preferences.getUChar(PREF_KEY_AC_RX_PIN, acRxPin);
    acTxPin = preferences.getUChar(PREF_KEY_AC_TX_PIN, acTxPin);

    // Load output pins
    String outputPinsStr = preferences.getString(PREF_KEY_OUTPUT_PINS, "");
    if (outputPinsStr.length() > 0) {
      outputPinCount = 0;
      int startPos = 0;
      int commaPos = outputPinsStr.indexOf(',');
      while (commaPos >= 0 && outputPinCount < MAX_OUTPUT_PINS) {
        outputPins[outputPinCount++] = outputPinsStr.substring(startPos, commaPos).toInt();
        startPos = commaPos + 1;
        commaPos = outputPinsStr.indexOf(',', startPos);
      }
      // Add the last pin after the last comma (or the only pin if no commas)
      if (startPos < outputPinsStr.length() && outputPinCount < MAX_OUTPUT_PINS) {
        outputPins[outputPinCount++] = outputPinsStr.substring(startPos).toInt();
      }
    }

    // Load input pins
    String inputPinsStr = preferences.getString(PREF_KEY_INPUT_PINS, "");
    if (inputPinsStr.length() > 0) {
      inputPinCount = 0;
      int startPos = 0;
      int commaPos = inputPinsStr.indexOf(',');
      while (commaPos >= 0 && inputPinCount < MAX_INPUT_PINS) {
        inputPins[inputPinCount++] = inputPinsStr.substring(startPos, commaPos).toInt();
        startPos = commaPos + 1;
        commaPos = inputPinsStr.indexOf(',', startPos);
      }
      // Add the last pin after the last comma (or the only pin if no commas)
      if (startPos < inputPinsStr.length() && inputPinCount < MAX_INPUT_PINS) {
        inputPins[inputPinCount++] = inputPinsStr.substring(startPos).toInt();
      }
    }
    preferences.end();

    // Load zone configurations
    preferences.begin("zone-config", true);
    String zonesStr = preferences.getString(PREF_KEY_ZONES, "");
    if (zonesStr.length() > 0) {
      JsonDocument zonesDoc;
      DeserializationError error = deserializeJson(zonesDoc, zonesStr);
      if (!error) {
        JsonArray zonesArray = zonesDoc.as<JsonArray>();
        zoneCount = min((size_t)MAX_ZONES, zonesArray.size());
        for (int i = 0; i < zoneCount; i++) {
          zones[i].id = zonesArray[i]["id"].as<String>();
          zones[i].inputPin = zonesArray[i]["inputPin"];
          zones[i].outputPin = zonesArray[i]["outputPin"];
        }
        Serial.printf("Loaded %d zones from preferences\n", zoneCount);
      } else {
        Serial.println("Error parsing zones JSON from preferences");
      }
    }
    preferences.end();

    Serial.println("Loaded pin configuration:");
    Serial.printf("AC RX Pin: %d, AC TX Pin: %d\n", acRxPin, acTxPin);
    Serial.print("Output Pins: ");
    for (int i = 0; i < outputPinCount; i++) {
      Serial.printf("%d ", outputPins[i]);
    }
    Serial.println();
    Serial.print("Input Pins: ");
    for (int i = 0; i < inputPinCount; i++) {
      Serial.printf("%d ", inputPins[i]);
    }
    Serial.println();

    fujitsu.connect(&Serial2, true, acRxPin, acTxPin);

    preferences.begin("mqtt-config", true);
    preferences.getString(PREF_KEY_MQTT_BROKER, mqttBroker, sizeof(mqttBroker));
    mqttPort = preferences.getInt(PREF_KEY_MQTT_PORT, 1883);
    preferences.getString(PREF_KEY_MQTT_USER, mqttUser, sizeof(mqttUser));
    preferences.getString(PREF_KEY_MQTT_PASS, mqttPassword, sizeof(mqttPassword));
    preferences.getString(PREF_KEY_MQTT_TOPIC, mqttBaseTopic, sizeof(mqttBaseTopic));
    preferences.end();

    if (strlen(mqttBroker) > 0) {
        mqttClient.setServer(mqttBroker, mqttPort);
        mqttClient.setCallback(mqttCallback);
        mqttClient.setBufferSize(MQTT_MAX_PACKET_SIZE);
    }

    FastLED.addLeds<WS2812, LEDS_PIN, GRB>(leds, LEDS_COUNT);
    FastLED.setBrightness(colourLEDBrightness);

    for (int i = 0; i < ARRAY_SIZE(outputPins); i++) {
      initOutputPin(outputPins[i]);
      outputStates[i] = false; // Ensure initial state is known
    }
    for (int i = 0; i < ARRAY_SIZE(inputPins); i++) {
      initInputPin(inputPins[i]);
      inputStates[i] = digitalRead(inputPins[i]) == HIGH;
    }

    // Initial status led blink sequence
    for (int i = 0; i < 33; i++) {
      delay(75);
      String log = (i % 2 == 0) ? "!" : "¡";
      uint8_t level = (i % 2 == 0) ? HIGH : LOW;
      Serial.print(log);
      digitalWrite(STATUS_LED_PIN, level);
    }
    // Ensure status led is LOW after blink
    digitalWrite(STATUS_LED_PIN, LOW);

    notifyAudibleTone(13);

    // Initialize SPIFFS and static web server
    if (!staticWebServer.begin()) {
      Serial.println("Failed to initialize static web server, falling back to dynamic HTML");
    } else {
      // List all files in SPIFFS for debugging
      staticWebServer.listFiles();
    }

    // Set up existing server routes for STA mode
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ processRootRoute(request); });
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){ processApiStatusRoute(request); });
    server.on("/api/mqtt/save", HTTP_POST, [](AsyncWebServerRequest *request){ processSaveMqttConfigRoute(request); });
    server.on("^\\/api\\/colourled\\/(state|brightness)\\/([0-9a-zA-Z]+)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processColourLEDControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("^\\/api\\/buzzer\\/(volume|test)\\/([0-9]+)?$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processBuzzerControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("^\\/api\\/out\\/([0-9]+)\\/(0|1|press)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processOutputPinControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("^\\/api\\/ac\\/(temp|mode|fan|power)\\/([0-9]+|dry|cool|heat|auto|quiet|low|medium|high|on|off|0|1)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processACControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("/api/pins/save", HTTP_POST,
      [](AsyncWebServerRequest *request) { request->send(400, "application/json", "{\"success\":false,\"error\":\"No data provided\"}"); },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        processSavePinConfigRoute(request, data, len, index, total);
      });
    server.on("/api/zones/save", HTTP_POST,
      [](AsyncWebServerRequest *request) { request->send(400, "application/json", "{\"success\":false,\"error\":\"No data provided\"}"); },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        processSaveZoneConfigRoute(request, data, len, index, total);
      });
    server.on("^\\/api\\/zone\\/([^/]+)\\/(toggle|on|off|0|1)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processZoneControl(request, request->pathArg(0), request->pathArg(1)); });
    server.onNotFound([](AsyncWebServerRequest *request){ process404(request); });

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    OTA.begin(&server);
    server.begin();

    Serial.print("HTTP server and WebSocket started in STA mode at http://");
    Serial.print(WiFi.localIP().toString().c_str());
    Serial.print(":");
    Serial.println(apiPort);

  } else {
    // --- AP Mode: WiFi Connection Failed or No Credentials ---
    startAPMode();
  }
}

void processFujitsuComms() {
  bool frameReceived = fujitsu.waitForFrame();

  if(frameReceived) {
    fujitsuLastConnected = millis();

    // Check if AC settings have changed
    ControlFrame *currentState = fujitsu.getCurrentState();

    if (!acStateInitialized) {
      acStateInitialized = true;
    } else {
      // Compare current state with previous state
      bool settingsChanged = false;

      if (previousACState.onOff != currentState->onOff ||
          previousACState.temperature != currentState->temperature ||
          previousACState.acMode != currentState->acMode ||
          previousACState.fanMode != currentState->fanMode ||
          previousACState.economyMode != currentState->economyMode ||
          previousACState.swingMode != currentState->swingMode ||
          previousACState.swingStep != currentState->swingStep) {

        settingsChanged = true;
      }

      // If settings changed, notify observers
      if (settingsChanged) {
        Serial.println("AC settings changed, notifying observers");
        notifyObservers();
      }
    }

    // Update previous state for next comparison
    memcpy(&previousACState, currentState, sizeof(ControlFrame));

    delay(60);
    fujitsu.sendPendingFrame();
  }

  if (millis() - fujitsuLastConnected > fujitsuConnectionTimeout) {
    Serial.println("Fujitsu AC connection timed out, resetting connection...");
    fujitsu.resetConnection();
    fujitsuLastConnected = millis();
    acStateInitialized = false; // Reset state initialization flag
  }
}

void processLEDColourCycle() {
  if (!colourLEDState) {
    FastLED.setBrightness(0);
    FastLED.show();
  } else {
    FastLED.setBrightness(colourLEDBrightness);
    if (millis() - colourCycleLastProcessedMillis >= colourCycleProcessFrequency) {
      colourCycleLastProcessedMillis += colourCycleProcessFrequency;
      leds[0] = CHSV(colourCycleOffset, 255, 255);
      FastLED.show();
      colourCycleOffset = (colourCycleOffset + 7) & 255;
    }
  }
}

void processResetButtonPress() {
  if (millis() - resetButtonLastPressMillis >= resetButtonSamplingFrequency) {
    resetButtonLastPressMillis += resetButtonSamplingFrequency;
    if (digitalRead(resetButtonPin) == LOW) { // Assuming LOW means pressed for BOOT button
      Serial.println("Reset button pressed, restarting ESP...");
      ESP.restart();
    }
  }
}

void processPinStateChanges() {
  if (millis() - pinStateCheckLastMillis >= pinStateCheckInterval) {
    pinStateCheckLastMillis = millis();
    bool changed = false;
    for (int i = 0; i < inputPinCount; i++) {
      bool currentState = digitalRead(inputPins[i]) == HIGH;
      if (currentState != inputStates[i]) {
        inputStates[i] = currentState;
        changed = true;
      }
    }

    if (changed) {
      notifyObservers();
    }
  }
}

void loop() {
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) { // If in any AP mode
    dnsServer.processNextRequest(); // Handle DNS for captive portal
  }

  // These should only run if WiFi is connected and system is in full operational STA mode
  if (WiFi.status() == WL_CONNECTED) {
    if (strlen(mqttBroker) > 0) {
        reconnectMQTT();
        if (mqttClient.connected()) {
            mqttClient.loop();
        }
    }
    OTA.loop();
    processFujitsuComms();
    processLEDColourCycle();
    processPinStateChanges();
    ws.cleanupClients();
  }
  processResetButtonPress(); // Reset button should always be active

  delay(10); // Small delay to yield
}
