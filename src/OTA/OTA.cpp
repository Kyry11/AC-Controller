#include "OTA.h"

OTAClass::OTAClass(){}

void OTAClass::begin(OTA_WEBSERVER *server, const char * username, const char * password){
  _server = server;

  setAuth(username, password);

  _server->on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
    if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
      return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", WEBAPP_STATIC, sizeof(WEBAPP_STATIC));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });


  _server->on("/ota/start", HTTP_GET, [&](AsyncWebServerRequest *request) {
    if (_authenticate && !request->authenticate(_username.c_str(), _password.c_str())) {
      return request->requestAuthentication();
    }

    // Get header x-ota-mode value, if present
    OTA_Mode mode = OTA_MODE_FIRMWARE;
    // Get mode from arg
    if (request->hasParam("mode")) {
      String argValue = request->getParam("mode")->value();
      if (argValue == "fs") {
        OTA_DEBUG_MSG("OTA Mode: Filesystem\n");
        mode = OTA_MODE_FILESYSTEM;
      } else {
        OTA_DEBUG_MSG("OTA Mode: Firmware\n");
        mode = OTA_MODE_FIRMWARE;
      }
    }

    // Get file MD5 hash from arg
    if (request->hasParam("hash")) {
      String hash = request->getParam("hash")->value();
      OTA_DEBUG_MSG(String("MD5: "+hash+"\n").c_str());
      if (!Update.setMD5(hash.c_str())) {
        OTA_DEBUG_MSG("ERROR: MD5 hash not valid\n");
        return request->send(400, "text/plain", "MD5 parameter invalid");
      }
    }

    // Serial output must be active to see the callback serial prints
    // Serial.setDebugOutput(true);

    // Pre-OTA update callback
    if (preUpdateCallback != NULL) preUpdateCallback();

    // Start update process
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
      OTA_DEBUG_MSG("Failed to start update process\n");
      // Save error to string
      StreamString str;
      Update.printError(str);
      _update_error_str = str.c_str();
      _update_error_str.concat("\n");
      OTA_DEBUG_MSG(_update_error_str.c_str());
    }

    return request->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
  });


  _server->on("/ota/upload", HTTP_POST, [&](AsyncWebServerRequest *request) {
      if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
        return request->requestAuthentication();
      }
      // Post-OTA update callback
      if (postUpdateCallback != NULL) postUpdateCallback(!Update.hasError());
      AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
      response->addHeader("Connection", "close");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      // Set reboot flag
      if (!Update.hasError()) {
        if (_auto_reboot) {
          _reboot_request_millis = millis();
          _reboot = true;
        }
      }
  }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      //Upload handler chunks in data
      if(_authenticate){
          if(!request->authenticate(_username.c_str(), _password.c_str())){
              return request->requestAuthentication();
          }
      }

      if (!index) {
        // Reset progress size on first frame
        _current_progress_size = 0;
      }

      // Write chunked data to the free sketch space
      if(len){
          if (Update.write(data, len) != len) {
              return request->send(400, "text/plain", "Failed to write chunked data to free space");
          }
          _current_progress_size += len;
          // Progress update callback
          if (progressUpdateCallback != NULL) progressUpdateCallback(_current_progress_size, request->contentLength());
      }

      if (final) { // if the final flag is set then this is the last frame of data
          if (!Update.end(true)) { //true to set the size to the current progress
              // Save error to string
              StreamString str;
              Update.printError(str);
              _update_error_str = str.c_str();
              _update_error_str.concat("\n");
              OTA_DEBUG_MSG(_update_error_str.c_str());
          }
      }else{
          return;
      }
  });

}

void OTAClass::setAuth(const char * username, const char * password){
  _username = username;
  _password = password;
  _authenticate = _username.length() && _password.length();
}

void OTAClass::clearAuth(){
  _authenticate = false;
}

void OTAClass::setAutoReboot(bool enable){
  _auto_reboot = enable;
}

void OTAClass::loop() {
  // Check if 2 seconds have passed
  if (_reboot && millis() - _reboot_request_millis > 2000) {
    OTA_DEBUG_MSG("Rebooting...\n");
    ESP.restart();
    _reboot = false;
  }
}

void OTAClass::onStart(std::function<void()> callable){
    preUpdateCallback = callable;
}

void OTAClass::onProgress(std::function<void(size_t current, size_t final)> callable){
    progressUpdateCallback= callable;
}

void OTAClass::onEnd(std::function<void(bool success)> callable){
    postUpdateCallback = callable;
}

OTAClass OTA;