#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// BH1750 — cảm biến ánh sáng (I2C)
#include <Wire.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_BH1750.h>

// InfluxDB — lưu trữ dữ liệu lịch sử
#include <InfluxDbClient.h>

#ifdef WOKWI
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";
#else
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
#endif

// ===== MQTT =====
const char *MQTT_BROKER = "9b17b35859014384af7ef09b30b38c31.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char *MQTT_CLIENT_ID = "esp32-agrisense-01";
const char *MQTT_USERNAME = "hivemq.webclient.1775805456143";
const char *MQTT_PASSWORD = "6y28haT$Gi1dpX@JH:L,";

const char *TOPIC_ENV = "iot/farm/env";
const char *TOPIC_CONTROL = "iot/farm/control";

// ===== GPIO =====
#define DHT_PIN 4
#define DHT_TYPE DHT22
#define PUMP_RELAY_PIN 26   // Relay bật/tắt máy bơm
#define BUZZER_PIN 27
#define SOIL_PIN 34        // ADC — Potentiometer mô phỏng độ ẩm đất

// I2C cho BH1750
#define I2C_SDA 21
#define I2C_SCL 22

// ===== NGƯỠNG CẢNH BÁO =====
#define SOIL_DRY_THRESHOLD 30          // Đất khô → bíp 2 lần + toast
#define SOIL_CRITICAL_THRESHOLD 20    // Nguy hiểm → bíp 5 lần
#define LIGHT_LOW_THRESHOLD 1000.0    // Thiếu sáng → cảnh báo (< 1000 lux)

// ===== CẢM BIẾN & CLIENT =====
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_BH1750 bh1750;
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// ===== INFLUXDB =====
#ifdef INFLUXDB_ENABLED
InfluxDBClient influxClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
Point sensorPoint("agrisense");
#endif

// ===== TRẠNG THÁI =====
unsigned long lastPublish = 0;
unsigned long lastMQTTRetry = 0;
const unsigned long PUBLISH_INTERVAL = 5000;
const unsigned long MQTT_RETRY_MS = 5000;

bool pumpRelayState = false;   // Trạng thái relay bơm (HIGH = bật)
bool lightSensorOK = false;    // BH1750 khởi tạo thành công

// ============================================================
// BEEP ALERT
// ============================================================
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

// ============================================================
// ĐIỀU KHIỂN RELAY BƠM
// ============================================================
void setPumpRelay(bool on)
{
  pumpRelayState = on;
  digitalWrite(PUMP_RELAY_PIN, on ? HIGH : LOW);
  Serial.printf("[RELAY] Pump %s\n", on ? "ON" : "OFF");
}

// ============================================================
// ĐỌC CẢM BIẾN ÁNH SÁNG BH1750
// ============================================================
float readLightLux()
{
  if (!lightSensorOK)
  {
    // BH1750 chưa khởi tạo → trả giá trị mặc định
    return -1.0f;
  }

  sensors_event_t event;
  if (bh1750.getEvent(&event))
  {
    // BH1750 trả lux = 0 khi nguồn yếu / lỗi → fallback
    if (event.light < 0)
      return -1.0f;
    return event.light;
  }
  return -1.0f;
}

// ============================================================
// ĐỌC ĐỘ ẨM ĐẤT (POTENTIOMETER / ADC)
// ============================================================
float readSoilMoisture()
{
  int raw = analogRead(SOIL_PIN);
  // map 0-4095 (ADC 12-bit) → 0-100%
  float moisture = map(constrain(raw, 0, 4095), 0, 4095, 0, 100);
  return moisture;
}

// ============================================================
// XỬ LÝ LỆNH ĐIỀU KHIỂN TỪ MQTT
// ============================================================
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
    setPumpRelay(pumpOn);
    Serial.printf("[CONTROL] Pump command: %s\n", pumpOn ? "ON" : "OFF");
  }
}

// ============================================================
// CALLBACK — MQTT nhận message
// ============================================================
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.printf("[MQTT] Topic: %s\n", topic);

  char msg[length + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';
  Serial.printf("[MQTT] Payload: %s\n", msg);

  if (strcmp(topic, TOPIC_CONTROL) == 0)
  {
    handleControlMessage(msg);
  }
}

// ============================================================
// KẾT NỐI WIFI
// ============================================================
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

// ============================================================
// KẾT NỐI MQTT
// ============================================================
void reconnectMQTT()
{
  Serial.print("[MQTT] Connecting...");
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD))
  {
    Serial.println(" Connected!");
    mqttClient.subscribe(TOPIC_CONTROL);
    Serial.printf("[MQTT] Subscribed: %s\n", TOPIC_CONTROL);
  }
  else
  {
    Serial.printf(" Failed, state: %d\n", mqttClient.state());
  }
}

// ============================================================
// GỬI DỮ LIỆU LÊN INFLUXDB
// ============================================================
void sendToInfluxDB(float temp, float hum, float soil, float lux, bool pumpOn)
{
#ifdef INFLUXDB_ENABLED
  sensorPoint.clearFields();
  sensorPoint.addField("temperature", temp);
  sensorPoint.addField("humidity", hum);
  sensorPoint.addField("soil_moisture", soil);
  sensorPoint.addField("light_lux", lux >= 0 ? lux : 0);
  sensorPoint.addField("pump_on", pumpOn ? 1 : 0);

  if (!influxClient.writePoint(sensorPoint))
  {
    Serial.printf("[INFLUX] Write failed: %s\n", influxClient.getLastErrorMessage().c_str());
  }
  else
  {
    Serial.println("[INFLUX] Data written");
  }
#endif
}

// ============================================================
// PUBLISH DỮ LIỆU LÊN MQTT
// ============================================================
void publishSensorData(float temp, float hum, float soil, float lux)
{
  StaticJsonDocument<512> doc;
  doc["device"] = "AgriSense-01";
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  doc["soilMoisture"] = soil;
  doc["lightLux"] = (lux >= 0) ? lux : 0;
  doc["relayStatus"] = pumpRelayState ? "ON" : "OFF";
  doc["timestamp"] = millis();

  char buffer[512];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_ENV, buffer, n);

  Serial.printf("Published → Temp=%.1fC | Hum=%.1f%% | Soil=%.1f%% | Light=%.1flux | Relay=%s\n",
                temp, hum, soil, lux, pumpRelayState ? "ON" : "OFF");
}

// ============================================================
// ĐỌC TOÀN BỘ CẢM BIẾN
// ============================================================
void readSensors(float &temp, float &hum, float &soil, float &lux)
{
  // --- DHT22: nhiệt độ + độ ẩm không khí ---
  temp = dht.readTemperature();
  hum = dht.readHumidity();

  if (isnan(temp))
  {
    temp = random(200, 350) / 10.0f;
  }
  if (isnan(hum))
  {
    hum = random(400, 800) / 10.0f;
  }

  // --- Độ ẩm đất (Potentiometer / ADC) ---
  soil = readSoilMoisture();

  // --- Ánh sáng (BH1750) ---
  lux = readLightLux();
}

// ============================================================
// KHỞI TẠO BH1750 (I2C)
// ============================================================
bool initBH1750()
{
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bh1750.begin(BH1750_CONTINUOUS_HIGH_RES_MODE))
  {
    Serial.println("[BH1750] Sensor not found! Check wiring.");
    return false;
  }
  Serial.println("[BH1750] Initialized OK (mode: High-Res)");
  return true;
}

// ============================================================
// SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  AgriSense IoT - Smart Irrigation");
  Serial.println("  ESP32 + DHT22 + BH1750 + Relay");
  Serial.println("  Soil + Air + Light + MQTT (TLS)");
  Serial.println("========================================");

  // --- GPIO ---
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);   // Relay OFF ban đầu

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(SOIL_PIN, INPUT);

  // --- DHT22 ---
  dht.begin();

  // --- BH1750 (I2C) ---
  lightSensorOK = initBH1750();

  // --- TLS ---
  espClient.setInsecure();

  // --- Wi-Fi ---
  setupWiFi();

  // --- MQTT ---
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  // --- InfluxDB ---
#ifdef INFLUXDB_ENABLED
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");
  if (influxClient.validateConnection())
  {
    Serial.printf("[INFLUX] Connected: %s\n", influxClient.getServerUrl().c_str());
  }
  else
  {
    Serial.printf("[INFLUX] Connection failed: %s\n", influxClient.getLastErrorMessage().c_str());
  }
#endif

  Serial.println("Setup complete!");
}

// ============================================================
// VÒNG LẶP CHÍNH
// ============================================================
void loop()
{
  // --- Wi-Fi ---
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WiFi] Lost! Reconnecting...");
    setupWiFi();
  }

  // --- MQTT ---
  if (!mqttClient.connected())
  {
    if (millis() - lastMQTTRetry >= MQTT_RETRY_MS)
    {
      lastMQTTRetry = millis();
      reconnectMQTT();
    }
  }
  mqttClient.loop();

  // --- PUBLISH định kỳ ---
  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL)
  {
    lastPublish = now;

    float temperature, humidity, soilMoisture, lightLux;
    readSensors(temperature, humidity, soilMoisture, lightLux);

    Serial.println("\n--- AgriSense Sensor Data ---");
    Serial.printf("  Temperature : %.1f C\n", temperature);
    Serial.printf("  Humidity    : %.1f %%\n", humidity);
    Serial.printf("  Soil Moist. : %.1f %%\n", soilMoisture);
    Serial.printf("  Light       : %.1f lux %s\n",
                  lightLux,
                  lightLux < 0 ? "(sensor error)" : "");

    // === CẢNH BÁO ĐỘ ẨM ĐẤT ===
    if (soilMoisture < SOIL_CRITICAL_THRESHOLD)
    {
      Serial.println("[ALERT] Soil CRITICAL - buzzer!");
      beepAlert(5, 300, 200);
    }
    else if (soilMoisture < SOIL_DRY_THRESHOLD)
    {
      Serial.println("[WARN] Soil is DRY!");
      beepAlert(2, 150, 150);
    }

    // === CẢNH BÁO ÁNH SÁNG (chỉ log, buzzer do Dashboard xử lý) ===
    if (lightSensorOK && lightLux >= 0 && lightLux < LIGHT_LOW_THRESHOLD)
    {
      Serial.printf("[WARN] Low light: %.0f lux < %.0f\n", lightLux, LIGHT_LOW_THRESHOLD);
    }

    // === GỬI MQTT + INFLUXDB ===
    if (mqttClient.connected())
    {
      publishSensorData(temperature, humidity, soilMoisture, lightLux);

      // Đồng bộ relay trạng thái với MQTT
      digitalWrite(PUMP_RELAY_PIN, pumpRelayState ? HIGH : LOW);

#ifdef INFLUXDB_ENABLED
      sendToInfluxDB(temperature, humidity, soilMoisture, lightLux, pumpRelayState);
#endif
    }
    else
    {
      Serial.println("[MQTT] Not connected, skipping publish...");
    }
  }
}
