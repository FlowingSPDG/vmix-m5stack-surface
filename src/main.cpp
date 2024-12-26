#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>

// queue related
QueueHandle_t xQueue = xQueueCreate( 10, sizeof( unsigned long ) );
QueueHandle_t xQueueConnectVMix = xQueueCreate( 1, sizeof( int8_t ) );
SemaphoreHandle_t preferencesSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t serialSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t spriteSemaphore = xSemaphoreCreateMutex();

// instance
WiFiClient client;

Preferences preferences;

static void TaskSender(void *pvParameters) {
  auto coreID = xPortGetCoreID();
  auto xLastWakeTime = xTaskGetTickCount();
  BaseType_t xStatus;
  int32_t SendValue = 0;

  xSemaphoreTake(serialSemaphore, portMAX_DELAY);
  M5.Lcd.fillScreen(TFT_BLUE);
  xSemaphoreGive(serialSemaphore);
  while (1) {
    ++SendValue;
    xStatus = xQueueSend(xQueue, &SendValue, 0);
    if(xStatus != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("rtos queue send error...");
      xSemaphoreGive(serialSemaphore);
    }
    vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
  }
}

static void TaskReceiver(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  BaseType_t xStatus;
  int32_t ReceivedValue = 0;
  
  while (1) {
    xStatus = xQueueReceive(xQueue, &ReceivedValue, portTICK_PERIOD_MS);
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    if(xStatus == pdPASS){
      Serial.printf("ReceivedValue:%d\n", ReceivedValue);
    } else {
      Serial.println("rtos queue receive error...");
    }
    xSemaphoreGive(serialSemaphore);
    vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
  }
}

static void TaskConnectToWiFi(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();

  // WiFiへ接続し、完了したら自身を削除する
  // 失敗した場合、設定用QRコード表示タスクへ切り替える
  xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);
  preferences.begin("vMixTally", false);
  auto WIFI_SSID = preferences.getString("wifi_ssid");
  auto WIFI_PASS = preferences.getString("wifi_pass");
  preferences.end();
  xSemaphoreGive(preferencesSemaphore);

  xSemaphoreTake(serialSemaphore, portMAX_DELAY);
  Serial.printf("WIFI_SSID:%s\n", WIFI_SSID.c_str());
  Serial.printf("WIFI_PASS:%s\n", WIFI_PASS.c_str());
  xSemaphoreGive(serialSemaphore);

  int retry = 0;
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    M5.Lcd.print('.');
    xSemaphoreGive(spriteSemaphore);
    /*
    if(++retry > 20){
      xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
      M5.Lcd.println("Failed to connect to WiFi");
      xSemaphoreGive(spriteSemaphore);

      // QRコード表示タスクへ切り替える
      
      vTaskDelete(NULL);
      return;
    }
    */
  }
  xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
  M5.Lcd.println("Connected to WiFi");
  xSemaphoreGive(spriteSemaphore);
  int8_t SendValue = 0;
  xQueueSend(xQueueConnectVMix, &SendValue, portMAX_DELAY);
  vTaskDelete(NULL);
}

static void TaskConnectVMix(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  int8_t ReceivedValue = 0;

  xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);
  preferences.begin("vMixTally", true);
  auto VMIX_IP = preferences.getString("vmix_ip"); 
  preferences.end();
  xSemaphoreGive(preferencesSemaphore);

  xSemaphoreTake(serialSemaphore, portMAX_DELAY);
  Serial.printf("VMIX_IP:%s\n", VMIX_IP.c_str());
  xSemaphoreGive(serialSemaphore);

  while(1){
    if (xQueueReceive(xQueueConnectVMix, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      return;
    }
    
    while (!client.connect(VMIX_IP.c_str(), 8099)){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to connect to vMix");
      xSemaphoreGive(serialSemaphore);
      xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
      M5.Lcd.println("Failed to connect to vMix");
      xSemaphoreGive(spriteSemaphore);
      vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Connected to vMix!");
    Serial.println("------------");
    xSemaphoreGive(serialSemaphore);

    // Subscribe to the tally events
    client.println("SUBSCRIBE TALLY");
    client.println("SUBSCRIBE ACTS");
    vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  auto cfg = M5.config();
  M5.begin(cfg);

  preferences.begin("vMixTally", false);
  preferences.putString("vmix_ip", "192.168.1.6");
  preferences.end();

  xTaskCreatePinnedToCore(TaskSender, "Sender", 4096, NULL, 1,NULL, 0);
  xTaskCreatePinnedToCore(TaskReceiver, "Receiver", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskConnectToWiFi, "ConnectToWiFi", 4096, NULL, 1,NULL, 0);
  xTaskCreatePinnedToCore(TaskConnectVMix, "ConnectVMix", 4096, NULL, 1,NULL, 1);
}

void loop() {}