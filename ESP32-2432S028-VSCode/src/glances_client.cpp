#include "glances_client.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

GlancesClient::GlancesClient(const char* ip, uint16_t port) {
    _ip = ip;
    _port = port;
    _stats = {0, 0, 0, 0, false};
}

ServerStats GlancesClient::getStats() {
    return _stats;
}

bool GlancesClient::update() {
    bool success = true;
    success &= fetchQuicklook();
    success &= fetchFs();
    success &= fetchSensors();
    
    _stats.is_online = success;
    return success;
}

bool GlancesClient::fetchQuicklook() {
    HTTPClient http;
    String url = String("http://") + _ip + ":" + _port + "/api/3/quicklook";
    http.begin(url);
    http.setTimeout(3000); // 3 seconds timeout
    
    int httpCode = http.GET();
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            
            if (!error) {
                _stats.cpu_percent = doc["cpu"] | 0.0f;
                _stats.mem_percent = doc["mem"] | 0.0f;
                http.end();
                return true;
            }
        }
    }
    http.end();
    return false;
}

bool GlancesClient::fetchFs() {
    HTTPClient http;
    String url = String("http://") + _ip + ":" + _port + "/api/3/fs";
    http.begin(url);
    http.setTimeout(3000);
    
    int httpCode = http.GET();
    if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error && doc.is<JsonArray>()) {
            for (JsonObject fs : doc.as<JsonArray>()) {
                const char* mnt_point = fs["mnt_point"];
                if (mnt_point && strcmp(mnt_point, "/") == 0) {
                    _stats.disk_percent = fs["percent"] | 0.0f;
                    http.end();
                    return true;
                }
            }
        }
    }
    http.end();
    return false;
}

bool GlancesClient::fetchSensors() {
    HTTPClient http;
    String url = String("http://") + _ip + ":" + _port + "/api/3/sensors";
    http.begin(url);
    http.setTimeout(3000);
    
    int httpCode = http.GET();
    if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error && doc.is<JsonArray>()) {
            for (JsonObject sensor : doc.as<JsonArray>()) {
                const char* type = sensor["type"];
                if (type && strncmp(type, "temperature", 11) == 0) {
                    _stats.temp_c = sensor["value"] | 0.0f;
                    http.end();
                    return true;
                }
            }
        }
    }
    http.end();
    return false;
}
