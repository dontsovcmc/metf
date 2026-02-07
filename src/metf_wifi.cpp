#ifdef ESP32

#include "metf_wifi.h"
#include "logging.h"
#include "AsyncSerialBuffer.h"

#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)

// WiFi connection function
bool connectToWiFi(const String& ssid, const String& password) {
    LOG_INFO("Connecting to WiFi: " << ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        LOG_DEBUG(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        LOG_INFO("Connected! IP: " << WiFi.localIP());

        #ifdef RGB_DEFAULT_PIN
        // Green = connected
        for (uint8_t i = 0; i < RGB_NUMBER; i++) {
            rgb_leds[i] = CRGB(0, 255, 0);
        }
        LOCK();
        FastLED.show();
        UNLOCK();
        #endif

        return true;
    }

    LOG_ERROR("Connection failed!");
    return false;
}

// Start access point mode
void startAPMode(const String& ap_name) {
    isAPMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_name.c_str());
    LOG_INFO("AP Mode started: " << ap_name);
    LOG_INFO("AP IP: " << WiFi.softAPIP());

    // Start DNS server for captive portal
    dnsServer.start(53, "*", WiFi.softAPIP());

    #ifdef RGB_DEFAULT_PIN
    // Red = AP mode
    for (uint8_t i = 0; i < RGB_NUMBER; i++) {
        rgb_leds[i] = CRGB(255, 0, 0);
    }
    LOCK();
    FastLED.show();
    UNLOCK();
    #endif
}

// Setup WiFi configuration
void setupWiFi() {
    LOG_INFO("Starting WiFi configuration...");

    // Open Preferences for stored credentials
    preferences.begin(PREFS_NAMESPACE, false);

    // Try to load saved credentials
    String saved_ssid = preferences.getString(WIFI_SSID_KEY, "");
    String saved_pass = preferences.getString(WIFI_PASS_KEY, "");

    bool connected = false;
    if (saved_ssid.length() > 0) {
        LOG_INFO("Found saved WiFi credentials");
        connected = connectToWiFi(saved_ssid, saved_pass);
    }

    // Set AP name
    ap_name = String("METF.") + String(METF_VERSION) + String(".") + String(FIRMWARE_VERSION);

    // If no credentials or connection failed, start AP mode
    if (!connected) {
        LOG_INFO("No saved credentials or connection failed");
        startAPMode(ap_name);
    } else {
        LOG_INFO("WiFi connected successfully!");
        LOG_INFO("IP Address: " << WiFi.localIP());
    }
}

// Setup WiFi configuration endpoints
void setupWiFiEndpoints() {
    // GET /wifi-scan - scan available networks
    server.on("/wifi-scan", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "[";
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN) + "}";
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    // POST /wifi-config - save new credentials and restart
    server.on("/wifi-config", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
            request->send(400, "text/plain", "Missing ssid or password");
            return;
        }

        String ssid = request->getParam("ssid", true)->value();
        String password = request->getParam("password", true)->value();

        // Save to Preferences
        preferences.putString(WIFI_SSID_KEY, ssid);
        preferences.putString(WIFI_PASS_KEY, password);

        request->send(200, "text/plain", "Credentials saved. Restarting...");
        delay(1000);
        ESP.restart();
    });

    // GET /wifi-status - current WiFi status
    server.on("/wifi-status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"mode\":\"" + String(isAPMode ? "AP" : "STA") + "\",";
        json += "\"ssid\":\"" + WiFi.SSID() + "\",";
        json += "\"ip\":\"" + (isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
        json += "\"connected\":" + String(WiFi.status() == WL_CONNECTED) + "}";
        request->send(200, "application/json", json);
    });

    // POST /reset-wifi - clear saved credentials
    server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        preferences.remove(WIFI_SSID_KEY);
        preferences.remove(WIFI_PASS_KEY);
        request->send(200, "text/plain", "WiFi credentials cleared. Restarting...");
        delay(1000);
        ESP.restart();
    });

    // GET /wifi-portal - HTML configuration page
    server.on("/wifi-portal", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = R"(<!DOCTYPE html>
<html><head><title>METF WiFi Config</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:Arial;margin:20px;background:#f0f0f0}
.container{max-width:400px;margin:auto;background:white;padding:20px;border-radius:8px}
h1{color:#333;text-align:center}
.network{background:#f9f9f9;padding:10px;margin:5px 0;border-radius:4px;cursor:pointer}
.network:hover{background:#e0e0e0}
button{width:100%;padding:12px;background:#4CAF50;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px}
button:hover{background:#45a049}
input{width:100%;padding:10px;margin:8px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}
</style></head><body>
<div class="container"><h1>METF WiFi Config</h1>
<div id="networks">Scanning...</div>
<form id="wifiForm">
<input type="text" id="ssid" placeholder="SSID" required>
<input type="password" id="password" placeholder="Password" required>
<button type="submit">Connect</button>
</form></div>
<script>
fetch('/wifi-scan').then(r=>r.json()).then(nets=>{
document.getElementById('networks').innerHTML=nets.map(n=>
`<div class="network" onclick="document.getElementById('ssid').value='${n.ssid}'">${n.ssid} (${n.rssi} dBm)</div>`
).join('');
});
document.getElementById('wifiForm').onsubmit=e=>{
e.preventDefault();
const data=new FormData();
data.append('ssid',document.getElementById('ssid').value);
data.append('password',document.getElementById('password').value);
fetch('/wifi-config',{method:'POST',body:data}).then(()=>
alert('Configuration saved! Device will restart.')
);
};
</script></body></html>)";
        request->send(200, "text/html", html);
    });
}

// Handle DNS server for captive portal
void handleDNS() {
    dnsServer.processNextRequest();
}

#endif // ESP32
