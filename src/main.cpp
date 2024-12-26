#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>

// type definitions
// TODO: コマンドの共通化(class?)
struct VMixCommandFunction {
  String Function;
  String Query;
};

// queue related
QueueHandle_t xQueueConnectWiFi = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueConnectVMix = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueVMixSendFunction = xQueueCreate( 1, sizeof( VMixCommandFunction ) );
SemaphoreHandle_t preferencesSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t serialSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t spriteSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t clientSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreWiFi = xSemaphoreCreateMutex();

// instance
WiFiClient client;
Preferences preferences;

// Task

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
  
  int8_t ReceivedValue = 0;
  while(1){
    if (xQueueReceive(xQueueConnectWiFi, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      continue;
    }

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Connecting to WiFi...");
    xSemaphoreGive(serialSemaphore);

    xSemaphoreTake(xSemaphoreWiFi, portMAX_DELAY);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("WiFi Retrying...");
      xSemaphoreGive(serialSemaphore);

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
     vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }
    xSemaphoreGive(xSemaphoreWiFi);

    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    M5.Lcd.println("Connected to WiFi");
    xSemaphoreGive(spriteSemaphore);

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Connected to WiFi");
    xSemaphoreGive(serialSemaphore);

    int8_t SendValue = 0;
    xQueueSend(xQueueConnectVMix, &SendValue, portMAX_DELAY);
  };
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
      continue;
    }

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Connecting to vMix...");
    xSemaphoreGive(serialSemaphore);
    
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);
    if (client.connected()) {
      xSemaphoreGive(clientSemaphore);

      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Already connected to vMix");
      xSemaphoreGive(serialSemaphore);
      continue;
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
    xSemaphoreGive(clientSemaphore);
  }
}

static void TaskVMixReceiveClient(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  while (1) {
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);
    if(!client.available()) {
      xSemaphoreGive(clientSemaphore);
      continue;
    }
    auto data = client.readStringUntil('\r\n');
    xSemaphoreGive(clientSemaphore);

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.printf("Received data from vMix: %s\n", data.c_str());
    xSemaphoreGive(serialSemaphore);

    // データ受信・パース処理
    // 受け取ったデータをパースし、tally/activators専用のqueueに送信する
    // vMixにデータを送るのは別タスクで行う
  }
}

static void TaskVMixSendClient(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  VMixCommandFunction ReceivedValue;
  while (1) {
    if (xQueueReceive(xQueueVMixSendFunction, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      continue;
    }
    // これはTCP APIに接続されてるかどうかの確認であっているのか？
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);
    if (client.connected()) {
      // データ送信処理
      // tally/activators専用のqueueからデータを受け取り、vMixに送信する
      client.printf("%s %s\r\n", ReceivedValue.Function.c_str(), ReceivedValue.Query.c_str());
    }
    xSemaphoreGive(clientSemaphore);
  }
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  auto cfg = M5.config();
  M5.begin(cfg);

  xTaskCreatePinnedToCore(TaskConnectToWiFi, "ConnectToWiFi", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskConnectVMix, "ConnectVMix", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskVMixReceiveClient, "VMixReceiveClient", 4096, NULL, 1,NULL, 1);
  // xTaskCreatePinnedToCore(TaskVMixSendClient, "VMixSendClient", 4096, NULL, 1,NULL, 1);

  int8_t SendValue = 0;
  xQueueSend(xQueueConnectWiFi, &SendValue, portMAX_DELAY);
}

void loop() {}