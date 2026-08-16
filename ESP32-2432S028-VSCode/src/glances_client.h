#ifndef GLANCES_CLIENT_H
#define GLANCES_CLIENT_H

#include <Arduino.h>

struct ServerStats {
    float cpu_percent;
    float mem_percent;
    float disk_percent;
    float temp_c;
    bool is_online;
};

class GlancesClient {
public:
    GlancesClient(const char* ip, uint16_t port);
    
    // Fetch all stats from the server
    bool update();
    
    // Get the latest stats
    ServerStats getStats();

private:
    const char* _ip;
    uint16_t _port;
    ServerStats _stats;
    
    bool fetchQuicklook();
    bool fetchFs();
    bool fetchSensors();
};

#endif // GLANCES_CLIENT_H
