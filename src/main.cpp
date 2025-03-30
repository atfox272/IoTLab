#define LED_PIN 48
#define SDA_PIN GPIO_NUM_11
#define SCL_PIN GPIO_NUM_12

#include <WiFi.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include "DHT20.h"
#include "Wire.h"
#include <ArduinoOTA.h>

constexpr char WIFI_SSID[] = "ATFox";
constexpr char WIFI_PASSWORD[] = "Trananhtai272";

constexpr char TOKEN[] = "7s5pokn2se622pzn1jxu";

constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

constexpr char MQTT_CLIENT_ID[] = "qb7zjusiyjwyxrmgjhqw";
constexpr char MQTT_USER[] = "r2eb0mx2ynmygqkom9fa";
constexpr char MQTT_PASSWORD[] = "62ur5tohahtser1k9nu5";

constexpr uint32_t MAX_MESSAGE_SIZE = 1024U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;

constexpr char BLINKING_INTERVAL_ATTR[] = "blinkingInterval";
constexpr char LED_MODE_ATTR[] = "ledMode";
constexpr char LED_STATE_ATTR[] = "ledState";

volatile bool attributesChanged = false;
volatile int ledMode = 0;
volatile bool ledState = false;

constexpr uint16_t BLINKING_INTERVAL_MS_MIN = 10U;
constexpr uint16_t BLINKING_INTERVAL_MS_MAX = 60000U;
volatile uint16_t blinkingInterval = 1000U;

uint32_t previousStateChange;

constexpr int16_t telemetrySendInterval = 10000U;
uint32_t previousDataSend;

constexpr std::array<const char *, 2U> SHARED_ATTRIBUTES_LIST = {
  LED_STATE_ATTR,
  BLINKING_INTERVAL_ATTR
};

WiFiClient wifiClient;
Arduino_MQTT_Client mqttClient(wifiClient);
ThingsBoard tb(mqttClient, MAX_MESSAGE_SIZE);

DHT20 dht20;

// Scheduler region
bool schedState;
TaskHandle_t pSendTelemetryTask;


void sendTelemetryTask(void *pvParameters) {
  while (true) {
      dht20.read();
      
      float temperature = dht20.getTemperature();
      float humidity = dht20.getHumidity();

      if (isnan(temperature) || isnan(humidity)) {
        Serial.println("Failed to read from DHT20 sensor!");
      } else {
        Serial.print("Temperature: ");
        Serial.print(temperature);
        Serial.print(" °C, Humidity: ");
        Serial.print(humidity);
        Serial.println(" %");

        tb.sendTelemetryData("temperature", temperature);
        tb.sendTelemetryData("humidity", humidity);
      }

      vTaskDelay(5000 / portTICK_PERIOD_MS); //5s delay
    }
}

RPC_Response setValueLED(const RPC_Data &data) {
  // Serial.println("Received Switch state");
  // bool newState = data;
  // Serial.print("Switch state change: ");
  // Serial.println(newState);
  // digitalWrite(LED_PIN, newState);
  // attributesChanged = true;
  // return RPC_Response("setStateLED", newState);

  Serial.println("Received switch scheduler state");
  bool newState = data;
  Serial.print("Switch scheduler state: ");
  Serial.println(newState);
  // digitalWrite(LED_PIN, newState);
  // TODO: 
  if(schedState != newState) { // State change
    if(newState == 1) { // Scheduler ON
      xTaskCreate(sendTelemetryTask, "sendTelemetryTask", 4096, NULL, 2, &pSendTelemetryTask);
    }
    else { // Scheduler OFF
      // Delete "Send Telemetry" task
      vTaskDelete(pSendTelemetryTask);
    }
    attributesChanged = true;
  }
  return RPC_Response("setStateLED", newState);
}

const std::array<RPC_Callback, 1U> callbacks = {
  RPC_Callback{ "setStateLED", setValueLED}
};

void InitWiFi() {
  Serial.println("Connecting to AP ...");
  // Attempting to establish a connection to the given WiFi network
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    // Delay 500ms until a connection has been successfully established
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to AP");
}

const bool reconnect() {
  // Check to ensure we aren't connected yet
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return true;
  }
  // If we aren't establish a new connection to the given WiFi network
  InitWiFi();
  return true;
}

void processSharedAttributes(const Shared_Attribute_Data &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (strcmp(it->key().c_str(), BLINKING_INTERVAL_ATTR) == 0) {
      const uint16_t new_interval = it->value().as<uint16_t>();
      if (new_interval >= BLINKING_INTERVAL_MS_MIN && new_interval <= BLINKING_INTERVAL_MS_MAX) {
        blinkingInterval = new_interval;
        Serial.print("Blinking interval is set to: ");
        Serial.println(new_interval);
      }
    } else if (strcmp(it->key().c_str(), LED_STATE_ATTR) == 0) {
      ledState = it->value().as<bool>();
      digitalWrite(LED_PIN, ledState);
      Serial.print("LED state is set to: ");
      Serial.println(ledState);
    }
  }
  attributesChanged = true;
}

const Shared_Attribute_Callback attributes_callback(&processSharedAttributes, SHARED_ATTRIBUTES_LIST.cbegin(), SHARED_ATTRIBUTES_LIST.cend());
const Attribute_Request_Callback attribute_shared_request_callback(&processSharedAttributes, SHARED_ATTRIBUTES_LIST.cbegin(), SHARED_ATTRIBUTES_LIST.cend());

//############RTOS task############//
void connectToWiFi(void * parameter) {
  while (true) {
    if (!reconnect()) {
      return;
    }
    vTaskDelay(10000 / portTICK_PERIOD_MS); //10s delay
  }
}

void coreIoTConnectTask(void *pvParameters) {
  while (true) {
    if (!tb.connected()) {
      Serial.print("Connecting to: ");
      Serial.print(THINGSBOARD_SERVER);
      Serial.print(" with token ");
      Serial.println(TOKEN);
      if (!tb.connect(THINGSBOARD_SERVER, MQTT_USER, THINGSBOARD_PORT, MQTT_CLIENT_ID, MQTT_PASSWORD)) {
        Serial.println("Failed to connect");
        // vTaskDelay(10 / portTICK_PERIOD_MS);
        continue;
      }

      tb.sendAttributeData("macAddress", WiFi.macAddress().c_str());

      Serial.println("Subscribing for RPC...");
      if (!tb.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
        Serial.println("Failed to subscribe for RPC");
        // vTaskDelay(10 / portTICK_PERIOD_MS);
        continue;
      }

      if (!tb.Shared_Attributes_Subscribe(attributes_callback)) {
        Serial.println("Failed to subscribe for shared attribute updates");
        // vTaskDelay(10 / portTICK_PERIOD_MS);
        continue;
      }

      Serial.println("Subscribe done");

      if (!tb.Shared_Attributes_Request(attribute_shared_request_callback)) {
        Serial.println("Failed to request for shared attributes");
        // vTaskDelay(10 / portTICK_PERIOD_MS);
        continue;
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); //1s delay
  }
}


void sendAtributesTask(void *pvParameters) {
  while (true) {
    if (attributesChanged) {
      attributesChanged = false;
      tb.sendAttributeData(LED_STATE_ATTR, digitalRead(LED_PIN));
    }

    tb.sendAttributeData("rssi", WiFi.RSSI());
    tb.sendAttributeData("channel", WiFi.channel());
    tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
    tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
    tb.sendAttributeData("ssid", WiFi.SSID().c_str());

    vTaskDelay(1000 / portTICK_PERIOD_MS); //1s delay
  }
}



void tbLoopTask(void *pvParameters) {
  while (true) {
    tb.loop();
    vTaskDelay(10 / portTICK_PERIOD_MS); //10ms delay
  }
}

void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);
  pinMode(LED_PIN, OUTPUT);
  delay(1000);
  InitWiFi();

  Wire.begin(SDA_PIN, SCL_PIN);
  dht20.begin();
  
  xTaskCreate(connectToWiFi, "connectToWiFi", 4096, NULL, 1, NULL);
  xTaskCreate(coreIoTConnectTask, "coreIoTConnectTask", 4096, NULL, 1, NULL);
  xTaskCreate(sendAtributesTask, "sendAtributesTask", 4096, NULL, 2, NULL);
  xTaskCreate(sendTelemetryTask, "sendTelemetryTask", 4096, NULL, 2, &pSendTelemetryTask);
  xTaskCreate(tbLoopTask, "tbLoopTask", 2048, NULL, 1, NULL);
  
}

void loop() {
  
}
