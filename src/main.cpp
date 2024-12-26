#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSserver.h>
#include <Webserver.h>
#include <Ministache.h>

// type definitions
// TODO: コマンドの共通化(class?)
struct VMixCommandFunction {
  String Function;
  String Query;
};

enum class Screen {
  TALLY,
  NETWORK,
  SETTINGS,
  SETTINGS_QR,
  AP,
  TALLY_SET
};

enum class Mode {
  TALLY,
  ACTS,
};

enum Tally {
  SAFE,
  PGM,
  PRV,
  UNKNOWN,
};

// queue related
QueueHandle_t xQueueConnectWiFi = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueConnectVMix = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueVMixSendFunction = xQueueCreate( 1, sizeof( VMixCommandFunction ) );
QueueHandle_t xQueueShowSettingsQRCode = xQueueCreate( 1, sizeof( int8_t ) );
SemaphoreHandle_t preferencesSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t serialSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t spriteSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t clientSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreWiFi = xSemaphoreCreateMutex();

// instance
WiFiClient client;
Preferences preferences;
WebServer server;
M5Canvas sprite(&M5.Lcd);

// state
Screen currentScreen = Screen::TALLY;

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
  bool shouldRetry = true;
  bool isWiFiConnected = false;
  while(1){
    if (xQueueReceive(xQueueConnectWiFi, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Connecting to WiFi...");
    xSemaphoreGive(serialSemaphore);

    xSemaphoreTake(xSemaphoreWiFi, portMAX_DELAY);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (shouldRetry && !isWiFiConnected){
      if (WiFi.status() == WL_CONNECTED){
        xSemaphoreGive(xSemaphoreWiFi);
        shouldRetry = false;
        isWiFiConnected = true;
        delay(1);
        break;
      }
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("WiFi Retrying...");
      xSemaphoreGive(serialSemaphore);

      xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
      sprite.print('.');
      sprite.pushSprite(0, 0);
      xSemaphoreGive(spriteSemaphore);
      if(++retry > 20){
        xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
        sprite.println("Failed to connect to WiFi");
        sprite.pushSprite(0, 0);
        xSemaphoreGive(spriteSemaphore);

        // QRコード表示タスクへ切り替える
        int8_t SendValue = 0;
        xQueueSend(xQueueShowSettingsQRCode, &SendValue, portMAX_DELAY);
        shouldRetry = false;
        isWiFiConnected = false;
      }
     delay(1000);
    }

    xSemaphoreGive(xSemaphoreWiFi);
    if (!isWiFiConnected && !shouldRetry){
      delay(1);
      continue;
    };

    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    sprite.println("Connected to WiFi");
    sprite.pushSprite(0, 0);
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
      // TODO: 一定回数リトライしても失敗した場合、待機モードへ移行する
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to connect to vMix");
      xSemaphoreGive(serialSemaphore);
      xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
      sprite.println("Failed to connect to vMix");
      sprite.pushSprite(0, 0);
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

static void TaskDNSServer(void *pvParameters) {
  // DNSサーバータスク
  DNSServer dnsServer;

  while (!dnsServer.start(53, "*", WiFi.softAPIP())){
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Failed to start DNS server!");
    xSemaphoreGive(serialSemaphore);
    delay(1000);
    continue;
  }

  xSemaphoreTake(serialSemaphore, portMAX_DELAY);
  Serial.println("DNS server started!");
  xSemaphoreGive(serialSemaphore);

  while(true){
    dnsServer.processNextRequest();
  }
}

void handleCaptivePortal() {
  static const char captivePortalTemplateHTML[] = R"===(
<!DOCTYPE html>
<html lang="ja">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>M5Stack Control</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            line-height: 1.6;
            margin: 8px;
            padding: 0;
            display: flex;
            flex-direction: column;
            align-items: center;
            background-color: #f4f4f4;
        }
        header {
            background: #333;
            color: #fff;
            padding: 10px 20px;
            width: 100%;
            text-align: center;
        }
        form {
            background: #fff;
            padding: 20px;
            margin: 20px;
            border: 1px solid #ddd;
            border-radius: 5px;
            width: 100%;
            max-width: 400px;
            box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
        }
        label {
            display: block;
            margin-bottom: 8px;
            font-weight: bold;
        }
        input {
            width: 100%;
            padding: 10px;
            margin-bottom: 15px;
            border: 1px solid #ddd;
            border-radius: 5px;
        }
        button {
            display: block;
            width: 100%;
            background: #28a745;
            color: #fff;
            border: none;
            padding: 10px;
            border-radius: 5px;
            font-size: 16px;
            cursor: pointer;
        }
        button:hover {
            background: #218838;
        }
    </style>
</head>
<body>
    <header>
        <h1>M5Stack Control Panel</h1>
    </header>

    <form action="/settings" method="POST">
        <h2>vMix Configuration</h2>
        <label for="vmix-ip">IP Address</label>
        <input type="text" id="vmix-ip" name="ip" placeholder="Enter IP address" value="{{vmix_ip}}" required>

        <h2>Wi-Fi Configuration</h2>
        <label for="wifi-ssid">SSID</label>
        <input type="text" id="wifi-ssid" name="ssid" placeholder="Enter SSID" value="{{wifi_ssid}}" required>
        <label for="wifi-password">Password</label>
        <input type="password" id="wifi-password" name="password" placeholder="Enter Password" value="{{wifi_password}}" required>
        <button type="submit">Submit</button>
    </form>
</body>
</html>

)===";

  preferences.begin("vMixTally", true);
  JsonDocument data;
  data["vmix_ip"] = preferences.getString("vmix_ip");
  data["wifi_ssid"] = preferences.getString("wifi_ssid");
  data["wifi_password"] = preferences.getString("wifi_pass");
  preferences.end();
  auto resp = ministache::render(captivePortalTemplateHTML, data);
  server.send(200, "text/html", resp);
};

static void TaskHTTPServer(void *pvParameters) {
  // captive portal...
  server.on("/hotspot-detect.html", [&]() {
    handleCaptivePortal();
  });
  server.on("/generate_204", [&]() {
    handleCaptivePortal();
  });
  server.on("/portal", [&]() {
    handleCaptivePortal();
  });
  server.on("/connecttest.txt", [&]() {
    handleCaptivePortal(); 
  });
  server.on("/redirect", [&]() {
    handleCaptivePortal();
  });
  server.on("/success.txt", [&]() {
    handleCaptivePortal();
  });
  server.on("/wpad.dat", [&]() {
    handleCaptivePortal();
  });
  server.on("/ncsi.txt", [&]() {
    handleCaptivePortal();
  });
  server.on("/fwlink", [&]() {
    handleCaptivePortal();
  });
  server.onNotFound([&]() {
    server.sendHeader("Location", "/portal");
    server.send(302, "text/plain", "redirect to captive portal");
  });
  // APIs...
  server.on("/settings", HTTP_POST, [&]() {
    xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);
    preferences.begin("vMixTally", false);
    preferences.putString("vmix_ip", server.arg("ip"));
    preferences.putString("wifi_ssid", server.arg("ssid"));
    preferences.putString("wifi_pass", server.arg("password"));
    preferences.end();
    xSemaphoreGive(preferencesSemaphore);

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.printf("Settings saved: %s, %s, %s\n", server.arg("ip").c_str(), server.arg("ssid").c_str(), server.arg("password").c_str());
    xSemaphoreGive(serialSemaphore);

    server.send(200, "text/plain", "Success");

    ESP.restart();
  });

  server.begin();

  while (true) {
    server.handleClient();
  }
}

// スクリーン制御タスク...

static void TaskShowSetingsQRCode(void *pvParameters) {
  // QRコード表示タスク
  // WiFiをAPとしてスタートし、接続用QRコードを表示する
  auto xLastWakeTime = xTaskGetTickCount();
  int8_t ReceivedValue = 0;
  while (1) {
    if (xQueueReceive(xQueueShowSettingsQRCode, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      continue;
    }

    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Starting WiFi AP...");
    xSemaphoreGive(serialSemaphore);

    xSemaphoreTake(xSemaphoreWiFi, portMAX_DELAY);

    WiFi.mode(WIFI_MODE_APSTA);
    const char *ssid = "vMixTally";
    const char *password = "vMixTally";
    // use generated ssid and password instead
    if (!WiFi.softAP(ssid, password)) {
      xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
      sprite.println("failed to start WiFi AP");
      sprite.pushSprite(0, 0);
      xSemaphoreGive(spriteSemaphore);
      continue;
    }
    delay(300);
    // Fixed IPs
    IPAddress local_IP(192, 168, 4,22);
    IPAddress gateway(192, 168, 4,9);  
    IPAddress subnet(255, 255, 255,0); 
    if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
      xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
      sprite.println("WiFi AP configuration failed");
      sprite.pushSprite(0, 0);
      xSemaphoreGive(spriteSemaphore);
      continue;
    };

    // 画面表示
    Serial.println("Showing Settings QR Code");
    currentScreen = Screen::SETTINGS_QR;
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    sprite.fillScreen(TFT_BLACK);
    sprite.setTextColor(WHITE, BLACK);
    sprite.setCursor(0, 0);
    sprite.println();
    sprite.println("Scan QR Code to configure");
    sprite.printf("  SSID:%s\n PW:%s\n", ssid, password);
    sprite.pushSprite(0, 0);
    xSemaphoreGive(spriteSemaphore);

    // TODO: Split method / destructor

    char buf[61];
    auto qr = sprintf(buf, "WIFI:T:WPA;S:%s;P:%s;H:false;;", ssid, password);
    Serial.printf("QR Code: %s\n", buf);
    sprite.println();
    sprite.println();
    // 表示位置: 中央=(画面横幅/2)-(QRコードの幅/2)
    auto width = sprite.width()/3;
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    sprite.qrcode(buf, 0, (sprite.width()/2)-(width/2), width, 3);
    sprite.pushSprite(0, 0);
    xSemaphoreGive(spriteSemaphore);

    // HTTP/DNSサーバーを起動
    xTaskCreatePinnedToCore(TaskDNSServer, "DNSServer", 4096, NULL, 1,NULL, 1);
    xTaskCreatePinnedToCore(TaskHTTPServer, "HTTPServer", 4096, NULL, 1,NULL, 1);

    // captivePortalについて
    // なぜか実際に開かれるまでかかる時間が非常に長い
    // AP_STAモードだからかもしれないので、制御出来そうなら設定時はAP、それ以外はSTAにする
    delay(1);
  }
}

static void TaskShowTally(void *pvParameters) {
  // Tally表示タスク
  auto xLastWakeTime = xTaskGetTickCount();
  while (1) {
    // Tally表示処理
    // vMixから受け取ったデータを元に、Tally表示を行う
  }
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  auto cfg = M5.config();
  M5.begin(cfg);

  // display
  sprite.setColorDepth(8);
  void *p = sprite.createSprite(M5.Lcd.width(), M5.Lcd.height());
  if ( p == NULL ) {
    Serial.println("メモリが足りなくて確保できない...");
    for(;;){};
  }

  xTaskCreatePinnedToCore(TaskConnectToWiFi, "ConnectToWiFi", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskConnectVMix, "ConnectVMix", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskVMixReceiveClient, "VMixReceiveClient", 4096, NULL, 1,NULL, 1);
  // xTaskCreatePinnedToCore(TaskVMixSendClient, "VMixSendClient", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskShowSetingsQRCode, "ShowSettingsQRCode", 4096, NULL, 1,NULL, 1);
  
  // xQueueConnectWiFiに値を送信することでTaskConnectToWiFiを開始する
  int8_t SendValue = 0;
  xQueueSend(xQueueConnectWiFi, &SendValue, portMAX_DELAY);
}

void loop() {}