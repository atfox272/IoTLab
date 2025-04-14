#define LED_PIN 48
#define SDA_PIN GPIO_NUM_11
#define SCL_PIN GPIO_NUM_12

#include <WiFi.h>
#include <Arduino_MQTT_Client.h>
// #include <HTTPClient.h>
#include <ThingsBoard.h>
#include "DHT20.h"
#include "Wire.h"
// #include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPClient.h> // Include HTTPClient library
#include <WiFiClientSecure.h>

constexpr char WIFI_SSID[] = "ATFox";
constexpr char WIFI_PASSWORD[] = "Trananhtai272";

constexpr char TOKEN[] = "wl6l3sfpxaeqts1a16nl";

constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

constexpr char MQTT_CLIENT_ID[] = "36040z4q1rmqbrwktxef";
constexpr char MQTT_USER[] = "dns3ezwxgd09m97kkn1q";
constexpr char MQTT_PASSWORD[] = "l6mf51aqcnsn3bndsb9k";

constexpr uint32_t MAX_MESSAGE_SIZE = 1024U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;

// constexpr char BLINKING_INTERVAL_ATTR[] = "blinkingInterval";
// constexpr char LED_MODE_ATTR[] = "ledMode";
// constexpr char LED_STATE_ATTR[] = "ledState";
constexpr char SCHED_STATE_ATTR[] = "schedState";
constexpr char FW_TITLE_ATTR[] = "fw_title";
constexpr char FW_VERSION_ATTR[] = "fw_version";
constexpr char FW_TAG_ATTR[] = "fw_tag";
constexpr char FW_URL_ATTR[] = "fw_url";

volatile bool attributesChanged = false;
volatile int ledMode = 0;
volatile bool ledState = false;

constexpr uint16_t BLINKING_INTERVAL_MS_MIN = 10U;
constexpr uint16_t BLINKING_INTERVAL_MS_MAX = 60000U;
volatile uint16_t blinkingInterval = 1000U;

uint32_t previousStateChange;

constexpr int16_t telemetrySendInterval = 10000U;
uint32_t previousDataSend;

constexpr std::array<const char *, 5U> SHARED_ATTRIBUTES_LIST = {
  SCHED_STATE_ATTR,
  FW_TITLE_ATTR,
  FW_VERSION_ATTR,
  FW_TAG_ATTR,
  FW_URL_ATTR
};

WiFiClient wifiClient;
Arduino_MQTT_Client mqttClient(wifiClient);
ThingsBoard tb(mqttClient, MAX_MESSAGE_SIZE);

DHT20 dht20;

// Firmware upgrage (current)
String fwVersion = "1.2";
String fwTitle = "OTA";
String fwTag = "OTA 1.2";
String fwVersionProc = "1.2";
String fwTitleProc = "OTA";
String fwTagProc = "OTA 1.2";
String fwUrlProc = "";
bool isFwUrlReady = false;
bool isFwOutdated = false;
bool isFwUpgradeTriggered = false; 

// Scheduler region
bool schedState = true;
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
  if(data == "getStateLED") {
    return RPC_Response("getStateLED", digitalRead(LED_PIN));
  }
  else {
    Serial.println("Received Switch state");
    bool newState = data;
    Serial.print("Switch state change: ");
    Serial.println(newState);
    digitalWrite(LED_PIN, newState);
    attributesChanged = true;
    return RPC_Response("setStateLED", newState);
  }
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
    if (strcmp(it->key().c_str(), SCHED_STATE_ATTR) == 0) { 
      schedState = it->value().as<bool>();
      
      if(schedState) { // Scheduler ON
        if(pSendTelemetryTask == NULL) {
          Serial.println("[INFO]: Scheduler is ON");
          xTaskCreate(sendTelemetryTask, "sendTelemetryTask", 4096, NULL, 2, &pSendTelemetryTask);
        }
      }
      else { // Scheduler OFF
        // Delete "Send Telemetry" task
        if(pSendTelemetryTask != NULL) {
          Serial.println("[INFO]: Scheduler is OFF");
          vTaskDelete(pSendTelemetryTask);
          pSendTelemetryTask = NULL;
        }
      }
    }
    if (strcmp(it->key().c_str(), FW_VERSION_ATTR) == 0) {
      String newFwVersion = it->value().as<String>();
      // Serial.print("[INFO]: Subscribe FW_VERSION_ATTR with value ");
      if(newFwVersion != fwVersion) {
        fwVersionProc = newFwVersion;
        isFwOutdated = true;
      }
    }
    if (strcmp(it->key().c_str(), FW_TAG_ATTR) == 0) {
      String newFwTag = it->value().as<String>();
      if(newFwTag != fwTag) {
        fwTagProc = newFwTag;
        isFwOutdated = true;
      }
    }
    if (strcmp(it->key().c_str(), FW_TITLE_ATTR) == 0) {
      String newFwTitle = it->value().as<String>();
      if(newFwTitle != fwTitle) {
        fwTitleProc = newFwTitle;
        isFwOutdated = true;
      }
    }
    if (strcmp(it->key().c_str(), FW_URL_ATTR) == 0) {
      String newFwUrl = it->value().as<String>();
      fwUrlProc = newFwUrl;
      isFwUrlReady = isFwOutdated;
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
      // tb.sendAttributeData(SCHED_STATE_ATTR, schedState);
    }

    tb.sendAttributeData("rssi", WiFi.RSSI());
    tb.sendAttributeData("channel", WiFi.channel());
    tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
    tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
    tb.sendAttributeData("ssid", WiFi.SSID().c_str());

    vTaskDelay(1000 / portTICK_PERIOD_MS); //1s delay
  }
}
void VersionMonitorTask(void *pvParameters) {
  while (true) {
    Serial.printf("[INFO]: Print from firmware with tag %s\n", fwTag);
    vTaskDelay(4000 / portTICK_PERIOD_MS); //4s delay
  }
  
}
void OTAUpdateTask(void *pvParameters) {
  WiFiClientSecure client;
  HTTPClient http;
  
  client.setInsecure();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  while(true) {
    // Serial.printf("[INFO]: Print from OTA Task - isFwOutdated: %s, isFwUrlReady: %s\n", isFwOutdated ? "true" : "false", isFwUrlReady ? "true" : "false");
    // Serial.printf("[INFO]: Print from OTA Task - Current firmware tag: %s, Current firmware version: %s\n", fwTag, fwVersion);
    if(isFwOutdated && isFwUrlReady) {
      Serial.println("[INFO]: New firmware version is ready");
      Serial.println("[INFO]: Connecting to " + String(fwUrlProc));
    
      http.begin(client, fwUrlProc);
      int httpCode = http.GET();
    
      if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[ERROR]: HTTP Response: %u\n", httpCode);
        Serial.printf("[ERROR]: HTTP GET firmware failed, error: %s\n", http.errorToString(httpCode).c_str());
        tb.sendAttributeData("fw_state", "FAILED");
        http.end();
        return;
      }
      
      int contentLength = http.getSize();
      if (contentLength <= 0) {
        Serial.println("[ERROR]: Firmware Content-Length is not valid");
        tb.sendAttributeData("fw_state", "FAILED");
        http.end();
        return;
      }
      
      bool canBegin = Update.begin(contentLength);
      if (!canBegin) {
        Serial.println("[WARN]: ESP32 does not enough space to begin OTA");
        tb.sendAttributeData("fw_state", "FAILED");
        http.end();
        return;
      }
    
      Serial.println("[INFO]: Begin OTA update");
      size_t written = Update.writeStream(*http.getStreamPtr());
    
      if (written == contentLength) {
        Serial.println("[INFO]: Written firmware successfully");
      } else {
        Serial.printf("[ERROR]: Written only %d/%d bytes. OTA failed!\n", written, contentLength);
        tb.sendAttributeData("fw_state", "FAILED");
        http.end();
        return;
      }
    
      if (Update.end()) {
        if (Update.isFinished()) {
          Serial.println("[INFO]: Update successfully completed. Rebooting...");
          tb.sendAttributeData("fw_state", "UPDATED");
          http.end();
          delay(1000);
          ESP.restart();
          return;
        } else {
          Serial.println("[ERROR]: Update not finished");
          tb.sendAttributeData("fw_state", "FAILED");
          http.end();
          return;
        }
      } else {
        Serial.printf("[INFO]: Update.end() failed with error %d\n", Update.getError());
        tb.sendAttributeData("fw_state", "FAILED");
        http.end();
        return;
      }
      isFwOutdated = false;
      isFwUrlReady = false;
      fwVersion = fwVersion;
      fwTitle = fwTitle;
      fwTag = fwTag;
    }
    vTaskDelay(4000 / portTICK_PERIOD_MS); //1s delay
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
  xTaskCreate(tbLoopTask, "tbLoopTask", 24576, NULL, 1, NULL);
  xTaskCreate(OTAUpdateTask, "OTAUpdateTask", 24576, NULL, 1, NULL);
  xTaskCreate(VersionMonitorTask, "VersionMonitorTask", 4096, NULL, 1, NULL);
  
}

void loop() {

}
