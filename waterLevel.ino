#include <WiFi.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include <Wire.h>
#include <TaskScheduler.h>
#include "ESPAsyncWebServer.h"
#include <ESPmDNS.h>
#include "apps/sntp/sntp.h"
#include "SPIFFS.h"
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

const char* pref_ssid = "";
const char* pref_pass = "";
String client_wifi_ssid;
String client_wifi_password;


const char* TZ_INFO    = "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00";  // enter your time zone (https://remotemonitoringsystems.ca/time-zone-abbreviations.php)

const char* bluetooth_name = "jimka-esp32";

long start_wifi_millis;
long wifi_timeout = 10000;
bool bluetooth_disconnect = false;
bool clear_preferences_requested = false;

enum wifi_setup_stages { NONE, SCAN_START, SCAN_COMPLETE, SSID_ENTERED, WAIT_PASS, PASS_ENTERED, WAIT_CONNECT, LOGIN_FAILED };
enum wifi_setup_stages wifi_stage = NONE;

// this will assign the name PushButton to pin numer 4
const int PushButton = 4;

// form settings
uint32_t hloubka = 0;
uint32_t napust = 0;
String thingspeakApiKey = "";
unsigned long thingspeakChannel = 0;

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

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

void onSave(AsyncWebServerRequest *request) {
  preferences.begin("jimka", false);
  if (request->hasParam("hloubka", true)) {
    hloubka = atoi(request->getParam("hloubka", true)->value().c_str());
    preferences.putUInt("hloubka", hloubka);
  }

  if (request->hasParam("napust", true)) {
    napust = atoi(request->getParam("napust", true)->value().c_str());
    preferences.putUInt("napust", napust);
  }

  if (request->hasParam("thingspeakApi", true)) {
    thingspeakApiKey = request->getParam("thingspeakApi", true)->value().c_str();
    preferences.putString("thingspeakApi", String(thingspeakApiKey));
  }

  if (request->hasParam("thingspeakChannel", true)) {
    thingspeakChannel = atol(request->getParam("thingspeakChannel", true)->value().c_str());
    preferences.putULong("thingspeakChannel", thingspeakChannel);
  }

  if (request->hasParam("duckdnsDomain", true)) {
    duckdnsDomain = request->getParam("duckdnsDomain", true)->value().c_str();
    preferences.putString("duckdnsDomain", duckdnsDomain);
  }

  if (request->hasParam("duckdnsToken", true)) {
    duckdnsToken = request->getParam("duckdnsToken", true)->value().c_str();
    preferences.putString("duckdnsToken", duckdnsToken);
  }
  preferences.end();
  if (duckdnsDomain != "" && duckdnsToken != "")
    EasyDDNS.client(duckdnsDomain, duckdnsToken);

  Serial.println("save executed");
}

void startWebServer() {
  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/style.css", "text/css");
  });

  server.on("/view.css", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/view.css", "text/css");
  });

  server.on("/configuration.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/configuration.html", String(), false, processor);
  });

  server.on("/view.js", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/view.js", "text/javascript");
  });

  server.on("/blank.gif", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/blank.gif", "image/gif");
  });

  server.on("/shadow.gif", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/shadow.gif", "image/gif");
  });

  server.on("/bottom.png", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/bottom.png", "image/png");
  });

  server.on("/top.png", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/top.png", "image/png");
  });

  server.on("/iepngfix.htc", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/iepngfix.htc", "text/x-component");
  });

  server.on("/configuration.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    onSave(request);
    request->send(200, "text/plain", "Ulozeno");
  });

  server.onNotFound(notFound);
  server.begin();
}

void log(String text, bool reset = true)
{
  if (reset) {
    Serial.println(text);
  } else {
    Serial.print(text);
  }
}


void start_mdns_service()
{
  //initialize mDNS service
  esp_err_t err = mdns_init();
  if (err) {
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

void clearPreferences() {
  //log("clearPreferences executed");
  preferences.begin("wifi_access", false);
  preferences.clear();
  preferences.end();
  preferences.begin("jimka", false);
  preferences.clear();
  preferences.end();
  log("BTN pressed. Preferences deleted. Rebooting in 3 seconds");
  delay(3000);
  ESP.restart();
}

void getJimkaPreferences() {
  preferences.begin("jimka", true);
  hloubka = preferences.getUInt("hloubka", 200);
  napust = preferences.getUInt("napust", 0);
  thingspeakApiKey = preferences.getString("thingspeakApi", "");
  thingspeakChannel = preferences.getULong("thingspeakChannel", 0);
  preferences.end();
}


void isr() {
  t1.enable();
  log("button pressed");
  clear_preferences_requested = true;
}


bool getNTPtime() {
  time_t now;
  char strftime_buf[64];
  struct tm timeinfo;

  time(&now);
  setenv("TZ", TZ_INFO, 1);
  tzset();

  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
}

void getFormattedTime(tm localTime) {
  Serial.printf(
    "%04d-%02d-%02d %02d:%02d:%02d, day %d, %s time\n",
    localTime.tm_year + 1900,
    localTime.tm_mon + 1,
    localTime.tm_mday,
    localTime.tm_hour,
    localTime.tm_min,
    localTime.tm_sec,
    (localTime.tm_wday > 0 ? localTime.tm_wday : 7 ),
    (localTime.tm_isdst == 1 ? "summer" : "standard")
  );
}

String processor(const String& var) {
  //Serial.println(var);
  if (var == "NAPUST") {
    return String(napust);
  }

  if (var == "HLOUBKA") {
    return String(hloubka);
  }

  if (var == "VOLT") {
    return String(battVoltage);
  }

  if (var == "BATTPERCENT") {
    return String(battPerc);
  }

  if (var == "HLADINA") {
    int hladina;
    hladina = hloubka - distance - napust;
    return String(hladina);
  }

  if (var == "PLNOSTPERC") {
    int hladina;
    hladina = hloubka - distance - napust;
    float perc = ((float) hladina / (float) hloubka) * 100.00;
    int perc2 = round(perc);
    return String(perc2);
  }

  if (var == "TEPLOTA") {
    return String(temperature);
  }

  if (var == "VLHKOST") {
    return String(humidity);
  }

  if (var == "THINGSPEAKAPI") {
    return thingspeakApiKey;
  }

  if (var == "THINGSPEAKCHANNEL") {
    return String(thingspeakChannel);
  }

  if (var == "LASTMEASUREMENT") {
    uptime::calculateUptime();
    return String(uptime::getMinutesRaw() - uptime_minutes_received);
  }

  if (var == "UPTIME") {
    return String(uptime_formatter::getUptime());
  }

  if (var == "DUCKDNSDOMAIN") {
    return duckdnsDomain;
  }

  if (var == "DUCKDNSTOKEN") {
    return duckdnsToken;
  }
  return String();
}


bool init_wifi(String ssid, String pass)
{
  Serial.println(ssid);
  Serial.println(pass);

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

  start_wifi_millis = millis();
  WiFi.begin(ssid.c_str(), pass.c_str());
  log("\nConnecting: ");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    log(".", false);
    if (millis() - start_wifi_millis > wifi_timeout) {
      WiFi.disconnect(true, true);
      log(" failed", false);
      return false;
    }
  }
  log(" success: " + WiFi.localIP().toString(), false);
  delay(2000);
  start_mdns_service();
  add_mdns_services();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "cz.pool.ntp.org");
  sntp_init();
  setenv("TZ", TZ_INFO, 1);
  tzset();
  return true;
}

void scan_wifi_networks()
{
  WiFi.mode(WIFI_STA);
  // WiFi.scanNetworks will return the number of networks found
  int n =  WiFi.scanNetworks();
  if (n == 0) {
    SerialBT.println(F("no networks found"));
  } else {
    SerialBT.println();
    SerialBT.print(n);
    SerialBT.println(F(" networks found"));
    delay(1000);
    for (int i = 0; i < n; ++i) {
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

  if (event == ESP_SPP_SRV_OPEN_EVT) {
    wifi_stage = SCAN_START;
  }

  if (event == ESP_SPP_DATA_IND_EVT && wifi_stage == SCAN_COMPLETE) { // data from phone is SSID
    int client_wifi_ssid_id = SerialBT.readString().toInt();
    client_wifi_ssid = ssids_array[client_wifi_ssid_id];
    wifi_stage = SSID_ENTERED;
  }

  if (event == ESP_SPP_DATA_IND_EVT && wifi_stage == WAIT_PASS) { // data from phone is password
    client_wifi_password = SerialBT.readString();
    client_wifi_password.trim();
    wifi_stage = PASS_ENTERED;
  }

}

void callback_show_ip(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    SerialBT.print("ESP32 IP: ");
    SerialBT.println(WiFi.localIP());
    bluetooth_disconnect = true;
  }
}


void disconnect_bluetooth()
{
  delay(1000);
  log("BT stopping");
  SerialBT.println(F("Bluetooth disconnecting..."));
  delay(1000);
  SerialBT.flush();
  SerialBT.disconnect();
  SerialBT.end();
  log("BT stopped");
  delay(1000);
  bluetooth_disconnect = false;
}

bool receive433() {
  // Set buffer to size of expected message
  uint8_t buf[RH_ASK_MAX_MESSAGE_LEN];
  uint8_t buflen = sizeof(buf);
  // Check if received packet is correct size
  if (driver.recv(buf, &buflen))
  {
    uptime::calculateUptime();
    uptime_minutes_received = uptime::getMinutesRaw();
    int i;
    String message;
    for (i = 0; i < buflen; i++)
    {
      char c = (buf[i]);
      message = message + c ; // make a message from the received characters
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
    Serial.print("Vlhkost: ");
    Serial.print(hum);
    Serial.print(" %, Teplota: ");
    Serial.print(temp);
    Serial.println(" Celsius");
    Serial.print("Distance = ");
    Serial.print(dist);
    Serial.println(" cm");
    Serial.print("Batt percent: ");
    Serial.print(batteryPercentage);
    Serial.println("%");
    Serial.print("Batt voltage: ");
    Serial.print(batteryVoltage);
    Serial.println("V");
    return true;
  }

  return false;
}

void thingspeakSendData() {
  if (thingspeakApiKey == "" || thingspeakChannel == 0)
    return;

  ThingSpeak.setField(1, humidity);
  ThingSpeak.setField(2, temperature);
  ThingSpeak.setField(3, (int) (hloubka - distance - napust));
  ThingSpeak.setField(4, battVoltage);
  char cstr[100];
  thingspeakApiKey.toCharArray(cstr, 100);
  ThingSpeak.writeFields(thingspeakChannel, cstr);
}

String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = { 0, -1 };
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
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

  log("Booting...");

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  attachInterrupt(PushButton, isr, RISING);
  delay(2000);
  if (clear_preferences_requested) {
    clearPreferences();
  }

  preferences.begin("wifi_access", true);
  String pref_ssid = preferences.getString("pref_ssid", "");
  String pref_pass = preferences.getString("pref_pass", "");
  preferences.end();
  getJimkaPreferences();

  if (pref_ssid == "") {
    // BT config
    log("BT Configuration enabled");
    SerialBT.register_callback(callback);
  } else {
    while (!init_wifi(pref_ssid, pref_pass)) {
      runner.execute();
      log("Could not connect to the selected WiFi network, waiting 10 seconds");
      delay(10000);
      log("Retying");
    }
    SerialBT.register_callback(callback_show_ip);
    startWebServer();
  }
  SerialBT.begin(bluetooth_name);

  if (!driver.init())
    Serial.println("433 MHz init failed");

  ThingSpeak.begin(client);  // Initialize ThingSpeak
  EasyDDNS.service("duckdns");
  if (duckdnsDomain != "" && duckdnsToken != "")
    EasyDDNS.client(duckdnsDomain, duckdnsToken);
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
      log("Scanning Wi-Fi networks");
      scan_wifi_networks();
      SerialBT.println(F("Please enter the number for your Wi-Fi"));
      wifi_stage = SCAN_COMPLETE;
      break;

    case SSID_ENTERED:
      SerialBT.println(F("Please enter your Wi-Fi password"));
      log("Please enter your Wi-Fi password");
      wifi_stage = WAIT_PASS;
      break;

    case PASS_ENTERED:
      SerialBT.println(F("Please wait for Wi-Fi connection..."));
      log("Please wait for Wi_Fi connection...");
      wifi_stage = WAIT_CONNECT;
      preferences.begin("wifi_access", false);
      preferences.putString("pref_ssid", client_wifi_ssid);
      preferences.putString("pref_pass", client_wifi_password);
      preferences.end();
      if (init_wifi(client_wifi_ssid, client_wifi_password)) { // Connected to WiFi
        connected_string = "ESP32 IP: ";
        connected_string = connected_string + WiFi.localIP().toString();
        SerialBT.println(connected_string);
        log(connected_string);
        bluetooth_disconnect = true;
      } else { // try again
        wifi_stage = LOGIN_FAILED;
      }
      break;

    case LOGIN_FAILED:
      SerialBT.println(F("Wi-Fi connection failed"));
      log("Wi-Fi connection failed");
      delay(2000);
      wifi_stage = SCAN_START;
      break;
  }

  bool r433 = receive433();
  if (WiFi.localIP().toString() != "0.0.0.0") {
    if (duckdnsDomain != "" && duckdnsToken != "")
      EasyDDNS.update(10000, true);

    if (r433)
      thingspeakSendData();
  }
}
