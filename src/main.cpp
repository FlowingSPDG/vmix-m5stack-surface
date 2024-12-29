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
QueueHandle_t xQueueShowTally = xQueueCreate( 1, sizeof( Tally ) );
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

// utility functions
Tally parseTallyInt(char c) {
  switch (c) {
    case '0':
      return Tally::SAFE;
    case '1':
      return Tally::PGM;
    case '2':
      return Tally::PRV;
    default:
      return Tally::UNKNOWN;
  }
};

// Task

static void TaskShowTally(void *pvParameters) {
  // Tally表示タスク
  auto xLastWakeTime = xTaskGetTickCount();
  auto current = Tally::UNKNOWN;
  Tally ReceivedValue;
  while (1) {
    // TODO: Canvas/Sprite?
    if (xQueueReceive(xQueueShowTally, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    if (current == ReceivedValue){
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);

    switch (ReceivedValue){
      case Tally::SAFE:
        sprite.fillScreen(TFT_BLACK);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("SAFE", sprite.width()/2, sprite.height()/2, 4);
        Serial.println("SAFE");
        break;
      case Tally::PGM:
        sprite.fillScreen(TFT_RED);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("PGM", sprite.width()/2, sprite.height()/2, 4);
        Serial.println("PGM");
        break;
      case Tally::PRV:
        sprite.fillScreen(TFT_GREEN);
        sprite.setTextColor(TFT_BLACK);
        sprite.drawCentreString("PRV", sprite.width()/2, sprite.height()/2, 4);
        Serial.println("PRV");
        break;
      case Tally::UNKNOWN:
        sprite.fillScreen(TFT_WHITE);
        sprite.setTextColor(TFT_BLACK);
        sprite.drawCentreString("UNKNOWN", sprite.width()/2, sprite.height()/2, 4);
        Serial.println("UNKNOWN");
        break;
    }
    sprite.pushSprite(0, 0);

    current = ReceivedValue;
    
    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(spriteSemaphore);

    delay(1);
  }
}


static void TaskVMixReceiveClient(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  int tally_target = 1;
  String data;
  Tally current;

  xTaskCreatePinnedToCore(TaskShowTally, "ShowTally", 4096, NULL, 1,NULL, 1);

  while (1) {
    // take semaphore
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);

    if(!client.available()) {
      xSemaphoreGive(clientSemaphore);
      xSemaphoreGive(serialSemaphore);
      xSemaphoreGive(preferencesSemaphore);
      delay(1);
      continue;
    }
    data = client.readStringUntil('\r\n');
    Serial.printf("Received data from vMix: %s\n", data.c_str());

    // データ受信・パース処理
    // 受け取ったデータをパースし、tally/activators専用のqueueに送信する
    // vMixにデータを送るのは別タスクで行う

    // TODO: Modeによって処理を分ける
    // TALLY
    if (data.startsWith("TALLY OK")) {
      // fetch preference
      preferences.begin("vMixTally", true);
      tally_target = preferences.getUInt("tally");
      preferences.end();

      data = data.substring(sizeof("TALLY OK"));
      current = parseTallyInt(data.charAt(tally_target -1));
      // apply
      xQueueSend(xQueueShowTally, &current, portMAX_DELAY);
    }

    // ACTS
    if (data.startsWith("ACTS OK")) {
      // TODO...
    }

    // release semaphore
    xSemaphoreGive(clientSemaphore);
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(preferencesSemaphore);

    delay(1);
  }
}


static void TaskConnectVMix(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  int8_t ReceivedValue = 0;

  // take semaphore
  xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);
  xSemaphoreTake(serialSemaphore, portMAX_DELAY);

  preferences.begin("vMixTally", true);
  auto VMIX_IP = preferences.getString("vmix_ip"); 
  preferences.end();

  Serial.printf("VMIX_IP:%s\n", VMIX_IP.c_str());

  // release semaphore
  xSemaphoreGive(preferencesSemaphore);
  xSemaphoreGive(serialSemaphore);

  while(1){
    if (xQueueReceive(xQueueConnectVMix, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    
    Serial.println("Connecting to vMix...");
    
    if (client.connected()) {
      Serial.println("Already connected to vMix");
      // release semaphore
      xSemaphoreGive(serialSemaphore);
      xSemaphoreGive(clientSemaphore);
      xSemaphoreGive(spriteSemaphore);
      delay(1);
      continue;
    }
    while (!client.connect(VMIX_IP.c_str(), 8099)){
      // TODO: 一定回数リトライしても失敗した場合、待機モードへ移行する
      Serial.println("Failed to connect to vMix");
      sprite.println("Failed to connect to vMix");
      sprite.pushSprite(0, 0);
      delay(1);
    }
    Serial.println("Connected to vMix!");
    Serial.println("------------");

    // Subscribe to the tally events
    client.println("SUBSCRIBE TALLY");
    client.println("SUBSCRIBE ACTS");

    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(clientSemaphore);
    xSemaphoreGive(spriteSemaphore);

    xTaskCreatePinnedToCore(TaskVMixReceiveClient, "VMixReceiveClient", 4096, NULL, 1,NULL, 1);
    delay(1);
  }
}


static void TaskConnectToWiFi(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();

  // take semaphore
  xSemaphoreTake(serialSemaphore, portMAX_DELAY);
  xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);

  // WiFiへ接続し、完了したら自身を削除する
  // 失敗した場合、設定用QRコード表示タスクへ切り替える
  preferences.begin("vMixTally", false);
  auto WIFI_SSID = preferences.getString("wifi_ssid");
  auto WIFI_PASS = preferences.getString("wifi_pass");
  preferences.end();
  
  Serial.printf("WIFI_SSID:%s\n", WIFI_SSID.c_str());
  Serial.printf("WIFI_PASS:%s\n", WIFI_PASS.c_str());

  // release semaphore
  xSemaphoreGive(preferencesSemaphore);
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

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(xSemaphoreWiFi, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);

    Serial.println("Connecting to WiFi...");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (shouldRetry && !isWiFiConnected){
      if (WiFi.status() == WL_CONNECTED){
        shouldRetry = false;
        isWiFiConnected = true;
        delay(1);
        break;
      }
      Serial.println("WiFi Retrying...");

      sprite.print('.');
      sprite.pushSprite(0, 0);
      if(++retry > 20){
        sprite.println("Failed to connect to WiFi");
        sprite.pushSprite(0, 0);

        // QRコード表示タスクへ切り替える
        int8_t SendValue = 0;
        xQueueSend(xQueueShowSettingsQRCode, &SendValue, portMAX_DELAY);
        shouldRetry = false;
        isWiFiConnected = false;
        xSemaphoreGive(spriteSemaphore);
        xSemaphoreGive(xSemaphoreWiFi);
        xSemaphoreGive(serialSemaphore);
        delay(1);
        break;
      }
     delay(3000);
    }

    if (!isWiFiConnected && !shouldRetry){
      xSemaphoreGive(spriteSemaphore);
      xSemaphoreGive(xSemaphoreWiFi);
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    };

    sprite.println("Connected to WiFi");
    sprite.pushSprite(0, 0);

    Serial.println("Connected to WiFi");

    int8_t SendValue = 0;
    xQueueSend(xQueueConnectVMix, &SendValue, portMAX_DELAY);

    // release semaphore
    xSemaphoreGive(spriteSemaphore);
    xSemaphoreGive(xSemaphoreWiFi);
    xSemaphoreGive(serialSemaphore);

    // Start vMix task
    xTaskCreatePinnedToCore(TaskConnectVMix, "ConnectVMix", 4096, NULL, 1,NULL, 1);

    delay(1);
  };
}


static void TaskVMixSendClient(void *pvParameters) {
  auto xLastWakeTime = xTaskGetTickCount();
  VMixCommandFunction ReceivedValue;
  while (1) {
    if (xQueueReceive(xQueueVMixSendFunction, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);

    // これはTCP APIに接続されてるかどうかの確認であっているのか？
    if (client.connected()) {
      // データ送信処理
      // tally/activators専用のqueueからデータを受け取り、vMixに送信する
      client.printf("%s %s\r\n", ReceivedValue.Function.c_str(), ReceivedValue.Query.c_str());
    }

    // release semaphore
    xSemaphoreGive(clientSemaphore);

    delay(1);
  }
}

static void TaskDNSServer(void *pvParameters) {
  // DNSサーバータスク
  DNSServer dnsServer;

  while (!dnsServer.start(53, "*", WiFi.softAPIP())){
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    Serial.println("Failed to start DNS server!");
    xSemaphoreGive(serialSemaphore);
    delay(1);
    continue;
  }

  xSemaphoreTake(serialSemaphore, portMAX_DELAY);
  Serial.println("DNS server started!");
  xSemaphoreGive(serialSemaphore);

  while(true){
    dnsServer.processNextRequest();
    delay(1);
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
    delay(1);
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
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    xSemaphoreTake(xSemaphoreWiFi, portMAX_DELAY);

    Serial.println("Starting WiFi AP...");
    WiFi.mode(WIFI_MODE_APSTA);
    const char *ssid = "vMixTally";
    const char *password = "vMixTally";
    // use generated ssid and password instead
    if (!WiFi.softAP(ssid, password)) {
      sprite.println("failed to start WiFi AP");
      sprite.pushSprite(0, 0);
      xSemaphoreGive(serialSemaphore);
      xSemaphoreGive(spriteSemaphore);
      xSemaphoreGive(xSemaphoreWiFi);
      continue;
    }
    delay(300);
    // Fixed IPs
    IPAddress local_IP(192, 168, 4,22);
    IPAddress gateway(192, 168, 4,9);  
    IPAddress subnet(255, 255, 255,0); 
    if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
      sprite.println("WiFi AP configuration failed");
      sprite.pushSprite(0, 0);
      xSemaphoreGive(serialSemaphore);
      xSemaphoreGive(spriteSemaphore);
      xSemaphoreGive(xSemaphoreWiFi);
      continue;
    };

    // 画面表示
    Serial.println("Showing Settings QR Code");
    currentScreen = Screen::SETTINGS_QR;
    sprite.fillScreen(TFT_BLACK);
    sprite.setTextColor(WHITE, BLACK);
    sprite.setCursor(0, 0);
    sprite.println();
    sprite.println("Scan QR Code to configure");
    sprite.printf("  SSID:%s\n PW:%s\n", ssid, password);
    sprite.pushSprite(0, 0);

    // TODO: Split method / destructor

    char buf[61];
    auto qr = sprintf(buf, "WIFI:T:WPA;S:%s;P:%s;H:false;;", ssid, password);
    Serial.printf("QR Code: %s\n", buf);
    sprite.println();
    sprite.println();
    // 表示位置: 中央=(画面横幅/2)-(QRコードの幅/2)
    auto width = sprite.width()/3;
    sprite.qrcode(buf, 0, (sprite.width()/2)-(width/2), width, 3);
    sprite.pushSprite(0, 0);

    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(spriteSemaphore);
    xSemaphoreGive(xSemaphoreWiFi);

    // HTTP/DNSサーバーを起動
    xTaskCreatePinnedToCore(TaskDNSServer, "DNSServer", 4096, NULL, 1,NULL, 1);
    xTaskCreatePinnedToCore(TaskHTTPServer, "HTTPServer", 4096, NULL, 1,NULL, 1);

    // captivePortalについて
    // なぜか実際に開かれるまでかかる時間が非常に長い
    // AP_STAモードだからかもしれないので、制御出来そうなら設定時はAP、それ以外はSTAにする
    delay(1);
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
  xTaskCreatePinnedToCore(TaskShowSetingsQRCode, "ShowSettingsQRCode", 4096, NULL, 1,NULL, 1);
  
  // xQueueConnectWiFiに値を送信することでTaskConnectToWiFiを開始する
  int8_t SendValue = 0;
  xQueueSend(xQueueConnectWiFi, &SendValue, portMAX_DELAY);
}

void loop() {}