#include <WiFi.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include <U8x8lib.h>
#include <Wire.h>
#include <TaskScheduler.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "apps/sntp/sntp.h"


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

enum wifi_setup_stages { NONE, SCAN_START, SCAN_COMPLETE, SSID_ENTERED, WAIT_PASS, PASS_ENTERED, WAIT_CONNECT, LOGIN_FAILED };
enum wifi_setup_stages wifi_stage = NONE;

// this will assign the name PushButton to pin numer 4
const int PushButton = 4;

U8X8_SSD1306_128X64_NONAME_SW_I2C u8x8(/* clock=*/ 23, /* data=*/ 18);
WebServer server(80);

BluetoothSerial SerialBT;
Preferences preferences;

void clearPreferences();
Task t1(1000, 1, &clearPreferences);
Scheduler runner;

void log(String text, bool reset = true)
{
  if (reset) {
    Serial.println(text);
    u8x8.print(F("                 \n                 \n                 \n                 "));
    u8x8.home();
  } else {
    Serial.print(text);
  }
  // printf ("%.*s%s\n", (sz < strlen(s)) ? 0 : sz - strlen(s), pad, s);
  u8x8.print(wrap(text, 16));
}

String wrap(String s, int limit) {
  int space = 0;
  int i = 0;
  int line = 0;
  while (i < s.length()) {

    if (s.substring(i, i + 1) == " ") {
      space = i;
    }
    if (line > limit - 1) {
      s = s.substring(0, space) + "~" + s.substring(space + 1);
      line = 0;
    }
    i++; line++;
  }
  s.replace("~", "\n");
  return s;
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
  preferences.clear();
  log("BTN pressed. Preferences deleted. Rebooting in 3 seconds");
  delay(3000);
  ESP.restart();
}

void isr() {
  t1.enable();
  log("button pressed");
}

void webIndex() {
  // načtení hodnoty analogového pinu a času
  // od spuštění Arduina ve formátu String
  // String analog = String(analogRead(analogPin));
  String cas = String(millis() / 1000);
  // vytvoření zprávy, která bude vytištěna
  // v prohlížeči (<br> znamená nový řádek)
  String zprava = "Ahoj Arduino svete!<br>";
  //zprava += "Analogovy pin A0: ";
  //zprava += analog;
  zprava += "<br>Cas od spusteni Arduina je ";
  zprava += cas;
  zprava += " vterin.<br><br>";
  //zprava += "Stav LED: ";
  //zprava += digitalRead(LEDka);
  zprava += "<br><br>";
  zprava += "<a href=\"/ledON\"\">Zapni LEDku</a><br><br>";
  zprava += "<a href=\"/ledOFF\"\">Vypni LEDku</a>";
  // vytištění zprávy se statusem 200 - OK
  server.send(200, "text/html", zprava);
}

void web404() {
  // vytvoření zprávy s informací o neexistujícím odkazu
  // včetně metody a zadaného argumentu
  String zprava = "Neexistujici odkaz\n\n";
  zprava += "URI: ";
  zprava += server.uri();
  zprava += "\nMetoda: ";
  zprava += (server.method() == HTTP_GET) ? "GET" : "POST";
  zprava += "\nArgumenty: ";
  zprava += server.args();
  zprava += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    zprava += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  // vytištění zprávy se statusem 404 - Nenalezeno
  server.send(404, "text/plain", zprava);
}

bool getNTPtime() {
  time_t now;
  char strftime_buf[64];
  struct tm timeinfo;

  time(&now);
  // Set timezone to China Standard Time
  setenv("TZ", "CST-8", 1);
  tzset();

  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGI(TAG, "The current date/time in Shanghai is: %s", strftime_buf);
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

void setup()
{
  runner.init();
  runner.addTask(t1);
  Serial.begin(115200);

  u8x8.begin();
  u8x8.setFont(u8x8_font_amstrad_cpc_extended_r);
  log("Booting...");

  preferences.begin("wifi_access", false);
  //int Push_button_state = digitalRead(PushButton);
  attachInterrupt(PushButton, isr, RISING);

  String pref_ssid = preferences.getString("pref_ssid", "");
  String pref_pass = preferences.getString("pref_pass", "");

  if (pref_ssid == "") {
    // BT config
    log("BT Configuration enabled");
    SerialBT.register_callback(callback);
  } else {
    while (!init_wifi(pref_ssid, pref_pass)) {
      log("Could not connect to the selected WiFi network, waiting 10 seconds");
      delay(10000);
      log("Retying");
      runner.execute();
    }
    SerialBT.register_callback(callback_show_ip);
  }
  SerialBT.begin(bluetooth_name);
  server.on("/", webIndex);
  server.onNotFound(web404);
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
  log(" success", false);
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

void loop()
{
  runner.execute();
  if (bluetooth_disconnect)
  {
    disconnect_bluetooth();
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
      preferences.putString("pref_ssid", client_wifi_ssid);
      preferences.putString("pref_pass", client_wifi_password);
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

  if (WiFi.localIP().toString() != "0.0.0.0") {
    //log("Connected " + WiFi.localIP().toString());
    server.handleClient();
  }
  delay(20);
}
