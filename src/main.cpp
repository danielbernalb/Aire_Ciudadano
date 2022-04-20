//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// AireCiudadano medidor Fijo - Medidor de PM2.5 abierto, medición opcional de humedad y temperatura.
// Más información en: aireciudadano.com
// Este firmware es un fork del proyecto Anaire (https://www.anaire.org/) recomendado para la medición de CO2.
// 19/04/2022 info@aireciudadano.com
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Pendientes:
// Nombre de la estacion con modelo de sensor, board y etiqueta propia si es posible
// Programacion de modelo de sensor por portal cautivo
// BT funcionando en este codigo y sin WIFI y encendiendo sensor con pin enable
// TTGO T Display funcionando
// Agregar comparacion de valores de PM25 para emoticons y colores
// OLED funcionando
// Valor de RSSI cuando la wifi este encendida
// Añadir coordenadas GPS
// Añadir VOCs y NOx para SEN5X

#include <Arduino.h>
#include "main.hpp"

#define BrownoutOFF false // Colocar en true en boards con problemas de RESET por Brownout o bajo voltaje
#define TDisplay false  // Set to true if Board TTGO T-Display is used
#define OLED false      // Set to true if you use a OLED Diplay
#define BLUETOOTH false // Set to true in case bluetooth is desired
#define SPS30sen true   // Sensor Sensirion SPS30
#define SEN5Xsen false  // Sensor Sensirion SEN5X
#define PMSsen false    // Sensor Plantower PMS
#define SHT31sen true  // Sensor DHT31 humedad y temperatura
#define AM2320sen false // Sensor AM2320 humedad y temperatura

// device id, automatically filled by concatenating the last three fields of the wifi mac address, removing the ":" in betweeen, in HEX format. Example: ChipId (HEX) = 85e646, ChipId (DEC) = 8775238, macaddress = E0:98:06:85:E6:46
String sw_version = "v0.4";
String anaire_device_id;

// Init to default values; if they have been chaged they will be readed later, on initialization
struct MyConfigStruct
{
  char anaire_device_name[24];                       // Device name; default to anaire_device_id
  uint16_t PM25_warning_threshold = 700;             // Warning threshold; default to 700ppm
  uint16_t PM25_alarm_threshold = 1000;              // Alarm threshold; default to 1000ppm
  char MQTT_server[32] = "sensor.aireciudadano.com"; // MQTT server url or public IP address. Default to Anaire Portal on portal.anaire.org
  uint16_t MQTT_port = 30183;                        // MQTT port; Default to Anaire Port on 30183
  uint16_t temperature_offset = 600;                 // temperature offset for SCD30 CO2 measurements: 600 by default, because of the housing
  uint16_t altitude_compensation = 600;              // altitude compensation for SCD30 CO2 measurements: 600, Madrid altitude
  char wifi_user[24];                                // WiFi user to be used on WPA Enterprise. Default to null (not used)
  char wifi_password[24];                            // WiFi password to be used on WPA Enterprise. Default to null (not used)
} eepromConfig;

#ifdef BrownoutOFF
//OFF BROWNOUT/////////////////////
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif

// to store data on nvs partition
#include <Preferences.h>
Preferences preferences;

// Measurements
float PM25_value = 0;       // PM25 measured value
float PM25_accumulated = 0; // Accumulates pm25 measurements for a MQTT period
float temperature;          // Read temperature as Celsius
float humidity;             // Read humidity in %
int PM25_samples = 0;       // Counts de number of samples for a MQTT period
int pm25int;                // PM25 publicado
int temp;
int humi;

// PM25 sensors
enum PM25_sensors
{
  none,
  sen5x_sensor,
  sps30_sensor,
  pms_sensor
}; // possible sensors integrated in the SW
PM25_sensors pm25_sensor = none;

// PM25 device status
enum pm25_status
{
  pm25_ok,
  pm25_warning,
  pm25_alarm
};                                        // the device can have one of those PM25 status
pm25_status pm25_device_status = pm25_ok; // initialized to ok

// device status
boolean err_global = false;
boolean err_wifi = false;
boolean err_MQTT = false;
boolean err_sensor = false;

// Measurements loop: time between measurements
unsigned int measurements_loop_duration = 1000; // 1 second
unsigned long measurements_loop_start;          // holds a timestamp for each control loop start

// MQTT loop: time between MQTT measurements sent to the cloud
unsigned int MQTT_loop_duration = 60000; // 60 seconds
unsigned long MQTT_loop_start;           // holds a timestamp for each cloud loop start
unsigned long lastReconnectAttempt = 0;  // MQTT reconnections

// Errors loop: time between error condition recovery
unsigned int errors_loop_duration = 60000; // 60 seconds
unsigned long errors_loop_start;           // holds a timestamp for each error loop start

// TTGO ESP32 board
#include "esp_timer.h"
#include <Wire.h>
#include <esp_system.h>

#if TDisplay

// Display and fonts
#include <TFT_eSPI.h>
#include <SPI.h>
//#include "SensirionSimple25pt7b.h"
#include "ArchivoNarrow_Regular10pt7b.h"
#include "ArchivoNarrow_Regular50pt7b.h"
#define GFXFF 1
//#define FF99  &SensirionSimple25pt7b
#define FF90 &ArchivoNarrow_Regular10pt7b
#define FF95 &ArchivoNarrow_Regular50pt7b
TFT_eSPI tft = TFT_eSPI(135, 240); // Invoke library, pins defined in User_Setup.h

// Customized Anaire splash screen
#include "anaire_ttgo_splash.h"

// Buttons: Top and bottom considered when USB connector is positioned on the right of the board
#include "Button2.h"
#define BUTTON_TOP 35
#define BUTTON_BOTTOM 0
Button2 button_top(BUTTON_TOP);
Button2 button_bottom(BUTTON_BOTTOM);

// Define ADC PIN for battery voltage measurement
#define ADC_PIN 34
float battery_voltage;
int vref = 1100;

// Define voltage threshold
#define USB_Voltage 4.0
#define Voltage_Threshold_1 3.9
#define Voltage_Threshold_2 3.7
#define Voltage_Threshold_3 3.5
#define Voltage_Threshold_4 3.3

#endif

#define Sensor_SDA_pin 21 // Define the SDA pin used for the SCD30
#define Sensor_SCL_pin 22 // Define the SCL pin used for the SCD30

#if SPS30sen
#include <sps30.h>
SPS30 sps30;
#define SP30_COMMS Wire
#define DEBUG 0
// bool SPS30flag = false;
#endif

#if SEN5Xsen
#include <SensirionI2CSen5x.h>

// The used commands use up to 48 bytes. On some Arduino's the default buffer space is not large enough
#define MAXBUF_REQUIREMENT 48

#if (defined(I2C_BUFFER_LENGTH) &&                 \
     (I2C_BUFFER_LENGTH >= MAXBUF_REQUIREMENT)) || \
    (defined(BUFFER_LENGTH) && BUFFER_LENGTH >= MAXBUF_REQUIREMENT)
#define USE_PRODUCT_INFO
#endif

SensirionI2CSen5x sen5x;
// bool SEN5Xflag = false;
float massConcentrationPm1p0;
float massConcentrationPm2p5;
float massConcentrationPm4p0;
float massConcentrationPm10p0;
float ambientHumidity;
float ambientTemperature;
float vocIndex;
float noxIndex;

#endif

#if PMSsen

#include "PMS.h"
PMS pms(Serial1);
PMS::DATA data;
// bool PMSflag = false;
#define PMS_TX 17 // PMS TX pin
#define PMS_RX 15 // PMS RX pin

#endif

#include <Adafruit_SHT31.h>
Adafruit_SHT31 sht31;
bool SHT31flag = false;
byte failh = 0;

// Adafruit_SHT31 sht31 = Adafruit_SHT31();

#include "Adafruit_Sensor.h"
#include "Adafruit_AM2320.h"
Adafruit_AM2320 am2320 = Adafruit_AM2320();
bool AM2320flag = false;

// Bluetooth in TTGO T-Display
#if BLUETOOTH
#include "Sensirion_GadgetBle_Lib.h" // to connect to Sensirion MyAmbience Android App available on Google Play
// GadgetBle gadgetBle = GadgetBle(GadgetBle::DataType::T_RH_CO2_ALT);
GadgetBle gadgetBle = GadgetBle(GadgetBle::DataType::T_RH_VOC_PM25_V2);
bool bluetooth_active = false;
#endif

// AZ-Delivery Active Buzzer
//#define BUZZER_GPIO 12 // signal GPIO12 (pin TOUCH5/ADC15/GPIO12 on TTGO)

// WiFi
//#include <WiFi.h>
#include <WiFiManager.h>                // https://github.com/tzapu/WiFiManager
#include "esp_wpa2.h"                   //wpa2 library for connections to Enterprise networks
const int WIFI_CONNECT_TIMEOUT = 10000; // 10 seconds
// const int WIFI_CONNECT_TIMEOUT = 1000; // 1 seconds !!! TEST SEN5X
WiFiServer wifi_server(80);
WiFiClient wifi_client;
bool PortalFlag = false;

// MQTT
#include <PubSubClient.h>
char MQTT_message[256];
PubSubClient MQTT_client(wifi_client);
char received_payload[384];
String MQTT_send_topic;
String MQTT_receive_topic;

// JSON
#include <ArduinoJson.h>
StaticJsonDocument<384> jsonBuffer;

// OTA Update
#include <HTTPClient.h>
#include <HTTPUpdate.h>

#include "rom/rtc.h"
bool ResetFlag = false;
// int reason;
void print_reset_reason(RESET_REASON reason);

// to know when there is an updating process in place
bool updating = false;

// To know when the device is in the following states
bool InCaptivePortal = false;
bool Calibrating = false;

///////////////////////////////////////////////////////////////////////////////////////////////////
// SETUP
///////////////////////////////////////////////////////////////////////////////////////////////////

void setup()
{

  // Initialize serial port for serial monitor in Arduino IDE
  Serial.begin(115200);
  while (!Serial)
  {
    delay(500); // wait 0.5 seconds for connection
  }
  Serial.setDebugOutput(true);

  Serial.println("CPU0 reset reason:");
  print_reset_reason(rtc_get_reset_reason(0));

  // print info
  Serial.println();
  Serial.println("### Inicializando Medidor Aire Ciudadano #################################");

#if BrownoutOFF
//OFF BROWNOUT/////////////////////
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable   detector
#endif

#if TDisplay
  // Initialize TTGO Display and show Anaire splash screen
  Display_Init();
  Display_Splash_Screen();
#endif

  // init preferences to handle persitent config data
  preferences.begin("config"); // use "config" namespace

  // Get device id
  Get_Anaire_DeviceId();

  // Set MQTT topics
  MQTT_send_topic = "measurement";                   // Measurements are sent to this topic
  MQTT_receive_topic = "config/" + anaire_device_id; // Config messages will be received in config/id

  // Read EEPROM config values
  // Wipe_EEPROM();
  Read_EEPROM();
  Print_Config();

// Initialize the GadgetBle Library for Bluetooth
#if BLUETOOTH
  gadgetBle.begin();
  Serial.print("Sensirion GadgetBle Lib initialized with deviceId = ");
  Serial.println(gadgetBle.getDeviceIdString());
#endif

#if TDisplay
  // Initialize TTGO board buttons
  Button_Init();
#endif

  // Start Captive Portal for 15 seconds
  if (ResetFlag == true)
  {
    Start_Captive_Portal();
    delay(100);
  }

  // Attempt to connect to WiFi network:
  Connect_WiFi();

  // Attempt to connect to MQTT broker
  if (!err_wifi)
  {
    Init_MQTT();
  }

  // Initialize and warm up PM25 sensor
  Setup_Sensor();

  // Init control loops
  measurements_loop_start = millis();
  MQTT_loop_start = millis();
  errors_loop_start = millis();

  Serial.println("");
  Serial.println("### Configuración del medidor AireCiudadano finalizada ###\n");
#if TDisplay
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.setTextDatum(6); // bottom left
  tft.setTextSize(1);
  tft.setFreeFont(FF90);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Medidor AireCiudadano", tft.width() / 2, tft.height() / 2);
  delay(1000);

  // Update display with new values
  Update_Display();
#else
  delay(1000);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// CONTROL LOOP
///////////////////////////////////////////////////////////////////////////////////////////////////
void loop()
{

  // Serial.println ("--- LOOP BEGIN ---");

  // If a firmware update is in progress do not do anything else
  if (updating)
  {
    return;
  }

  // Measurement loop
  if ((millis() - measurements_loop_start) >= measurements_loop_duration)
  {

    // New timestamp for the loop start time
    measurements_loop_start = millis();

    // Read sensors
    Read_Sensor();

    if (PM25_value >= 0) // REVISAR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    {
      // Evaluate PM25 value
      Evaluate_PM25_Value();

      // Update display with new values
#if TDisplay
      Update_Display();
#endif

// Update bluetooth app with new values
#if BLUETOOTH
      Write_Bluetooth();
#endif

      // Accumulates samples
      PM25_accumulated += PM25_value;
      PM25_samples++;
    }
  }

  // MQTT loop
  if ((millis() - MQTT_loop_start) >= MQTT_loop_duration)
  {

    // New timestamp for the loop start time
    MQTT_loop_start = millis();

    // Message the MQTT broker in the cloud app to send the measured values
    if ((!err_wifi) && (PM25_samples > 0))
    {
      Send_Message_Cloud_App_MQTT();
    }

    // Reset samples after sending them to the MQTT server
    PM25_accumulated = 0.0;
    PM25_samples = 0.0;
  }

  // Errors loop
  if ((millis() - errors_loop_start) >= errors_loop_duration)
  {

    // New timestamp for the loop start time
    errors_loop_start = millis();

    // Try to recover error conditions
    if (err_sensor)
    {
      Serial.println("--- err_sensor");
      // Setup_Sensor();  // Init pm25 sensors
    }

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("--- err_wifi");
      err_wifi = true;
      WiFi.reconnect();
    }
    else
    {
      err_wifi = false;
    }

    // Reconnect MQTT if needed
    if ((!MQTT_client.connected()) && (!err_wifi))
    {
      Serial.println("--- err_mqtt");
      err_MQTT = true;
    }

    // Reconnect MQTT if needed
    if ((err_MQTT) && (!err_wifi))
    {
      Serial.println("--- MQTT reconnect");
      // Attempt to connect to MQTT broker
      MQTT_Reconnect();
      Init_MQTT();
    }
  }

  // From here, all other tasks performed outside of measurements, MQTT and error loops

  // if not there are not connectivity errors, receive MQTT messages
  if ((!err_MQTT) && (!err_wifi))
  {
    MQTT_client.loop();
  }

  // Process wifi server requests
  Check_WiFi_Server();

// Process bluetooth events
#if BLUETOOTH
  gadgetBle.handleEvents();
#endif

#if TDisplay
  // Process buttons events
  button_top.loop();
  button_bottom.loop();
#endif

  // Serial.println("--- END LOOP");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FUNCTIONS
///////////////////////////////////////////////////////////////////////////////////////////////////

void WiFiEvent(WiFiEvent_t event)
{
  Serial.printf("[WiFi-event] event: %d - ", event);

  switch (event)
  {
  case SYSTEM_EVENT_WIFI_READY:
    Serial.println("WiFi interface ready");
    break;
  case SYSTEM_EVENT_SCAN_DONE:
    Serial.println("Completed scan for access points");
    break;
  case SYSTEM_EVENT_STA_START:
    Serial.println("WiFi client started");
    break;
  case SYSTEM_EVENT_STA_STOP:
    Serial.println("WiFi clients stopped");
    break;
  case SYSTEM_EVENT_STA_CONNECTED:
    Serial.println("Connected to access point");
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    Serial.println("Disconnected from WiFi access point");
    break;
  case SYSTEM_EVENT_STA_AUTHMODE_CHANGE:
    Serial.println("Authentication mode of access point has changed");
    break;
  case SYSTEM_EVENT_STA_GOT_IP:
    Serial.print("Obtained IP address: ");
    Serial.println(WiFi.localIP());
    break;
  case SYSTEM_EVENT_STA_LOST_IP:
    Serial.println("Lost IP address and IP address is reset to 0");
    break;
  case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
    Serial.println("WiFi Protected Setup (WPS): succeeded in enrollee mode");
    break;
  case SYSTEM_EVENT_STA_WPS_ER_FAILED:
    Serial.println("WiFi Protected Setup (WPS): failed in enrollee mode");
    break;
  case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
    Serial.println("WiFi Protected Setup (WPS): timeout in enrollee mode");
    break;
  case SYSTEM_EVENT_STA_WPS_ER_PIN:
    Serial.println("WiFi Protected Setup (WPS): pin code in enrollee mode");
    break;
  case SYSTEM_EVENT_AP_START:
    Serial.println("WiFi access point started");
    break;
  case SYSTEM_EVENT_AP_STOP:
    Serial.println("WiFi access point  stopped");
    break;
  case SYSTEM_EVENT_AP_STACONNECTED:
    Serial.println("Client connected");
    break;
  case SYSTEM_EVENT_AP_STADISCONNECTED:
    Serial.println("Client disconnected");
    break;
  case SYSTEM_EVENT_AP_STAIPASSIGNED:
    Serial.println("Assigned IP address to client");
    break;
  case SYSTEM_EVENT_AP_PROBEREQRECVED:
    Serial.println("Received probe request");
    break;
  case SYSTEM_EVENT_GOT_IP6:
    Serial.println("IPv6 is preferred");
    break;
  case SYSTEM_EVENT_ETH_START:
    Serial.println("Ethernet started");
    break;
  case SYSTEM_EVENT_ETH_STOP:
    Serial.println("Ethernet stopped");
    break;
  case SYSTEM_EVENT_ETH_CONNECTED:
    Serial.println("Ethernet connected");
    break;
  case SYSTEM_EVENT_ETH_DISCONNECTED:
    Serial.println("Ethernet disconnected");
    break;
  case SYSTEM_EVENT_ETH_GOT_IP:
    Serial.println("Obtained IP address");
    break;
  default:
    break;
  }
}

#if TDisplay
void Interrupt_Restart(Button2 &btn)
{ // Restarts the device if any button is pressed while calibrating or in captive portal
  Serial.println("Any button click");
  if ((InCaptivePortal) || (Calibrating))
  {
    ESP.restart();
  }
}
#endif

void Connect_WiFi()
{ // Connect to WiFi

  WiFi.disconnect(true); // disconnect form wifi to set new wifi connection
  WiFi.mode(WIFI_STA);   // init wifi mode
  WiFi.onEvent(WiFiEvent);

  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf); // Get WiFi configuration
  Serial.print("Attempting to connect to WiFi network: ");
  Serial.println(String(reinterpret_cast<const char *>(conf.sta.ssid))); // WiFi.SSID() is not filled up until the connection is established

  // If there are not wifi user and wifi password defined, proceed to traight forward configuration
  if ((strlen(eepromConfig.wifi_user) == 0) && (strlen(eepromConfig.wifi_password) == 0))
  {
    Serial.println("Attempting to authenticate...");
  }
  else
  { // set up wpa2 enterprise
    Serial.println("Attempting to authenticate using WPA2 Enterprise...");
    Serial.print("User: ");
    Serial.println(eepromConfig.wifi_user);
    Serial.print("Password: ");
    Serial.println(eepromConfig.wifi_password);
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)eepromConfig.wifi_user, strlen(eepromConfig.wifi_user));         // provide identity
    esp_wifi_sta_wpa2_ent_set_username((uint8_t *)eepromConfig.wifi_user, strlen(eepromConfig.wifi_user));         // provide username --> identity and username is same
    esp_wifi_sta_wpa2_ent_set_password((uint8_t *)eepromConfig.wifi_password, strlen(eepromConfig.wifi_password)); // provide password
    esp_wpa2_config_t config = WPA2_CONFIG_INIT_DEFAULT();                                                         // set config settings to default
    esp_wifi_sta_wpa2_ent_enable(&config);                                                                         // set config settings to enable function
  }

  // Connect to wifi
  WiFi.begin();

  // Timestamp for connection timeout
  int wifi_timeout_start = millis();

  // Wait for warming time while blinking blue led
  while ((WiFi.status() != WL_CONNECTED) && ((millis() - wifi_timeout_start) < WIFI_CONNECT_TIMEOUT))
  {
    delay(500); // wait 0.5 seconds for connection
    Serial.println(".");
  }

  // Status
  if (WiFi.status() != WL_CONNECTED)
  {
    err_wifi = true;
    Serial.println("WiFi not connected");
  }
  else
  {
    err_wifi = false;
    Serial.println("WiFi connected");
    // start the web server on port 80
    wifi_server.begin();
  }
  Print_WiFi_Status();
}

void Print_WiFi_Status()
{ // Print wifi status on serial monitor

  // Get current status
  //  WL_CONNECTED: assigned when connected to a WiFi network;
  //  WL_NO_SHIELD: assigned when no WiFi shield is present;
  //  WL_IDLE_STATUS: it is a temporary status assigned when WiFi.begin() is called and remains active until the number of attempts expires (resulting in WL_CONNECT_FAILED) or a connection is established (resulting in WL_CONNECTED);
  //  WL_NO_SSID_AVAIL: assigned when no SSID are available;
  //  WL_SCAN_COMPLETED: assigned when the scan networks is completed;
  //  WL_CONNECT_FAILED: assigned when the connection fails for all the attempts;
  //  WL_CONNECTION_LOST: assigned when the connection is lost;
  //  WL_DISCONNECTED: assigned when disconnected from a network;

  Serial.print("wifi_status: ");
  switch (WiFi.status())
  {
  case WL_CONNECTED:
    Serial.println("WiFi connected");
    break;
  case WL_NO_SHIELD:
    Serial.println("No WiFi HW detected");
    break;
  case WL_IDLE_STATUS:
    Serial.println("Attempting...");
    break;
  case WL_NO_SSID_AVAIL:
    Serial.println("No SSID available");
    break;
  case WL_SCAN_COMPLETED:
    Serial.println("Networks scan completed");
    break;
  case WL_CONNECT_FAILED:
    Serial.println("Connect failed");
    break;
  case WL_CONNECTION_LOST:
    Serial.println("Connection lost");
    break;
  case WL_DISCONNECTED:
    Serial.println("Disconnected");
    break;
  default:
    Serial.println("Unknown status");
    break;
  }

  // Print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // Print your WiFi shield's IP address:
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Print your WiFi shield's MAC address:
  Serial.print("MAC Adress: ");
  Serial.println(WiFi.macAddress());

  // Print the received signal strength:
  Serial.print("Signal strength (RSSI):");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

void Check_WiFi_Server()
{                                              // Wifi server
  WiFiClient client = wifi_server.available(); // listen for incoming clients
  if (client)
  {                               // if you get a client,
    Serial.println("new client"); // print a message out the serial port
    String currentLine = "";      // make a String to hold incoming data from the client
    while (client.connected())
    { // loop while the client's connected
      if (client.available())
      {                         // if there's bytes to read from the client,
        char c = client.read(); // read a byte, then
        Serial.write(c);        // print it out the serial monitor
        if (c == '\n')
        { // if the byte is a newline character

          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0)
          {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println();
            // Print current info
            client.print("Medidor AireCiudadano");
            client.println("<br>");
            client.print("SW version: ");
            client.print(sw_version);
            client.println("<br>");
            client.println("------");
            client.println("<br>");
            client.print("Anaire Device ID: ");
            client.print(anaire_device_id);
            client.println("<br>");
            client.print("Anaire Device name: ");
            client.print(eepromConfig.anaire_device_name);
            client.println("<br>");
            client.print("SSID: ");
            client.print(String(WiFi.SSID()));
            client.println("<br>");
            client.print("IP Adress: ");
            client.print(WiFi.localIP());
            client.println("<br>");
            client.print("MAC Adress: ");
            client.print(WiFi.macAddress());
            client.println("<br>");
            client.print("RSSI: ");
            client.print(WiFi.RSSI());
            client.println("<br>");
            client.println("------");
            client.println("<br>");
            client.print("PM25_warning_threshold: ");
            client.print(eepromConfig.PM25_warning_threshold);
            client.println("<br>");
            client.print("PM25_alarm_threshold: ");
            client.print(eepromConfig.PM25_alarm_threshold);
            client.println("<br>");
            client.print("MQTT Server: ");
            client.print(eepromConfig.MQTT_server);
            client.println("<br>");
            client.print("MQTT Port: ");
            client.print(eepromConfig.MQTT_port);
            client.println("<br>");
            //            client.print("Alarm: ");
            //            client.print(eepromConfig.acoustic_alarm);
            //            client.println("<br>");
            client.println("------");
            client.println("<br>");
            client.print("PM2.5: ");
            client.print(PM25_value);
            client.println("<br>");
            client.print("Temperature: ");
            client.print(temp);
            client.println("<br>");
            client.print("Humidity: ");
            client.print(humi);
            client.println("<br>");
            client.print("PM2.5 STATUS: ");
            switch (pm25_device_status)
            {
            case pm25_ok:
              client.print("OK");
              break;
            case pm25_warning:
              client.print("WARNING");
              break;
            case pm25_alarm:
              client.print("ALARM");
              break;
            }
            client.println("<br>");
            client.println("------");
            client.println("<br>");

            // Captive portal:
            client.print("Click <a href=\"/3\">here</a> to launch captive portal to set up WiFi and MQTT endpoint.<br>");
            client.println("<br>");
            // Suspend:
            client.print("Click <a href=\"/4\">here</a> to suspend the device.<br>");
            client.println("<br>");
            // Restart:
            client.print("Click <a href=\"/5\">here</a> to restart the device.<br>");
            client.println("<br>");

            // The HTTP response ends with another blank line:
            client.println();

            // break out of the while loop:
            break;
          }
          else
          { // if you got a newline, then clear currentLine:
            currentLine = "";
          }
        }
        else if (c != '\r')
        {                   // if you got anything else but a carriage return character,
          currentLine += c; // add it to the end of the currentLine
        }

        // Check to see if the client request was "GET /1" to calibrate the sensor:

        // Check to see if the client request was "GET /3" to launch captive portal:
        if (currentLine.endsWith("GET /3"))
        {
          PortalFlag = true;
          Start_Captive_Portal();
        }

        // Check to see if the client request was "GET /4" to suspend the device:
        if (currentLine.endsWith("GET /4"))
        {
          Suspend_Device();
        }

        // Check to see if the client request was "GET /5" to restart the device:
        if (currentLine.endsWith("GET /5"))
        {
          ESP.restart();
        }
      }
    }

    // close the connection:
    client.stop();
    Serial.println("client disconnected");
  }
}

void Start_Captive_Portal()
{ // Run a captive portal to configure WiFi and MQTT

  InCaptivePortal = true;
  String wifiAP = "AnaireWiFi_" + anaire_device_id;
  Serial.println(wifiAP);

#if TDisplay
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_RED, TFT_WHITE);
  tft.setTextSize(1);
  tft.setFreeFont(FF90);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(wifiAP, tft.width() / 2, tft.height() / 2);
#endif

  wifi_server.stop();

  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  wifiManager.setDebugOutput(true);
  wifiManager.disconnect();
  WiFi.mode(WIFI_AP); // explicitly set mode, esp defaults to STA+AP

  // Captive portal parameters
  WiFiManagerParameter custom_wifi_html("<p>Set WPA2 Enterprise</p>"); // only custom html
  WiFiManagerParameter custom_wifi_user("User", "WPA2 Enterprise user", eepromConfig.wifi_user, 24);
  WiFiManagerParameter custom_wifi_password("Password", "WPA2 Enterprise Password", eepromConfig.wifi_password, 24);
  WiFiManagerParameter custom_mqtt_html("<p>Set MQTT server</p>"); // only custom html
  WiFiManagerParameter custom_mqtt_server("Server", "MQTT server", eepromConfig.MQTT_server, 24);
  char port[6];
  itoa(eepromConfig.MQTT_port, port, 10);
  WiFiManagerParameter custom_mqtt_port("Port", "MQTT port", port, 6);

  // wifiManager.setSaveParamsCallback(saveParamCallback);

  // Add parameters
  wifiManager.addParameter(&custom_wifi_html);
  wifiManager.addParameter(&custom_wifi_user);
  wifiManager.addParameter(&custom_wifi_password);
  wifiManager.addParameter(&custom_mqtt_html);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);

  // sets timeout in seconds until configuration portal gets turned off.
  // If not specified device will remain in configuration mode until
  // switched off via webserver or device is restarted.

  wifiManager.setConfigPortalTimeout(30);
  // wifiManager.setConfigPortalTimeout(3);   !!!TEST SEN5X

  // it starts an access point
  // and goes into a blocking loop awaiting configuration
  // wifiManager.resetSettings(); // reset previous configurations
  bool res = wifiManager.startConfigPortal(wifiAP.c_str());
  if (!res)
  {
    Serial.println("Not able to start captive portal");
  }
  else
  {
    // if you get here you have connected to the WiFi
    Serial.println("Captive portal operative");
  }

  // Save parameters to EEPROM only if any of them changed
  bool write_eeprom = false;

  if (eepromConfig.wifi_user != custom_wifi_user.getValue())
  {
    strncpy(eepromConfig.wifi_user, custom_wifi_user.getValue(), sizeof(eepromConfig.wifi_user));
    eepromConfig.wifi_user[sizeof(eepromConfig.wifi_user) - 1] = '\0';
    write_eeprom = true;
    Serial.print("WiFi user: ");
    Serial.println(eepromConfig.wifi_user);
  }
  if (eepromConfig.wifi_password != custom_wifi_password.getValue())
  {
    strncpy(eepromConfig.wifi_password, custom_wifi_password.getValue(), sizeof(eepromConfig.wifi_password));
    eepromConfig.wifi_password[sizeof(eepromConfig.wifi_password) - 1] = '\0';
    write_eeprom = true;
    Serial.print("WiFi password: ");
    Serial.println(eepromConfig.wifi_password);
  }
  if (eepromConfig.MQTT_server != custom_mqtt_server.getValue())
  {
    strncpy(eepromConfig.MQTT_server, custom_mqtt_server.getValue(), sizeof(eepromConfig.MQTT_server));
    eepromConfig.MQTT_server[sizeof(eepromConfig.MQTT_server) - 1] = '\0';
    write_eeprom = true;
    Serial.print("MQTT server: ");
    Serial.println(eepromConfig.MQTT_server);
  }

  if (eepromConfig.MQTT_port != atoi(custom_mqtt_port.getValue()))
  {
    eepromConfig.MQTT_port = atoi(custom_mqtt_port.getValue());
    write_eeprom = true;
    Serial.print("MQTT port: ");
    Serial.println(eepromConfig.MQTT_port);
  }

  if (write_eeprom)
  {
    Write_EEPROM();
    ESP.restart();
  }

  InCaptivePortal = false;
}

void Init_MQTT()
{ // MQTT Init function
  Serial.print("Attempting to connect to the MQTT broker ");
  Serial.print(eepromConfig.MQTT_server);
  Serial.print(":");
  Serial.println(eepromConfig.MQTT_port);

  // Attempt to connect to MQTT broker
  MQTT_client.setBufferSize(512); // to receive messages up to 512 bytes length (default is 256)
  MQTT_client.setServer(eepromConfig.MQTT_server, eepromConfig.MQTT_port);
  MQTT_client.setCallback(Receive_Message_Cloud_App_MQTT);
  MQTT_client.connect(anaire_device_id.c_str());

  if (!MQTT_client.connected())
  {
    err_MQTT = true;
    MQTT_Reconnect();
  }
  else
  {
    err_MQTT = false;
    lastReconnectAttempt = 0;
    // Once connected resubscribe
    MQTT_client.subscribe(MQTT_receive_topic.c_str());
    Serial.print("MQTT connected - Receive topic: ");
    Serial.println(MQTT_receive_topic);
  }
}

void MQTT_Reconnect()
{ // MQTT reconnect function
  // Try to reconnect only if it has been more than 5 sec since last attemp
  unsigned long now = millis();
  if (now - lastReconnectAttempt > 5000)
  {
    lastReconnectAttempt = now;
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (MQTT_client.connect(anaire_device_id.c_str()))
    {
      err_MQTT = false;
      Serial.println("MQTT connected");
      lastReconnectAttempt = 0;
      // Once connected resubscribe
      MQTT_client.subscribe(MQTT_receive_topic.c_str());
      Serial.print("MQTT connected - Receive topic: ");
      Serial.println(MQTT_receive_topic);
    }
    else
    {
      err_MQTT = true;
      Serial.print("failed, rc=");
      Serial.print(MQTT_client.state());
      Serial.println(" try again in 5 seconds");
    }
  }
}

void Send_Message_Cloud_App_MQTT()
{ // Send measurements to the cloud application by MQTT
  // Print info
  float pm25f;

  Serial.print("Sending MQTT message to the send topic: ");
  Serial.println(MQTT_send_topic);
  Serial.println(PM25_accumulated);
  Serial.println(PM25_samples);
  pm25f = PM25_accumulated / PM25_samples;
  pm25int = round(pm25f);
  Serial.println(pm25int);
  ReadHyT();
  sprintf(MQTT_message, "{id: %s,PM25: %d,humidity: %d,temperature: %d}", anaire_device_id.c_str(), pm25int, humi, temp);
  Serial.print(MQTT_message);
  Serial.println();

  // send message, the Print interface can be used to set the message contents
  MQTT_client.publish(MQTT_send_topic.c_str(), MQTT_message);
}

void Receive_Message_Cloud_App_MQTT(char *topic, byte *payload, unsigned int length)
{                               // callback function to receive configuration messages from the cloud application by MQTT
  boolean write_eeprom = false; // to track if writing the eeprom is required
  memcpy(received_payload, payload, length);
  Serial.print("Message arrived: ");
  Serial.println(received_payload);

  // Deserialize the JSON document
  DeserializationError error = deserializeJson(jsonBuffer, received_payload);

  // Test if parsing succeeds.
  if (error)
  {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.f_str());
    return;
  }

  // Update name
  if ((jsonBuffer["name"]) && (eepromConfig.anaire_device_name != jsonBuffer["name"]))
  {
    strncpy(eepromConfig.anaire_device_name, jsonBuffer["name"].as<const char *>(), sizeof(eepromConfig.anaire_device_name));
    eepromConfig.anaire_device_name[sizeof(eepromConfig.anaire_device_name) - 1] = '\0';
    Serial.print("Anaire device name: ");
    Serial.println(eepromConfig.anaire_device_name);
    write_eeprom = true;
  }

  // Update warning threshold
  if ((jsonBuffer["warning"]) && (eepromConfig.PM25_warning_threshold != (int)jsonBuffer["warning"]))
  {
    eepromConfig.PM25_warning_threshold = (int)jsonBuffer["warning"];
    Evaluate_PM25_Value();
    Serial.print("New warning threshold: ");
    Serial.println(eepromConfig.PM25_warning_threshold);
    write_eeprom = true;
  }

  // Update alarm threshold
  if ((jsonBuffer["caution"]) && (eepromConfig.PM25_alarm_threshold != (int)jsonBuffer["caution"]))
  {
    eepromConfig.PM25_alarm_threshold = (int)jsonBuffer["caution"];
    Evaluate_PM25_Value();
    Serial.print("New alarm threshold: ");
    Serial.println(eepromConfig.PM25_alarm_threshold);
    write_eeprom = true;
  }

  // Check MQTT server
  if ((jsonBuffer["MQTT_server"]) && (eepromConfig.MQTT_server != jsonBuffer["MQTT_server"]))
  {
    strncpy(eepromConfig.MQTT_server, jsonBuffer["MQTT_server"], sizeof(eepromConfig.MQTT_server));
    eepromConfig.MQTT_server[sizeof(eepromConfig.MQTT_server) - 1] = '\0';
    Serial.print("MQTT Server: ");
    Serial.println(eepromConfig.MQTT_server);
    write_eeprom = true;

    // Attempt to connect to MQTT broker
    if (!err_wifi)
    {
      Init_MQTT();
    }
  }

  // Check MQTT port
  if ((jsonBuffer["MQTT_port"]) && (eepromConfig.MQTT_port != int(jsonBuffer["MQTT_port"])))
  {
    eepromConfig.MQTT_port = int(jsonBuffer["MQTT_port"]);
    // strncpy(eepromConfig.MQTT_port, jsonBuffer["MQTT_port"], sizeof(eepromConfig.MQTT_port));
    // eepromConfig.MQTT_port[sizeof(eepromConfig.MQTT_port) - 1] = '\0';
    Serial.print("MQTT Port: ");
    Serial.println(eepromConfig.MQTT_port);
    write_eeprom = true;

    // Attempt to connect to MQTT broker
    if (!err_wifi)
    {
      Init_MQTT();
    }
  }

  // print info
  Serial.println("MQTT update - message processed");
  Print_Config();

  // If factory reset has been enabled, just do it
  if ((jsonBuffer["factory_reset"]) && (jsonBuffer["factory_reset"] == "ON"))
  {
    Wipe_EEPROM(); // Wipe EEPROM
    ESP.restart();
  }

  // If reboot, just do it, without cleaning the EEPROM
  if ((jsonBuffer["reboot"]) && (jsonBuffer["reboot"] == "ON"))
  {
    ESP.restart();
  }

  // save the new values if the flag was set
  if (write_eeprom)
  {
    Write_EEPROM();
  }

  // if update flag has been enabled, update to latest bin
  // It has to be the last option, to allow to save EEPROM if required
  if (((jsonBuffer["update"]) && (jsonBuffer["update"] == "ON")))
  {
    // boolean result = EEPROM.wipe();
    // if (result) {
    //   Serial.println("All EEPROM data wiped");
    // } else {
    //   Serial.println("EEPROM data could not be wiped from flash store");
    // }

    // Update firmware to latest bin
    Serial.println("Update firmware to latest bin");
    Firmware_Update();
  }
}

void Setup_Sensor()
{ // Identify and initialize PM25, temperature and humidity sensor

  // Test PM2.5 SPS30

#if SPS30sen

  Serial.println("Test Sensirion SPS30 sensor");
  // Wire.begin(Sensor_SDA_pin, Sensor_SCL_pin);

  sps30.EnableDebugging(DEBUG);
  // Begin communication channel
  SP30_COMMS.begin();
  if (sps30.begin(&SP30_COMMS) == false)
  {
    Errorloop((char *)"Could not set I2C communication channel.", 0);
  }
  // check for SPS30 connection
  if (!sps30.probe())
    Errorloop((char *)"could not probe / connect with SPS30.", 0);
  else
  {
    Serial.println("Detected SPS30.");
    pm25_sensor = sps30_sensor;
    // SPS30flag = true;
    //  read device info
    GetDeviceInfo();
  }
  // start measurement
  if (sps30.start())
    Serial.println("Measurement started");
  else
    Errorloop((char *)"Could NOT start measurement", 0);
#endif

    //////////////////////////////////////////////////////////////////////////////////////////////
    // Test PM2.5 SEN5X

#if SEN5Xsen

  Serial.println("Test Sensirion SEN5X sensor");
  Wire.begin(Sensor_SDA_pin, Sensor_SCL_pin);
  delay(10);
  sen5x.begin(Wire);
  delay(10);

  uint16_t error;
  char errorMessage[256];
  error = sen5x.deviceReset();
  if (error)
  {
    Serial.print("Error trying to execute deviceReset(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
  }
  else
  {
    // Print SEN55 module information if i2c buffers are large enough
    Serial.println("SEN5X sensor found!");
    pm25_sensor = sen5x_sensor;
    // SEN5Xflag = true;
    printSerialNumber();
    printModuleVersions();

    // Start Measurement
    error = sen5x.startMeasurement();
    if (error)
    {
      Serial.print("Error trying to execute startMeasurement(): ");
      errorToString(error, errorMessage, 256);
      Serial.println(errorMessage);
      //ESP.restart();
    }
    else
      Serial.println("SEN5X measurement OK");
  }
#endif

  ///////////////////////////////////////////////////////////////////////////////////////////

  // PMS7003 PMSA003
#if PMSsen

  Serial.println("Test Plantower Sensor");
  Serial1.begin(PMS::BAUD_RATE, SERIAL_8N1, PMS_TX, PMS_RX);
  delay(1000);

  while (Serial1.available())
  {
    Serial1.read();
  }
  pms.requestRead();
  if (pms.readUntil(data))
  {
    Serial.println("Plantower sensor found!");
    pm25_sensor = pms_sensor;
    // PMSflag = true;
  }
  else
  {
    Serial.println("Could not find Plantower sensor!");
  }
#endif

#if SHT31sen

  Serial.print("SHT31 test: ");
  if (!sht31.begin(0x44))
  { // Set to 0x45 for alternate i2c addr
    Serial.println("none");
  }
  else
  {
    Serial.println("OK");
    SHT31flag = true;
  }

  Serial.print("Heater Enabled State: ");
  if (sht31.isHeaterEnabled())
    Serial.println("ENABLED");
  else
    Serial.println("DISABLED");
#endif

#if AM2320sen

  Serial.print("AM2320 test: ");
  am2320.begin();
  humidity = am2320.readHumidity();
  temperature = am2320.readTemperature();
  if (!isnan(humidity))
  {
    Serial.println("OK");
    AM2320flag = true;
  }
  else
    Serial.println("none");
#endif

}

void Read_Sensor()
{ // Read PM25, temperature and humidity values

#if SPS30sen
  uint8_t ret, error_cnt = 0;
  struct sps_values val;
  // loop to get data
  do
  {
    ret = sps30.GetValues(&val);
    // data might not have been ready
    if (ret == ERR_DATALENGTH)
    {
      if (error_cnt++ > 3)
      {
        ErrtoMess((char *)"Error during reading values: ", ret);
        // return(false);
      }
      delay(1000);
    }
    // if other error
    else if (ret != ERR_OK)
    {
      ErrtoMess((char *)"Error during reading values: ", ret);
      // return(false);
    }
  } while (ret != ERR_OK);

  PM25_value = val.MassPM2;

  if (!err_sensor)
  {
    // Provide the sensor values for Tools -> Serial Monitor or Serial Plotter
    Serial.print("SPS30 PM2.5: ");
    Serial.print(PM25_value);
    Serial.print(" ug/m3   ");
  }
#endif

#if SEN5Xsen

  uint16_t error;
  char errorMessage[256];

  error = sen5x.readMeasuredValues(
      massConcentrationPm1p0, massConcentrationPm2p5, massConcentrationPm4p0,
      massConcentrationPm10p0, ambientHumidity, ambientTemperature, vocIndex,
      noxIndex);

  if (error)
  {
    Serial.print("Error trying to execute readMeasuredValues(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    delay(10);
    //        Wire.begin(18, 19);;
    //        delay(10);
    //        sen5x.begin(Wire);
    //        delay(10);
    //        Wire.begin(21, 22);;
    //        delay(10);
    //        sen5x.begin(Wire);
    Setup_Sensor();
    Serial.println("Reinit I2C");
    delay(10);
  }
  else
  {
    PM25_value = massConcentrationPm2p5;
    Serial.print("SEN5X PM2.5: ");
    Serial.print(PM25_value);
    Serial.print(" ug/m3   ");
    Serial.print(" Humi % = ");
    Serial.print(ambientHumidity);
    humi = round(ambientHumidity);
    Serial.print("   Temp *C = ");
    Serial.print(ambientTemperature);
    temp = round(temperature);
    Serial.print("   VocIndex:");
    if (isnan(vocIndex))
      Serial.print(" n/a");
    else
      Serial.print(vocIndex);
    Serial.print("   NoxIndex:");
    if (isnan(noxIndex))
      Serial.println(" n/a");
    else
      Serial.println(noxIndex);
  }
#endif

#if PMSsen
  while (Serial1.available())
  {
    Serial1.read();
  }
  pms.requestRead();
  if (pms.readUntil(data))
  {
    PM25_value = data.PM_AE_UG_2_5;
    Serial.print("PMS PM2.5: ");
    Serial.print(PM25_value);
    Serial.print(" ug/m3   ");
  }
  else
  {
    Serial.println("No data by Plantower sensor!");
  }
#endif
}

/**
 * @brief : read and display device info
 */

#if SPS30sen

void GetDeviceInfo()
{
  char buf[32];
  uint8_t ret;
  SPS30_version v;
  // try to read serial number
  ret = sps30.GetSerialNumber(buf, 32);
  if (ret == ERR_OK)
  {
    Serial.print("Serial number : ");
    if (strlen(buf) > 0)
      Serial.println(buf);
    else
      Serial.println("not available");
  }
  else
    ErrtoMess((char *)"could not get serial number. ", ret);
  // try to get product name
  ret = sps30.GetProductName(buf, 32);
  if (ret == ERR_OK)
  {
    Serial.print("Product name  : ");
    if (strlen(buf) > 0)
      Serial.println(buf);
    else
      Serial.println("not available");
  }
  else
    ErrtoMess((char *)"could not get product name. ", ret);
  // try to get version info
  ret = sps30.GetVersion(&v);
  if (ret != ERR_OK)
  {
    Serial.println("Can not read version info.");
    return;
  }
  Serial.print("Firmware level: ");
  Serial.print(v.major);
  Serial.print(".");
  Serial.println(v.minor);

  Serial.print("Library level : ");
  Serial.print(v.DRV_major);
  Serial.print(".");
  Serial.println(v.DRV_minor);
}

void Errorloop(char *mess, uint8_t r)
{
  if (r)
    ErrtoMess(mess, r);
  else
    Serial.println(mess);
  Serial.println("No SPS30 connected");
}

/**
 *  @brief : display error message
 *  @param mess : message to display
 *  @param r : error code
 *
 */
void ErrtoMess(char *mess, uint8_t r)
{
  char buf[80];
  Serial.print(mess);
  sps30.GetErrDescription(r, buf, 80);
  Serial.println(buf);
}

#endif

#if SEN5Xsen

void printModuleVersions()
{
  uint16_t error;
  char errorMessage[256];

  unsigned char productName[32];
  uint8_t productNameSize = 32;

  error = sen5x.getProductName(productName, productNameSize);

  if (error)
  {
    Serial.print("Error trying to execute getProductName(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
  }
  else
  {
    Serial.print("ProductName:");
    Serial.println((char *)productName);
  }

  bool firmwareDebug;
  uint8_t firmwareMajor;
  uint8_t firmwareMinor;
  uint8_t hardwareMajor;
  uint8_t hardwareMinor;
  uint8_t protocolMajor;
  uint8_t protocolMinor;

  error = sen5x.getVersion(firmwareMajor, firmwareMinor, firmwareDebug,
                           hardwareMajor, hardwareMinor, protocolMajor,
                           protocolMinor);
  if (error)
  {
    Serial.print("Error trying to execute getVersion(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
  }
  else
  {
    Serial.print("Firmware: ");
    Serial.print(firmwareMajor);
    Serial.print(".");
    Serial.print(firmwareMinor);
    Serial.print(", ");

    Serial.print("Hardware: ");
    Serial.print(hardwareMajor);
    Serial.print(".");
    Serial.println(hardwareMinor);
  }
}

void printSerialNumber()
{
  uint16_t error;
  char errorMessage[256];
  unsigned char serialNumber[32];
  uint8_t serialNumberSize = 32;

  error = sen5x.getSerialNumber(serialNumber, serialNumberSize);
  if (error)
  {
    Serial.print("Error trying to execute getSerialNumber(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
  }
  else
  {
    Serial.print("SerialNumber:");
    Serial.println((char *)serialNumber);
  }
}

#endif

void Evaluate_PM25_Value()
{ // Evaluate measured PM25 value against warning and alarm thresholds

  // Status: ok
  if (PM25_value < eepromConfig.PM25_warning_threshold)
  {
    pm25_device_status = pm25_ok; // Update PM25 status
  }

  // Status: warning
  else if ((PM25_value >= eepromConfig.PM25_warning_threshold) && (PM25_value < eepromConfig.PM25_alarm_threshold))
  {
    pm25_device_status = pm25_warning; // update PM25 status
  }

  // Status: alarm
  else
  {
    pm25_device_status = pm25_alarm; // update PM25 status
  }

  // Print info on serial monitor
  switch (pm25_device_status)
  {
  case pm25_ok:
    Serial.println("STATUS: PM2.5 OK");
    break;
  case pm25_warning:
    Serial.println("STATUS: PM2.5 WARNING");
    break;
  case pm25_alarm:
    Serial.println("STATUS: PM2.5 ALARM");
    break;
  }
}

///////////////////////////////////////////////////////////////////////////////
void ReadHyT()
{
  /////////  SHT31
#if SHT31sen

  temperature = 0.0;
  humidity = 0.0;
  humidity = sht31.readHumidity();
  temperature = sht31.readTemperature();

  if (!isnan(humidity))
  { // check if 'is not a number'
    failh = 0;
    Serial.print("SHT31 Humi % = ");
    Serial.print(humidity);
    humi = round(humidity);
  }
  else
  {
    Serial.println("Failed to read humidity SHT31");
    if (failh == 5)
    {
      failh = 0;
      sht31.begin(0x44);
    }
    else
      failh = failh + 1;
  }

  if (!isnan(temperature))
  { // check if 'is not a number'
    Serial.print("   Temp *C = ");
    Serial.println(temperature);
    temp = round(temperature);
  }
  else
    Serial.println("Failed to read temperature SHT31");
#endif

    // AM2320//
#if AM2320sen

  temperature = 0.0;
  humidity = 0.0;
  humidity = am2320.readHumidity();
  temperature = am2320.readTemperature();

  if (!isnan(humidity))
  {
    failh = 0;
    Serial.print("AM2320 Humi % = ");
    Serial.print(humidity);
    humi = round(humidity);
  }
  else
  {
    Serial.println("Failed to read humidity AM2320");
    if (failh == 5)
    {
      failh = 0;
      am2320.begin();
    }
    else
      failh = failh + 1;
  }

  if (!isnan(temperature))
  {
    Serial.print("   Temp *C = ");
    Serial.println(temperature);
    temp = round(temperature);
  }
  else
    Serial.println("Failed to read temperature AM2320");
#endif
}

///////////////////////////////////////////////////////////////////////////////

void Print_Config()
{ // print Anaire device settings

  Serial.println("#######################################");
  Serial.print("device id: ");
  Serial.println(anaire_device_id);
  Serial.print("anaire device name: ");
  Serial.println(eepromConfig.anaire_device_name);
  Serial.print("SW version: ");
  Serial.println(sw_version);
  Serial.print("PM25 Warning threshold: ");
  Serial.println(eepromConfig.PM25_warning_threshold);
  Serial.print("PM25 Alarm threshold: ");
  Serial.println(eepromConfig.PM25_alarm_threshold);
  Serial.print("MQTT server: ");
  Serial.println(eepromConfig.MQTT_server);
  Serial.print("MQTT Port: ");
  Serial.println(eepromConfig.MQTT_port);
  //  Serial.print("Acoustic Alarm: ");
  //  Serial.println(eepromConfig.acoustic_alarm);
  //  Serial.print("Self Calibration: ");
  //  Serial.println(eepromConfig.self_calibration);
  //  Serial.print("Forced Recalibration Reference: ");
  //  Serial.println(eepromConfig.forced_recalibration_reference);
  Serial.print("Temperature Offset: ");
  Serial.println(eepromConfig.temperature_offset);
  Serial.print("Altitude Compensation: ");
  Serial.println(eepromConfig.altitude_compensation);
  Serial.print("WiFi user: ");
  Serial.println(eepromConfig.wifi_user);
  Serial.print("WiFi user's password: ");
  Serial.println(eepromConfig.wifi_password);
  Serial.println("#######################################");
}

void espDelay(int ms)
{ //! Long time delay, it is recommended to use shallow sleep, which can effectively reduce the current consumption
  esp_sleep_enable_timer_wakeup(ms * 1000);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_light_sleep_start();
}

#if TDisplay
void Button_Init()
{ // Manage TTGO T-Display board buttons

  // Buttons design:
  //   - Top button short click: info about the device
  //   - Top button long click: toggles acoustic alarm; enabled by default
  //   - Top button double click: sleep; click a button to wake up
  //   - Top button triple click: starts captive portal
  //   - Bottom button short click: buttons usage
  //   - Bottom button double click: restart device
  //   - Bottom button triple click: enables auto self calibration

  // Long clicks: keep pressing more than 1 second
  button_top.setLongClickTime(1000);
  button_bottom.setLongClickTime(1000);

  // If any button is pressed run the following function. Intended to interrupt calibration or captive portal and restart the device. Not yet implemented.
  button_top.setTapHandler(Interrupt_Restart);
  button_bottom.setTapHandler(Interrupt_Restart);

  // Top button short click: show info about the device
  button_top.setClickHandler([](Button2 &b)
                             {
    Serial.println("Top button short click");
    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setTextDatum(TL_DATUM); // top left
    tft.setTextSize(1);
    tft.setFreeFont(FF90);
    tft.drawString("ID " + anaire_device_id, 10, 5);
    tft.drawString("SW " + sw_version, 10, 21);
    tft.drawString("SSID " + String(WiFi.SSID()), 10, 37);
    tft.drawString("IP " + WiFi.localIP().toString(), 10, 53);
    tft.drawString("MAC " + String(WiFi.macAddress()), 10, 69);
    tft.drawString("RSSI " + String(WiFi.RSSI()), 10, 85);

    delay(5000); // keep the info in the display for 5s
    Update_Display(); });

  // Top button long click: toggle acoustic alarm
  button_top.setLongClickDetectedHandler([](Button2 &b)
                                         {
    Serial.println("Top button long click");
    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setTextSize(1);
    tft.setFreeFont(FF90);
    tft.setTextDatum(MC_DATUM);

    Write_EEPROM();
    delay(5000); // keep the info in the display for 5s
    Update_Display(); });

  // Top button double click: sleep
  button_top.setDoubleClickHandler([](Button2 &b)
                                   {
    Serial.println("Top button double click");
    PortalFlag = true;
    Start_Captive_Portal(); });

  // Top button triple click: launch captive portal to configure WiFi and MQTT sleep
  button_top.setTripleClickHandler([](Button2 &b)
                                   {
    Serial.println("Top button triple click");
    Suspend_Device(); });

  // Bottom button short click: show buttons info
  button_bottom.setClickHandler([](Button2 &b)
                                {
    Serial.println("Bottom button short click");
    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextDatum(TL_DATUM); // top left
    tft.setTextSize(1);
    tft.setFreeFont(FF90);
    tft.drawString("Arriba Corto: Status", 10, 5);
    tft.drawString("  Largo: Alarma", 10, 21);
    tft.drawString("  Doble: Dormir", 10, 37);
    tft.drawString("  Triple: Config Portal", 10, 53);
    tft.drawString("Abajo Corto: Info", 10, 69);
    tft.drawString("  Largo: Calibrar", 10, 85);
    tft.drawString("  Doble: Reiniciar", 10, 101);
    tft.drawString("  Triple: Autocalibración", 10, 117);
    delay(5000);
    Update_Display(); });

  // Bottom button long click: deactivate self calibration and perform sensor forced recalibration
  button_bottom.setLongClickDetectedHandler([](Button2 &b)
                                            {
                                              Serial.println("Bottom button long click");
//                                              eepromConfig.self_calibration = false;
                                              tft.fillScreen(TFT_WHITE);
                                              tft.setTextColor(TFT_RED, TFT_WHITE);
                                              tft.setTextSize(1);
                                              tft.setFreeFont(FF90);
                                              tft.setTextDatum(MC_DATUM);
//                                              tft.drawString("CALIBRACION: FORZADA", tft.width() / 2, tft.height() / 2);
                                              delay(1000);
//                                              Set_AutoSelfCalibration();
//                                              Do_Calibrate_Sensor();
                                              Write_EEPROM(); });

  // Bottom button double click: restart
  button_bottom.setDoubleClickHandler([](Button2 &b)
                                      {
    Serial.println("Bottom button double click");
    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setTextSize(1);
    tft.setFreeFont(FF90);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("REINICIO", tft.width()/2, tft.height()/2);
    delay(1000);
    ESP.restart(); });

  // Bottom button triple click: activate sensor self calibration
  button_bottom.setTripleClickHandler([](Button2 &b)
                                      {
//    Serial.println("Bottom button triple click");
//    eepromConfig.self_calibration = true;
    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setTextSize(1);
    tft.setFreeFont(FF90);
    tft.setTextDatum(MC_DATUM);
//    tft.drawString("CALIBRACION: AUTO", tft.width()/2, tft.height()/2);
    delay(1000);
//    Set_AutoSelfCalibration(); 
    Write_EEPROM();
    Update_Display(); });
}

void Display_Init()
{ // TTGO T-Display init
  tft.init();
  tft.setRotation(1);
}

void Display_Splash_Screen()
{ // Display Anaire splash screen
  tft.pushImage(0, 0, 240, 135, anaire_ttgo_splash);
}

void Update_Display()
{ // Update display

  tft.setTextDatum(TL_DATUM); // top left

  // Set screen and text colours based on PM25 value
  if (pm25_device_status == pm25_ok)
  {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    displayWifi(TFT_GREEN, TFT_BLACK, (WiFi.status() == WL_CONNECTED));
    //    displayBuzzer(TFT_GREEN, eepromConfig.acoustic_alarm);
    displayBatteryLevel(TFT_GREEN);
  }

  else if (pm25_device_status == pm25_warning)
  {
    tft.fillScreen(TFT_YELLOW);
    tft.setTextColor(TFT_RED, TFT_YELLOW);
    delay(50);
    displayWifi(TFT_RED, TFT_YELLOW, (WiFi.status() == WL_CONNECTED));
    //    displayBuzzer(TFT_RED, eepromConfig.acoustic_alarm);
    displayBatteryLevel(TFT_RED);
  }

  else if (pm25_device_status == pm25_alarm)
  {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);

    displayWifi(TFT_WHITE, TFT_RED, (WiFi.status() == WL_CONNECTED));
    //    displayBuzzer(TFT_WHITE, eepromConfig.acoustic_alarm);
    displayBatteryLevel(TFT_WHITE);
  }

  // Draw PM25 number
  tft.setTextSize(1);
  tft.setFreeFont(FF95);
  tft.drawString(String(round(PM25_value), 0), 60, 30);

  // Draw PM25 units
  tft.setTextSize(1);
  tft.setFreeFont(FF90);
  tft.drawString("PPM", 200, 115);

  // Draw temperature
  tft.drawString(String(temperature, 1) + "C", 80, 115);

  // Draw humidity
  tft.drawString(String(humidity, 1) + "%", 140, 115);

  // Draw bluetooth device id
#if BLUETOOTH
  //  tft.setTextDatum(8); // bottom right
  tft.drawString(gadgetBle.getDeviceIdString(), 200, 115);
#endif
}

#endif

void Get_Anaire_DeviceId()
{ // Get TTGO T-Display info and fill up anaire_device_id with last 6 digits (in HEX) of WiFi mac address
  uint32_t chipId = 0;
  for (int i = 0; i < 17; i = i + 8)
  {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  anaire_device_id = String(chipId, HEX); // HEX format for backwards compatibility to Anaire devices based on NodeMCU board
  Serial.printf("ESP32 Chip model = %s Rev %d.\t", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("This chip has %d cores and %dMB Flash.\n", ESP.getChipCores(), ESP.getFlashChipSize() / (1024 * 1024));
  Serial.print("Anaire Device ID: ");
  Serial.println(anaire_device_id);
}

void Read_EEPROM()
{ // Read Anaire device persistent info
  if (preferences.getBytesLength("config") > 0)
  {
    boolean result = preferences.getBytes("config", &eepromConfig, sizeof(eepromConfig));
    if (result)
    {
      Serial.println("Config data read from flash");
    }
    else
    {
      Serial.println("Config data could not be read from flash");
    }
  }
  else
  {
    anaire_device_id.toCharArray(eepromConfig.anaire_device_name, sizeof(eepromConfig.anaire_device_name)); // Initialize anaire_device_name with anaire_device_id
    Serial.println("No EEPROM data - using default config values");
  }
}

void Write_EEPROM()
{ // Write Anaire device persistent info
  boolean result = preferences.putBytes("config", &eepromConfig, sizeof(eepromConfig));
  if (result)
  {
    Serial.println("Config data written to flash");
  }
  else
  {
    Serial.println("Config data could not be written to flash");
  }
}

void Wipe_EEPROM()
{ // Wipe Anaire device persistent info to reset config data
  boolean result = preferences.clear();
  if (result)
  {
    Serial.println("All EEPROM data wiped");
  }
  else
  {
    Serial.println("EEPROM data could not be wiped from flash store");
  }
}

void Firmware_Update()
{

  Serial.println("### FIRMWARE UPDATE ###");

  // For remote firmware update
  WiFiClientSecure UpdateClient;
  UpdateClient.setInsecure();

  // Reading data over SSL may be slow, use an adequate timeout
  UpdateClient.setTimeout(30); // timeout argument is defined in seconds for setTimeout
  Serial.println("ACTUALIZACION EN CURSO");

#if TDisplay
  // Update display
  tft.fillScreen(TFT_ORANGE);
  tft.setTextColor(TFT_BLACK, TFT_ORANGE);
  tft.setTextSize(1);
  tft.setFreeFont(FF90);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("ACTUALIZACION EN CURSO", tft.width() / 2, tft.height() / 2);
#endif

  // t_httpUpdate_return ret = httpUpdate.update(UpdateClient, "https://raw.githubusercontent.com/anaireorg/anaire-devices/main/src/anaire.PiCO2/anaire.PiCO2.ino.esp32.bin");
  t_httpUpdate_return ret = httpUpdate.update(UpdateClient, "https://raw.githubusercontent.com/anaireorg/anaire-devices/main/Anaire.PiCO2/anaire.PiCO2/anaire.PiCO2.ino.esp32.bin");

  switch (ret)
  {

  case HTTP_UPDATE_FAILED:
    Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());

#if TDisplay
    tft.fillScreen(TFT_ORANGE);
    tft.drawString("ACTUALIZACION FALLIDA", tft.width() / 2, tft.height() / 2);
#endif

    delay(1000);
    break;

  case HTTP_UPDATE_NO_UPDATES:
    Serial.println("HTTP_UPDATE_NO_UPDATES");
    break;

  case HTTP_UPDATE_OK:
    Serial.println("HTTP_UPDATE_OK");
#if TDisplay
    tft.fillScreen(TFT_ORANGE);
    tft.drawString("ACTUALIZACION COMPLETA", tft.width() / 2, tft.height() / 2);
#endif
    delay(1000);
    break;
  }
}

#if TDisplay
void displayBatteryLevel(int colour)
{ // Draw a battery showing the level of charge

  // Measure the battery voltage
  battery_voltage = ((float)analogRead(ADC_PIN) / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);

  //  Serial.print("battery voltage: ");
  //  Serial.println(battery_voltage);

  // If battery voltage is up 4.5 then external power supply is working and battery is charging
  if (battery_voltage > USB_Voltage)
  {
    tft.drawRect(5, 110, 30, 18, colour);
    // tft.fillRect(35, 113, 5, 9, colour);
    tft.fillRect(35, 114, 5, 9, colour);
    tft.fillRect(7, 112, 5, 14, colour);
    // delay(2500);
    tft.fillRect(14, 112, 5, 14, colour);
    // delay(2500);
    tft.fillRect(21, 112, 5, 14, colour);
    // delay(2500);
    tft.fillRect(28, 112, 5, 14, colour);
  }
  else if (battery_voltage >= Voltage_Threshold_1)
  {
    tft.drawRect(5, 110, 30, 18, colour);
    tft.fillRect(35, 113, 5, 9, colour);
    tft.fillRect(7, 112, 5, 14, colour);
    tft.fillRect(14, 112, 5, 14, colour);
    tft.fillRect(21, 112, 5, 14, colour);
    tft.fillRect(28, 112, 5, 14, colour);
  }
  else if (battery_voltage >= Voltage_Threshold_2)
  {
    tft.drawRect(5, 110, 30, 18, colour);
    tft.fillRect(35, 113, 5, 9, colour);
    tft.fillRect(7, 112, 5, 14, colour);
    tft.fillRect(14, 112, 5, 14, colour);
    tft.fillRect(21, 112, 5, 14, colour);
  }
  else if (battery_voltage >= Voltage_Threshold_3)
  {
    tft.drawRect(5, 110, 30, 18, colour);
    tft.fillRect(35, 113, 5, 9, colour);
    tft.fillRect(7, 112, 5, 14, colour);
    tft.fillRect(14, 112, 5, 14, colour);
  }
  else if (battery_voltage >= Voltage_Threshold_4)
  {
    tft.drawRect(5, 110, 30, 18, colour);
    tft.fillRect(35, 113, 5, 9, colour);
    tft.fillRect(7, 112, 5, 14, colour);
  }
  else
  {
    tft.drawRect(5, 110, 30, 18, colour);
    tft.fillRect(35, 113, 5, 9, colour);

    // Measurements are not trustable with this battery level
    Serial.println("Battery level too low");
  }
}

void displayWifi(int colour_1, int colour_2, boolean active)
{ // Draw WiFi icon
  tft.drawCircle(20, 30, 14, colour_1);
  tft.drawCircle(20, 30, 10, colour_1);
  tft.fillCircle(20, 30, 6, colour_1);
  tft.fillRect(6, 30, 30, 30, colour_2);
  // tft.fillRect(18, 30, 4, 8, colour_1);
  tft.fillRect(19, 30, 4, 8, colour_1);

  if (!active)
  { // draw an X over
    tft.drawLine(6, 16, 34, 46, colour_1);
    tft.drawLine(34, 16, 6, 46, colour_1);
  }
}

#endif

void Suspend_Device()
{
  Serial.println("Presione un boton para despertar");

#if TDisplay
  // int r = digitalRead(TFT_BL);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(" Presione un boton para despertar", tft.width() / 2, tft.height() / 2);
  espDelay(3000);
  // digitalWrite(TFT_BL, !r);
  tft.writecommand(TFT_DISPOFF);
  tft.writecommand(TFT_SLPIN);
#else
  espDelay(3000);
#endif

  // After using light sleep, you need to disable timer wake, because here use external IO port to wake up
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  // esp_sleep_enable_ext1_wakeup(GPIO_SEL_0, ESP_EXT1_WAKEUP_ALL_LOW);

  // set top button for wake up
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0); // Top button
  // esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Bottom button

  delay(200);
  esp_deep_sleep_start();
}

void print_reset_reason(RESET_REASON reason)
{
  switch (reason)
  {
  case 1:
    Serial.println("POWERON_RESET");
    ResetFlag = true;
    break; /**<1,  Vbat power on reset*/
  case 3:
    Serial.println("SW_RESET");
    break; /**<3,  Software reset digital core*/
  case 4:
    Serial.println("OWDT_RESET");
    break; /**<4,  Legacy watch dog reset digital core*/
  case 5:
    Serial.println("DEEPSLEEP_RESET");
    break; /**<5,  Deep Sleep reset digital core*/
  case 6:
    Serial.println("SDIO_RESET");
    break; /**<6,  Reset by SLC module, reset digital core*/
  case 7:
    Serial.println("TG0WDT_SYS_RESET");
    break; /**<7,  Timer Group0 Watch dog reset digital core*/
  case 8:
    Serial.println("TG1WDT_SYS_RESET");
    break; /**<8,  Timer Group1 Watch dog reset digital core*/
  case 9:
    Serial.println("RTCWDT_SYS_RESET");
    break; /**<9,  RTC Watch dog Reset digital core*/
  case 10:
    Serial.println("INTRUSION_RESET");
    break; /**<10, Instrusion tested to reset CPU*/
  case 11:
    Serial.println("TGWDT_CPU_RESET");
    break; /**<11, Time Group reset CPU*/
  case 12:
    Serial.println("SW_CPU_RESET");
    ResetFlag = false;
    break; /**<12, Software reset CPU*/
  case 13:
    Serial.println("RTCWDT_CPU_RESET");
    break; /**<13, RTC Watch dog Reset CPU*/
  case 14:
    Serial.println("EXT_CPU_RESET");
    break; /**<14, for APP CPU, reseted by PRO CPU*/
  case 15:
    Serial.println("RTCWDT_BROWN_OUT_RESET");
    break; /**<15, Reset when the vdd voltage is not stable*/
  case 16:
    Serial.println("RTCWDT_RTC_RESET");
    break; /**<16, RTC Watch dog reset digital core and rtc module*/
  default:
    Serial.println("NO_MEAN");
  }
}

#if BLUETOOTH
void Write_Bluetooth()
{ // Write measurements to bluetooth
  gadgetBle.writePM2p5(pm25int);
  gadgetBle.writeTemperature(temp);
  gadgetBle.writeHumidity(humi);
  Serial.println("Bluetooth frame: PM25, humidity and temperature");
  gadgetBle.commit();
}
#endif