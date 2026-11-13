// ECSE3038 - Tutorial 9 starter. Your job: TODOs 1-4.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include <PubSubClient.h>

#if __has_include("secretss.h")
#include "secretss.h"
#else
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6
#define X_API_KEY "default_key"
#endif

// Your server from Tutorial 6. http, not https, and no / at the end.
const char *API_URL = "http://iot-xackary.belgiumcentral.cloudapp.azure.com";

// Your name on the class broker: lowercase letters and digits only, no spaces.
const char *MQTT_NAME = "qwerty2";

// The class broker on Phillip's VM.
const char *BROKER_HOST = "135.232.203.144";
const uint16_t BROKER_PORT = 1883;

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long RETRY_EVERY_MS = 5000;    // wait between reconnect attempts
const int DHT_PIN = 15;                       // the sensor's data wire
const unsigned long SEND_EVERY_MS = 10000;    // one reading every 10 s
const int LED_PIN = 2;                        // the LED, through a 220 ohm resistor

DHTesp dht;
WiFiClient net;                               // the TCP connection to the broker
PubSubClient mqtt(net);                       // MQTT on top of it

String statusTopic;                           // ecse3038/<name>/status
String alarmMaxTopic;                         // ecse3038/<name>/alarm/max
String alarmStateTopic;                       // ecse3038/<name>/alarm/state

unsigned long lastSend = 0;
unsigned long lastAttempt = 0;
float tempLimit = NAN;                        // no limit until the broker gives one
float lastTemp = NAN;                         // no reading yet
String reportedLed = "";                      // what we last told the broker: "on", "off" or nothing yet

bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("No Wi-Fi after 15 s. Trying again in 5 s.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println(" connected");
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  return true;
}

// Requirement B1: Send reading with X-API-Key header
void sendReading(float temp) {
  char body[80];
  snprintf(body, sizeof(body), "{\"mac\": \"%s\", \"temp\": %.1f}",
           WiFi.macAddress().c_str(), temp);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", X_API_KEY); // Added X-API-Key header
  int status = http.POST(body);

  Serial.print("POST ");
  Serial.print(body);
  Serial.print(" -> ");
  Serial.println(status);
  if (status == 404) {
    Serial.println("Unknown MAC: register this board with POST /devices first.");
  } else if (status < 0) {
    Serial.println(http.errorToString(status));
  }
  http.end();
}

bool parseLimit(const String &message, float &limit) {
  if (message.length() == 0) {
    return false;
  }
  char *end;
  float value = strtof(message.c_str(), &end);
  if (*end != '\0') {
    return false;                           // something after the number
  }
  limit = value;
  return true;
}

void updateLed() {
  bool tooHot = false;
  if (!isnan(tempLimit) && !isnan(lastTemp) && lastTemp > tempLimit) {
    tooHot = true;
  }

  String led = "off";
  if (tooHot) {
    led = "on";
  }
  if (tooHot) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  Serial.print("Limit: ");
  Serial.print(tempLimit);
  Serial.print("  Temperature: ");
  Serial.print(lastTemp);
  Serial.print("  -> LED ");
  Serial.println(led);

  // Requirement B5: Publish on/off on alarm/state, retained, only when LED changes
  if (led != reportedLed && mqtt.connected()) {
    mqtt.publish(alarmStateTopic.c_str(), led.c_str(), true);
    reportedLed = led;
  }
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.print("Got ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  String t = topic;

  // Requirement B3: Subscribe to alarm/max and refuse non-numbers
  if (t == alarmMaxTopic){
    float newLimit;
    if (parseLimit(message, newLimit)) {
      tempLimit = newLimit;
      updateLed();
    } else {
      Serial.println("Not a number: limit unchanged.");
    }
  }
}

bool connectMqtt() {
  String clientId = "esp32-" + String(MQTT_NAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to broker ");
  Serial.print(BROKER_HOST);
  Serial.print(" as ");
  Serial.println(clientId);

  // Requirement B2: Connect with last will (offline, retained, status)
  bool ok = mqtt.connect(clientId.c_str(), statusTopic.c_str(), 1, true, "offline");   
  
  if (!ok) {
    Serial.print("Broker said no, state ");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println("Connected to broker");

  // Requirement B2: Publish online, retained
  mqtt.publish(statusTopic.c_str(), "online", true);
  
  // Requirement B3: Subscribe to alarm/max
  mqtt.subscribe(alarmMaxTopic.c_str());
  reportedLed = "";                           // a new connection: report the LED again
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Hello from the ESP32");
  dht.setup(DHT_PIN, DHTesp::DHT22);
  pinMode(LED_PIN, OUTPUT);

  String base = "ecse3038/" + String(MQTT_NAME);
  statusTopic = base + "/status";
  alarmMaxTopic = base + "/alarm/max";     // Corrected to alarm/max
  alarmStateTopic = base + "/alarm/state"; // Corrected to alarm/state

  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(15);

  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastAttempt >= RETRY_EVERY_MS) {
      lastAttempt = millis();
      connectWiFi();
    }
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
  } else if (millis() - lastAttempt >= RETRY_EVERY_MS) {
    lastAttempt = millis();
    connectMqtt();
  }

  if (millis() - lastSend < SEND_EVERY_MS) {
    return;
  }
  lastSend = millis();

  float temp = dht.getTemperature();
  if (isnan(temp)) {
    Serial.println("No reading from the sensor. Check the three wires.");
    return;
  }
  lastTemp = temp;
  sendReading(temp);
  updateLed();
}