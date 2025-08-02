#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

class StaticWebServer {
private:
    AsyncWebServer* _server;
    bool _initialized;

    String getContentType(String filename) {
        if (filename.endsWith(".html")) return "text/html";
        else if (filename.endsWith(".css")) return "text/css";
        else if (filename.endsWith(".js")) return "application/javascript";
        else if (filename.endsWith(".json")) return "application/json";
        else if (filename.endsWith(".png")) return "image/png";
        else if (filename.endsWith(".jpg")) return "image/jpeg";
        else if (filename.endsWith(".ico")) return "image/x-icon";
        else if (filename.endsWith(".svg")) return "image/svg+xml";
        return "text/plain";
    }

public:
    StaticWebServer(AsyncWebServer* server) : _server(server), _initialized(false) {}

    bool begin() {

        if (_initialized) return true;

        if (!SPIFFS.begin(true)) {
            Serial.println("An error occurred while mounting SPIFFS");
            return false;
        }

        _server->serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

        _server->onNotFound([](AsyncWebServerRequest *request) {
            request->send(404, "text/plain", "File Not Found");
        });

        _initialized = true;
        return true;
    }

    // Method to list all files in SPIFFS (useful for debugging)
    void listFiles() {
        Serial.println("SPIFFS files:");
        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        while (file) {
            String fileName = file.name();
            size_t fileSize = file.size();
            Serial.printf("  %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
            file = root.openNextFile();
        }
    }

    String formatBytes(size_t bytes) {
        if (bytes < 1024) return String(bytes) + " B";
        else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " KB";
        else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
        else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
    }
};