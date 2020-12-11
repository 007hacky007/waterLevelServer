#include <Arduino.h>
#include <AsyncTCP.h>
#include <WiFi.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include <Wire.h>
#include <TaskScheduler.h>
#include "ESPAsyncWebServer.h"
#include <ESPmDNS.h>
#include "LittleFS.h"
#include "ThingSpeak.h"
#include "uptime.h"
#include "uptime_formatter.h"
#include <EasyDDNS.h>
#include <math.h>
#include <RH_ASK.h>
#include <SPI.h> // Not actually used but needed to compile

/*
    This code works only with ESP 1.4.0 version */

WiFiClient client; // for thingspeak

String ssids_array[50];
String network_string;
String connected_string;

const char *pref_ssid = "";
const char *pref_pass = "";
String client_wifi_ssid;
String client_wifi_password;

const char *TZ_INFO = "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00"; // enter your time zone (https://remotemonitoringsystems.ca/time-zone-abbreviations.php)

const char *bluetooth_name = "jimka-esp32";

long start_wifi_millis;
long wifi_timeout = 10000;
bool bluetooth_disconnect = false;
bool clear_preferences_requested = false;
bool data_received = false;

enum wifi_setup_stages
{
    NONE,
    SCAN_START,
    SCAN_COMPLETE,
    SSID_ENTERED,
    WAIT_PASS,
    PASS_ENTERED,
    WAIT_CONNECT,
    LOGIN_FAILED
};
enum wifi_setup_stages wifi_stage = NONE;

// this will assign the name PushButton to pin numer 4
const int PushButton = 4;

// form settings
uint32_t hloubka = 0;
uint32_t napust = 0;
String thingspeakApiKey = "";
uint32_t thingspeakChannel = 0;

float humidity = 0;
float temperature = 0;
uint32_t distance = 0;
int battPerc = 0;
float battVoltage = 0;
unsigned long uptime_minutes_received = 0;
String duckdnsDomain = "";
String duckdnsToken = "";

AsyncWebServer server(80);

BluetoothSerial SerialBT;
Preferences preferences;

void clearPreferences();
Task t1(1000, 1, &clearPreferences);
Scheduler runner;

RH_ASK driver(2000, 13); // 200bps

void notFound(AsyncWebServerRequest *request);
void onSave(AsyncWebServerRequest *request);
void startWebServer();
void log(String text, bool reset = true);
void start_mdns_service();
void add_mdns_services();
void clearPreferences();
void getJimkaPreferences();
void isr();
String checkNoData(String string, String altNoDataText = "");
String processor(const String &var);
bool init_wifi(String ssid, String pass);
void scan_wifi_networks();
void callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
void callback_show_ip(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
void disconnect_bluetooth();
bool receive433();
void thingspeakSendData();
String getValue(String data, char separator, int index);
bool loadFromLittleFS(AsyncWebServerRequest *request, String path, String dataType);

void notFound(AsyncWebServerRequest *request)
{
    request->send(404, F("text/plain"), F("Not found"));
}

void onSave(AsyncWebServerRequest *request)
{
    preferences.begin("jimka", false);
    if (request->hasParam(F("hloubka"), true))
    {
        hloubka = atoi(request->getParam(F("hloubka"), true)->value().c_str());
        preferences.putUInt("hloubka", hloubka);
    }

    if (request->hasParam(F("napust"), true))
    {
        napust = atoi(request->getParam(F("napust"), true)->value().c_str());
        preferences.putUInt("napust", napust);
    }

    if (request->hasParam(F("thingspeakApi"), true))
    {
        thingspeakApiKey = request->getParam(F("thingspeakApi"), true)->value().c_str();
        preferences.putString("thingspeakApi", String(thingspeakApiKey));
    }

    if (request->hasParam(F("thingspeakChannel"), true))
    {
        thingspeakChannel = atol(request->getParam(F("thingspeakChannel"), true)->value().c_str());
        preferences.putUInt("thingspeakChann", thingspeakChannel);
    }

    if (request->hasParam(F("duckdnsDomain"), true))
    {
        duckdnsDomain = request->getParam(F("duckdnsDomain"), true)->value().c_str();
        preferences.putString("duckdnsDomain", duckdnsDomain);
    }

    if (request->hasParam(F("duckdnsToken"), true))
    {
        duckdnsToken = request->getParam(F("duckdnsToken"), true)->value().c_str();
        preferences.putString("duckdnsToken", duckdnsToken);
    }
    preferences.end();
    if (duckdnsDomain != "" && duckdnsToken != "")
        EasyDDNS.client(duckdnsDomain, duckdnsToken);

    Serial.println(F("save executed"));
}

bool loadFromLittleFS(AsyncWebServerRequest *request, String path, String dataType)
{
    //Serial.print("Requested page -> ");
    //Serial.println(path);
    if (LITTLEFS.exists(path + ".gz"))
    {
        File dataFile = LITTLEFS.open(path + ".gz", "r");
        if (!dataFile)
        {
            notFound(request);
            return false;
        }

        AsyncWebServerResponse *response = request->beginResponse(LITTLEFS, path, dataType);
        //Serial.print("Real file path: ");
        //Serial.println(path);

        request->send(response);

        dataFile.close();
    }
    else
    {
        notFound(request);
        return false;
    }
    return true;
}

void startWebServer()
{
    server.on("/css/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/css/style.css", "text/css");
    });
    server.on("/js/bootstrap.bundle.min.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/js/bootstrap.bundle.min.js", "text/javascript");
    });
    server.on("/css/roboto.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/css/roboto.css", "text/css");
    });
    server.on("/js/jquery-3.5.1.min.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/js/jquery-3.5.1.min.js", "text/javascript");
    });
    server.on("/webfonts/fa-solid-900.woff", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/webfonts/fa-solid-900.woff", "font/woff");
    });
    server.on("/webfonts/roboto_c9.ttf", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/webfonts/roboto_c9.ttf", "font/ttf");
    });
    server.on("/webfonts/roboto_xP.ttf", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/webfonts/roboto_xP.ttf", "font/ttf");
    });
    server.on("/css/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/css/bootstrap.min.css", "text/css");
    });
    server.on("/css/fontawesome.min.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/css/fontawesome.min.css", "text/css");
    });
    server.on("/css/solid.min.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        loadFromLittleFS(request, "/css/solid.min.css", "text/css");
    });
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LITTLEFS, "/index.html", String(), false, processor);
    });
    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LITTLEFS, "/index.html", String(), false, processor);
    });
    server.on("/graphs.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LITTLEFS, "/graphs.html", String(), false, processor);
    });
    server.on("/configuration.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LITTLEFS, "/configuration.html", String(), false, processor);
    });

    server.on("/configuration.html", HTTP_POST, [](AsyncWebServerRequest *request) {
        onSave(request);
        request->send(200, F("text/plain"), F("Ulozeno"));
    });

    server.onNotFound(notFound);
    server.begin();
}

void log(String text, bool reset)
{
    if (reset)
    {
        Serial.println(text);
    }
    else
    {
        Serial.print(text);
    }
}

void start_mdns_service()
{
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err)
    {
        printf("MDNS Init failed: %d\n", err);
        return;
    }

    //set hostname
    mdns_hostname_set("jimka");
    //set default instance
    mdns_instance_name_set("Sledovani hladiny jimky");
}

void add_mdns_services()
{
    //add our services
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    //NOTE: services must be added before their properties can be set
    //use custom instance for the web server
    mdns_service_instance_name_set("_http", "_tcp", "Sledovani hladiny jimky");
}

void clearPreferences()
{
    //log("clearPreferences executed");
    preferences.begin("wifi_access", false);
    preferences.clear();
    preferences.end();
    preferences.begin("jimka", false);
    preferences.clear();
    preferences.end();
    log(F("BTN pressed. Preferences deleted. Rebooting in 3 seconds"));
    delay(3000);
    ESP.restart();
}

void getJimkaPreferences()
{
    preferences.begin("jimka", true);
    hloubka = preferences.getUInt("hloubka", 200);
    napust = preferences.getUInt("napust", 0);
    thingspeakApiKey = preferences.getString("thingspeakApi", "");
    thingspeakChannel = preferences.getUInt("thingspeakChann");
    duckdnsToken = preferences.getString("duckdnsToken", "");
    duckdnsDomain = preferences.getString("duckdnsDomain", "");
    preferences.end();
}

void isr()
{
    t1.enable();
    log(F("button pressed"));
    clear_preferences_requested = true;
}

String checkNoData(String string, String altNoDataText)
{
    if (!data_received)
    {
        if (altNoDataText == "")
        {
            return String(F("cekam na data..."));
        }
        else
        {
            return altNoDataText;
        }
    }
    else
    {
        return string;
    }
}

String processor(const String &var)
{
    if (var == F("NAPUST"))
    {
        return String(napust);
    }

    if (var == F("HLOUBKA"))
    {
        return String(hloubka);
    }

    if (var == F("VOLT"))
    {
        return checkNoData(String(battVoltage));
    }

    if (var == F("BATTPERCENT"))
    {
        return checkNoData(String(battPerc), String(F("0")));
    }

    if (var == F("HLADINA"))
    {
        int hladina;
        hladina = hloubka - distance - napust;
        return checkNoData(String(hladina));
    }

    if (var == F("PLNOSTPERC"))
    {
        int hladina;
        hladina = hloubka - distance - napust;
        float perc = ((float)hladina / (float)hloubka) * 100.00;
        int perc2 = round(perc);
        return checkNoData(String(perc2), String(F("0")));
    }

    if (var == F("TEPLOTA"))
    {
        return checkNoData(String(temperature));
    }

    if (var == F("VLHKOST"))
    {
        return checkNoData(String(humidity));
    }

    if (var == F("THINGSPEAKAPI"))
    {
        return thingspeakApiKey;
    }

    if (var == F("THINGSPEAKCHANNEL"))
    {
        return String(thingspeakChannel);
    }

    if (var == F("LASTMEASUREMENT"))
    {
        uptime::calculateUptime();
        return checkNoData(String(uptime::getMinutesRaw() - uptime_minutes_received),
                           String(F("-")));
    }

    if (var == F("UPTIME"))
    {
        return String(uptime_formatter::getUptime());
    }

    if (var == F("DUCKDNSDOMAIN"))
    {
        return duckdnsDomain;
    }

    if (var == F("DUCKDNSTOKEN"))
    {
        return duckdnsToken;
    }
    return String();
}

bool init_wifi(String ssid, String pass)
{
    Serial.println(ssid);
    //Serial.println(pass);

    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

    start_wifi_millis = millis();
    WiFi.begin(ssid.c_str(), pass.c_str());
    WiFi.setHostname("jimka-esp32");
    log(F("\nConnecting: "));
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        log(".", false);
        if (millis() - start_wifi_millis > wifi_timeout)
        {
            WiFi.disconnect(true, true);
            log(F(" failed"), false);
            return false;
        }
    }
    log(" success: " + WiFi.localIP().toString(), false);
    delay(2000);
    start_mdns_service();
    add_mdns_services();
    return true;
}

void scan_wifi_networks()
{
    WiFi.mode(WIFI_STA);
    // WiFi.scanNetworks will return the number of networks found
    int n = WiFi.scanNetworks();
    if (n == 0)
    {
        SerialBT.println(F("no networks found"));
    }
    else
    {
        SerialBT.println();
        SerialBT.print(n);
        SerialBT.println(F(" networks found"));
        delay(1000);
        for (int i = 0; i < n; ++i)
        {
            ssids_array[i + 1] = WiFi.SSID(i);
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.println(ssids_array[i + 1]);
            network_string = i + 1;
            network_string = network_string + ": " + WiFi.SSID(i) + " (Strength:" + WiFi.RSSI(i) + ")";
            SerialBT.println(network_string);
        }
        wifi_stage = SCAN_COMPLETE;
    }
}

void callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{

    if (event == ESP_SPP_SRV_OPEN_EVT)
    {
        wifi_stage = SCAN_START;
    }

    if (event == ESP_SPP_DATA_IND_EVT && wifi_stage == SCAN_COMPLETE)
    { // data from phone is SSID
        int client_wifi_ssid_id = SerialBT.readString().toInt();
        client_wifi_ssid = ssids_array[client_wifi_ssid_id];
        wifi_stage = SSID_ENTERED;
    }

    if (event == ESP_SPP_DATA_IND_EVT && wifi_stage == WAIT_PASS)
    { // data from phone is password
        client_wifi_password = SerialBT.readString();
        client_wifi_password.trim();
        wifi_stage = PASS_ENTERED;
    }
}

void callback_show_ip(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    if (event == ESP_SPP_SRV_OPEN_EVT)
    {
        SerialBT.print(F("ESP32 IP: "));
        SerialBT.println(WiFi.localIP());
        bluetooth_disconnect = true;
    }
}

void disconnect_bluetooth()
{
    delay(1000);
    log(F("BT stopping"));
    SerialBT.println(F("Bluetooth disconnecting..."));
    delay(1000);
    SerialBT.flush();
    SerialBT.disconnect();
    SerialBT.end();
    log(F("BT stopped"));
    delay(1000);
    bluetooth_disconnect = false;
}

bool receive433()
{
    // Set buffer to size of expected message
    uint8_t buf[RH_ASK_MAX_MESSAGE_LEN];
    uint8_t buflen = sizeof(buf);
    // Check if received packet is correct size
    if (driver.recv(buf, &buflen))
    {
        data_received = true;
        uptime::calculateUptime();
        uptime_minutes_received = uptime::getMinutesRaw();
        int i;
        String message;
        for (i = 0; i < buflen; i++)
        {
            char c = (buf[i]);
            message = message + c; // make a message from the received characters
        }
        String hum = getValue(message, ',', 0);
        String temp = getValue(message, ',', 1);
        String dist = getValue(message, ',', 2);
        String batteryPercentage = getValue(message, ',', 3);
        String batteryVoltage = getValue(message, ',', 4);

        humidity = hum.toFloat();
        temperature = temp.toFloat();
        distance = dist.toInt();
        battPerc = batteryPercentage.toInt();
        battVoltage = batteryVoltage.toFloat();

        // Message received with valid checksum
        Serial.print(F("Vlhkost: "));
        Serial.print(hum);
        Serial.print(F(" %, Teplota: "));
        Serial.print(temp);
        Serial.println(F(" Celsius"));
        Serial.print(F("Distance = "));
        Serial.print(dist);
        Serial.println(F(" cm"));
        Serial.print(F("Batt percent: "));
        Serial.print(batteryPercentage);
        Serial.println(F("%"));
        Serial.print(F("Batt voltage: "));
        Serial.print(batteryVoltage);
        Serial.println(F("V"));
        return true;
    }

    return false;
}

void thingspeakSendData()
{
    if (thingspeakApiKey == "" || thingspeakChannel == 0)
        return;

    ThingSpeak.setField(1, humidity);
    ThingSpeak.setField(2, temperature);
    ThingSpeak.setField(3, (int)(hloubka - distance - napust));
    ThingSpeak.setField(4, battVoltage);
    char cstr[100];
    thingspeakApiKey.toCharArray(cstr, 100);
    ThingSpeak.writeFields(thingspeakChannel, cstr);
}

String getValue(String data, char separator, int index)
{
    int found = 0;
    int strIndex[] = {0, -1};
    int maxIndex = data.length() - 1;

    for (int i = 0; i <= maxIndex && found <= index; i++)
    {
        if (data.charAt(i) == separator || i == maxIndex)
        {
            found++;
            strIndex[0] = strIndex[1] + 1;
            strIndex[1] = (i == maxIndex) ? i + 1 : i;
        }
    }
    return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void setup()
{
    runner.init();
    runner.addTask(t1);
    Serial.begin(115200);

    log(F("Booting..."));

    // Initialize SPIFFS
    if (!LITTLEFS.begin(false, "/littlefs", 100))
    {
        Serial.println(F("An Error has occurred while mounting LITTLEFS"));
        return;
    }

    attachInterrupt(PushButton, isr, RISING);
    delay(2000);
    if (clear_preferences_requested)
    {
        clearPreferences();
    }

    preferences.begin("wifi_access", true);
    String pref_ssid = preferences.getString("pref_ssid", "");
    String pref_pass = preferences.getString("pref_pass", "");
    preferences.end();
    getJimkaPreferences();

    if (pref_ssid == "")
    {
        // BT config
        log(F("BT Configuration enabled"));
        SerialBT.register_callback(callback);
        SerialBT.begin(bluetooth_name);
    }
    else
    {
        while (!init_wifi(pref_ssid, pref_pass))
        {
            runner.execute();
            log(F("Could not connect to the selected WiFi network, waiting 10 seconds"));
            delay(10000);
            log(F("Retying"));
        }
        SerialBT.register_callback(callback_show_ip);
        startWebServer();
    }
    Serial.println("Before 433 Mhz init");

    if (!driver.init())
        Serial.println(F("433 MHz init failed"));

    ThingSpeak.begin(client); // Initialize ThingSpeak
    Serial.println("before easyDDNS");
    EasyDDNS.service(F("duckdns"));
    if (duckdnsDomain != "" && duckdnsToken != "")
        EasyDDNS.client(duckdnsDomain, duckdnsToken);

    Serial.println("setup done");
}

void loop()
{
    runner.execute();
    if (bluetooth_disconnect)
    {
        disconnect_bluetooth();
        startWebServer();
    }

    switch (wifi_stage)
    {
    case SCAN_START:
        SerialBT.println(F("Scanning Wi-Fi networks"));
        log(F("Scanning Wi-Fi networks"));
        scan_wifi_networks();
        SerialBT.println(F("Please enter the number for your Wi-Fi"));
        wifi_stage = SCAN_COMPLETE;
        break;

    case SSID_ENTERED:
        SerialBT.println(F("Please enter your Wi-Fi password"));
        log(F("Please enter your Wi-Fi password"));
        wifi_stage = WAIT_PASS;
        break;

    case PASS_ENTERED:
        SerialBT.println(F("Please wait for Wi-Fi connection..."));
        log(F("Please wait for Wi_Fi connection..."));
        wifi_stage = WAIT_CONNECT;
        preferences.begin("wifi_access", false);
        preferences.putString("pref_ssid", client_wifi_ssid);
        preferences.putString("pref_pass", client_wifi_password);
        preferences.end();
        if (init_wifi(client_wifi_ssid, client_wifi_password))
        { // Connected to WiFi
            connected_string = "ESP32 IP: ";
            connected_string = connected_string + WiFi.localIP().toString();
            SerialBT.println(connected_string);
            log(connected_string);
            bluetooth_disconnect = true;
        }
        else
        { // try again
            wifi_stage = LOGIN_FAILED;
        }
        break;

    case LOGIN_FAILED:
        SerialBT.println(F("Wi-Fi connection failed"));
        log(F("Wi-Fi connection failed"));
        delay(2000);
        wifi_stage = SCAN_START;
        break;
    }

    bool r433 = receive433();
    if (WiFi.localIP().toString() != "0.0.0.0")
    {
        if (duckdnsDomain != "" && duckdnsToken != "")
            EasyDDNS.update(10000, true);

        if (r433)
            thingspeakSendData();
    }
}
