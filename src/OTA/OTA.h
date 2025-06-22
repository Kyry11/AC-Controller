#include "Arduino.h"
#include "stdlib_noniso.h"
#include "WebAppStatic.h"

#define OTA_DEBUG_MSG(x) Serial.printf("%s %s", "[OTA] ", x)

#include <functional>
#include "FS.h"
#include "Update.h"
#include "StreamString.h"
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"
#define OTA_WEBSERVER AsyncWebServer

enum OTA_Mode {
    OTA_MODE_FIRMWARE = 0,
    OTA_MODE_FILESYSTEM = 1
};

class OTAClass{
  public:
    OTAClass();

    void begin(OTA_WEBSERVER *server, const char * username = "", const char * password = "");

    void setAuth(const char * username, const char * password);
    void clearAuth();
    void setAutoReboot(bool enable);
    void loop();

    void onStart(std::function<void()> callable);
    void onProgress(std::function<void(size_t current, size_t final)> callable);
    void onEnd(std::function<void(bool success)> callable);

  private:
    OTA_WEBSERVER *_server;

    bool _authenticate;
    String _username;
    String _password;

    bool _auto_reboot = true;
    bool _reboot = false;
    unsigned long _reboot_request_millis = 0;

    String _update_error_str = "";
    unsigned long _current_progress_size;

    std::function<void()> preUpdateCallback = NULL;
    std::function<void(size_t current, size_t final)> progressUpdateCallback = NULL;
    std::function<void(bool success)> postUpdateCallback = NULL;
};

extern OTAClass OTA;