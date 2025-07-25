#include <Arduino.h>
#define ARRAY_SIZE(arr)   (sizeof(arr) / sizeof((arr)[0]))

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "OTA/OTA.h"
#include <ArduinoJson.h>
#include "AC/FujitsuAC.h"
#include <FastLED.h>
#include <Preferences.h>
#include <DNSServer.h>

#include "melody_player/melody_player.h"
#include "melody_player/melody_factory.h"

// Preferences keys for eeprom config
const char* PREF_KEY_SSID = "wifi_ssid";
const char* PREF_KEY_PASS = "wifi_pass";

// AP Mode settings for WiFi configuration
const char* AP_CONFIG_SSID = "Baums AC Controller"; // Unique name for config AP
const char* AP_CONFIG_PASSWORD = NULL;              // No password for config AP
const byte DNS_PORT = 53;

Preferences preferences;
DNSServer dnsServer;

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

bool colourLEDState = true;
uint8_t colourLEDBrightness = 30;

const uint8_t acRxPin = 25;
const uint8_t acTxPin = 32;

const uint8_t outputPins[] = { 2, 19, 21, 22, 23 };
bool outputStates[ARRAY_SIZE(outputPins)] = {};

const uint8_t inputPins[] = { 14, 26, 27, 33 };
bool inputStates[ARRAY_SIZE(inputPins)] = {};

const uint8_t STATUS_LED_PIN = outputPins[0];

const uint8_t resetButtonPin = 0;
const int resetButtonSamplingFrequency = 300;
unsigned long resetButtonLastPressMillis = millis();

const int colourCycleProcessFrequency = 300;
unsigned long colourCycleLastProcessedMillis = millis();
int colourCycleOffset = 0;

const int pinStateCheckInterval = 250;
unsigned long pinStateCheckLastMillis = millis();

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

void simulateButtonPressWithNegation(uint8_t pin, int delayMs) {
  int idx = findOutputIndexByPin(pin);
  if (idx == -1) return;
  bool level = outputStates[idx];
  digitalWrite(pin, level ? LOW : HIGH);
  delay(delayMs);
  digitalWrite(pin, level ? HIGH : LOW);
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

String buildOutputPinsHTML() {
  String buttons = "";
  for (int i = 0; i < ARRAY_SIZE(outputPins); i++) {
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
  for (int i = 0; i < ARRAY_SIZE(inputPins); i++) {
    bool state = digitalRead(inputPins[i]) == HIGH;
    String buttonClass = state ? "button" : "button button-off";
    buttons += "<div><a id=\"inPin" + String(inputPins[i]) + "\" href=\"javascript:void(0)\" class=\"" + buttonClass + "\">Pin " +
      String(inputPins[i]) + ": " + (state ? "ON" : "OFF") + "</a></div>";
  }
  return buttons;
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
  html += "      const acStateLabel = document.getElementById('acstatelabel')";
  html += "      if (acStateLabel) acStateLabel.innerHTML = 'Power: ' + (state.ac.power ? 'On' : 'Off');";
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
  html += "}";
  html += "document.addEventListener('DOMContentLoaded', function() {";
  html += "  reloadCurrentStateAsync().then(() => connectWebSocket());";
  html += "});";
  html += "</script></head>";
  html += "<body><h1>Fujitsu AC & Zone Controller</h1><h3>v1.1.1</h3>";
  html += "<h3>Update Current State</h3><div class=\"button-container\"><input type=\"button\" value=\"Refresh\" onclick=\"reloadCurrentStateAsync();return false;\"></div>";
  html += "<h3>Fujitsu AC Controller Status</h3><div>" + buildACControlHTML() + "</div>";
  html += "<h3>Colour Cycling LED Control</h3><div class=\"button-container\">" + buildColourLEDControlHTML() + "</div>";
  html += "<h3>Buzzer Control</h3><div class=\"button-container\">" + buildBuzzerControlHTML() + "</div>";
  html += "<h3>Available GPIO Output Control Pins</h3><div class=\"button-container\">" + buildOutputPinsHTML() + "</div>";
  html += "<h3>Available Input Control Pins</h3><div class=\"button-container\">" + buildInputPinsHTML() + "</div>";
  html += "<h3><a href=\"/update\">Upload new firmware</a></h3>";
  html += "<h3>Device Logs</h3><div id=\"log-container\" style=\"height: 200px; overflow-y: scroll; border: 1px solid #ccc; padding: 10px; text-align: left; background-color: #f9f9f9;\"></div>";
  html += "</body></html>";
  return html;
}

void processRootRoute(AsyncWebServerRequest *request) {
  request->send(200, "text/html", buildHtmlPage());
}

String buildCurrentStatePayload() {
  JsonDocument doc;
  doc["ac"]["power"] = fujitsu.getOnOff();
  doc["ac"]["mode"] = ACModeToString(static_cast<ACMode>(fujitsu.getMode()));
  doc["ac"]["fanMode"] = ACFanModeToString(static_cast<ACFanMode>(fujitsu.getFanMode()));
  doc["ac"]["temp"] = fujitsu.getTemp();
  JsonArray outputs = doc["outputs"].to<JsonArray>();
  for (int i = 0; i < ARRAY_SIZE(outputPins); i++) {
    JsonObject output = outputs.add<JsonObject>();
    output["pin"] = String(outputPins[i]);
    output["state"] = outputStates[i];
  }
  JsonArray inputs = doc["inputs"].to<JsonArray>();
  for (int i = 0; i < ARRAY_SIZE(inputPins); i++) {
    JsonObject input = inputs.add<JsonObject>();
    input["pin"] = String(inputPins[i]);
    input["state"] = inputStates[i];
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
  request->send(200, "application/json", buildCurrentStatePayload());
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

void notifyWSSubscribers() {
  ws.textAll(buildCurrentStatePayload());
}

void notifyObservers() {
  notifyWSSubscribers();
  notifyAudibleTone(4);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      // notifyObservers(); // Not sure if there is value in notifying all clients when a new client connects
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

void processOutputPinControl(AsyncWebServerRequest *request, String pinStr, String valueStr) {
  int pin = pinStr.toInt();
  bool changed = false;
  if (valueStr == "press") {
    simulateButtonPressWithNegation(pin, 350);
    // After press, the state might have toggled. We need to reflect the *new* actual state.
    // For simplicity, we assume it might have changed and notify. A more robust way is to read state after press.
    int outputIndex = findOutputIndexByPin(pin); // Re-check state after press
    if (outputIndex != -1) {
        // This is tricky: simulateButtonPressWithNegation toggles based on outputStates[outputIndex]
        // So outputStates[outputIndex] should be updated by simulateButtonPressWithNegation or here.
        // Let's assume simulateButtonPressWithNegation doesn't update outputStates, so we toggle it here.
        // outputStates[outputIndex] = !outputStates[outputIndex]; // This might be incorrect if press doesn't always toggle
    }
    changed = true; // Assume change for notification
    request->send(200, "application/json", "{\"success\":true,\"action\":\"press\",\"pin\":" + pinStr + "}");
  } else {
    bool newState = (valueStr.toInt() > 0);
    int outputIndex = findOutputIndexByPin(pin);
    if (outputIndex != -1 && outputStates[outputIndex] != newState) {
      digitalWrite(pin, newState ? HIGH : LOW);
      outputStates[outputIndex] = newState;
      changed = true;
    }
    request->send(200, "application/json", "{\"success\":true,\"pin\":" + pinStr + ",\"value\":" + valueStr + "}");
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
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"temp\",\"value\":" + value + "}");

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
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"mode\",\"value\":\"" + value + "\"}");

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
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"fan\",\"value\":\"" + value + "\"}");

  } else if (setting == "power") {

    bool newPower = (value == "on" || value == "1");
    if (static_cast<bool>(fujitsu.getOnOff()) != newPower) {
        fujitsu.setOnOff(newPower);
        changed = true;
    }
    request->send(200, "application/json", "{\"success\":true,\"setting\":\"power\",\"value\":\"" + value + "\"}");

  } else {

    request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown AC setting\"}"); return;

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

void setup() {

  Serial.begin(115200);
  Serial.println("\n\nBooting Baum's Fujitsu AC & Zone Controller...");

  player.setVolume(INITIAL_BUZZER_VOLUME);

  if (connectToWiFi()) {
    // --- STA Mode: WiFi Connected - Initialize full functionality ---
    Serial.println("STA Mode: Initializing full device functionality...");

    FastLED.addLeds<WS2812, LEDS_PIN, GRB>(leds, LEDS_COUNT);
    FastLED.setBrightness(colourLEDBrightness);

    fujitsu.connect(&Serial2, true, acRxPin, acTxPin);

    for (int i = 0; i < ARRAY_SIZE(outputPins); i++) {
      initOutputPin(outputPins[i]);
      outputStates[i] = false; // Ensure initial state is known
    }
    for (int i = 0; i < ARRAY_SIZE(inputPins); i++) {
      initInputPin(inputPins[i]);
      inputStates[i] = digitalRead(inputPins[i]) == HIGH;
    }

    // // Initial output pin blink sequence (optional, can be removed if not desired after WiFi setup)
    // for (int i = 0; i < 20; i++) {
    //   delay(75);
    //   String log = (i % 2 == 0) ? "!" : "¡";
    //   uint8_t level = (i % 2 == 0) ? HIGH : LOW;
    //   for (int j = 0; j < ARRAY_SIZE(outputPins); j++) {
    //     Serial.print(log);
    //     digitalWrite(outputPins[j], level);
    //   }
    // }
    // // Ensure pins are LOW after blink
    // for (int i = 0; i < ARRAY_SIZE(outputPins); i++) {
    //   digitalWrite(outputPins[i], LOW);
    // }

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

    // Set up existing server routes for STA mode
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ processRootRoute(request); });
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){ processApiStatusRoute(request); });
    server.on("^\\/api\\/colourled\\/(state|brightness)\\/([0-9a-zA-Z]+)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processColourLEDControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("^\\/api\\/buzzer\\/(volume|test)\\/([0-9]+)?$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processBuzzerControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("^\\/api\\/out\\/([0-9]+)\\/(0|1|press)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processOutputPinControl(request, request->pathArg(0), request->pathArg(1)); });
    server.on("^\\/api\\/ac\\/(temp|mode|fan|power)\\/([0-9]+|dry|cool|heat|auto|quiet|low|medium|high|on|off|0|1)$", HTTP_POST,
      [](AsyncWebServerRequest *request) { processACControl(request, request->pathArg(0), request->pathArg(1)); });
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
  if(fujitsu.waitForFrame()) {
    delay(60);
    fujitsu.sendPendingFrame();
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
    for (int i = 0; i < ARRAY_SIZE(inputPins); i++) {
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
    OTA.loop();
    processFujitsuComms();
    processLEDColourCycle();
    processPinStateChanges();
    ws.cleanupClients();
  }
  processResetButtonPress(); // Reset button should always be active

  delay(10); // Small delay to yield
}
