#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

#ifdef WOKWI
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";
#else
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
#endif

const char *MQTT_BROKER = "b1c8f2cc5cd4416fb74b671a407dc0ff.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char *MQTT_CLIENT_ID = "esp32-agrisense-01";
const char *MQTT_USERNAME = "hivemq.webclient.1775680760112";
const char *MQTT_PASSWORD = "amy&f0VPG3c5<8,U>qAB";

const char *TOPIC_ENV = "iot/farm/env";
const char *TOPIC_CONTROL = "iot/farm/control";

#define DHT_PIN 4
#define DHT_TYPE DHT22
#define PUMP_PIN 5
#define BUZZER_PIN 27
#define SOIL_PIN 34

#define SOIL_DRY_THRESHOLD 30
#define SOIL_CRITICAL_THRESHOLD 20

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

unsigned long lastPublish = 0;
unsigned long lastMQTTRetry = 0;
const unsigned long PUBLISH_INTERVAL = 5000;
const unsigned long MQTT_RETRY_MS = 5000;

bool pumpState = false;

void beepAlert(int times, int onMs, int offMs)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1)
      delay(offMs);
  }
}

void setPump(bool on)
{
  pumpState = on;
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
  Serial.printf("[PUMP] %s\n", on ? "ON" : "OFF");
}

void handleControlMessage(const char *payload)
{
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    Serial.printf("[CONTROL] JSON parse error: %s\n", err.c_str());
    return;
  }

  if (doc.containsKey("pump"))
  {
    bool pumpOn = doc["pump"].as<bool>();
    setPump(pumpOn);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.printf("[MQTT] Message arrived on topic: %s\n", topic);

  char msg[length + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';
  Serial.printf("[MQTT] Payload: %s\n", msg);

  if (strcmp(topic, TOPIC_CONTROL) == 0)
  {
    handleControlMessage(msg);
  }
}

void setupWiFi()
{
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected! IP: ");
  Serial.println(WiFi.localIP());
}

void reconnectMQTT()
{
  Serial.print("[MQTT] Connecting...");
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD))
  {
    Serial.println(" Connected!");
    mqttClient.subscribe(TOPIC_CONTROL);
    Serial.printf("[MQTT] Subscribed to: %s\n", TOPIC_CONTROL);
  }
  else
  {
    Serial.printf(" Failed, error: %d\n", mqttClient.state());
  }
}

float readSoilMoisture()
{
  int raw = analogRead(SOIL_PIN);
  float moisture = map(constrain(raw, 0, 4095), 0, 4095, 0, 100);
  return moisture;
}

void publishSensorData(float temp, float hum, float soil)
{
  StaticJsonDocument<512> doc;
  doc["device"] = "AgriSense-01";
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  doc["soilMoisture"] = soil;
  doc["pumpStatus"] = pumpState;
  doc["timestamp"] = millis();

  char buffer[512];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_ENV, buffer, n);

  Serial.printf("Published: Temp=%.1f C, Hum=%.1f %%, Soil=%.1f %%\n", temp, hum, soil);
}

void readSensors(float &temp, float &hum, float &soil)
{
  temp = dht.readTemperature();
  hum = dht.readHumidity();
  soil = readSoilMoisture();

  if (isnan(temp))
  {
    temp = random(200, 350) / 10.0;
  }
  if (isnan(hum))
  {
    hum = random(400, 800) / 10.0;
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  AgriSense IoT - Smart Irrigation");
  Serial.println("  ESP32 + DHT22 + Soil + MQTT (TLS)");
  Serial.println("========================================");

  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(SOIL_PIN, INPUT);

  dht.begin();

  espClient.setInsecure();

  setupWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  Serial.println("Setup complete!");
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WiFi] Lost connection, reconnecting...");
    setupWiFi();
  }

  if (!mqttClient.connected())
  {
    if (millis() - lastMQTTRetry >= MQTT_RETRY_MS)
    {
      lastMQTTRetry = millis();
      reconnectMQTT();
    }
  }
  mqttClient.loop();

  unsigned long now = millis();

  if (now - lastPublish >= PUBLISH_INTERVAL)
  {
    lastPublish = now;

    float temperature, humidity, soilMoisture;
    readSensors(temperature, humidity, soilMoisture);

    Serial.println("\n--- AgriSense Sensor Data ---");
    Serial.print("Air Temperature: ");
    Serial.print(temperature);
    Serial.println(" C");
    Serial.print("Air Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");
    Serial.print("Soil Moisture: ");
    Serial.print(soilMoisture);
    Serial.println(" %");

    if (soilMoisture < SOIL_CRITICAL_THRESHOLD)
    {
      Serial.println("[ALERT] Soil moisture CRITICAL - activating buzzer!");
      beepAlert(5, 300, 200);
    }
    else if (soilMoisture < SOIL_DRY_THRESHOLD)
    {
      Serial.println("[WARN] Soil is dry!");
      beepAlert(2, 150, 150);
    }

    if (mqttClient.connected())
    {
      publishSensorData(temperature, humidity, soilMoisture);
      digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
    }
    else
    {
      Serial.println("[MQTT] Not connected, skipping...");
    }
  }
}
