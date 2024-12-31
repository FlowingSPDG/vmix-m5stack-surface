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
  String Query; // TODO: これはどうするか? 配列?
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
  DISCONNECTED,
};

// queue related
QueueHandle_t xQueueConnectWiFi = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueConnectVMix = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueVMixSendFunction = xQueueCreate( 1, sizeof( VMixCommandFunction * ) );
QueueHandle_t xQueueShowSettingsQRCode = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueShowTally = xQueueCreate( 1, sizeof( Tally ) );
QueueHandle_t xQueueShowSettings = xQueueCreate( 1, sizeof( int8_t ) );
QueueHandle_t xQueueChangeSettings = xQueueCreate( 1, sizeof( uint32_t ) );
SemaphoreHandle_t preferencesSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t serialSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t spriteSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t clientSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreWiFi = xSemaphoreCreateMutex();
TaskHandle_t xTaskShowTallyHandle;
TaskHandle_t xTaskRetryVmixHandle;
TaskHandle_t xTaskConnectVMixHandle;

// instance
WiFiClient client;
Preferences preferences;
WebServer server;
M5Canvas sprite(&M5.Lcd);

// state
Screen currentScreen = Screen::TALLY;
uint32_t tallyTarget = 1;
bool vMixConnected = false;

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
  Tally current = Tally::UNKNOWN;
  Tally ReceivedValue = Tally::UNKNOWN;
  int8_t SendValue = 0;

  while (1) {
    if (xQueueReceive(xQueueShowTally, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }
    
    // TODO: 二重描画対策...

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);

    sprite.setTextSize(5);
    switch (ReceivedValue){
      case Tally::SAFE:
        sprite.fillSprite(TFT_BLACK);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("SAFE", sprite.width()/2, sprite.height()/2, 5);
        Serial.println("SAFE");
        break;
      case Tally::PGM:
        sprite.fillSprite(TFT_RED);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("PGM", sprite.width()/2, sprite.height()/2, 5);
        Serial.println("PGM");
        break;
      case Tally::PRV:
        sprite.fillSprite(TFT_GREEN);
        sprite.setTextColor(TFT_BLACK);
        sprite.drawCentreString("PRV", sprite.width()/2, sprite.height()/2, 5);
        Serial.println("PRV");
        break;
      case Tally::UNKNOWN:
        sprite.fillSprite(TFT_WHITE);
        sprite.setTextColor(TFT_BLACK);
        sprite.drawCentreString("UNKNOWN", sprite.width()/2, sprite.height()/2, 5);
        Serial.println("UNKNOWN");
        break;
      case Tally::DISCONNECTED:
        sprite.fillSprite(TFT_DARKGRAY);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("DC", sprite.width()/2, sprite.height()/2, 5);
        Serial.println("DISCONNECTED...");
        break;
    }

    sprite.pushSprite(0, 0);

    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(spriteSemaphore);

    current = ReceivedValue;
    currentScreen = Screen::TALLY;

    delay(1);
  }
}

static void TaskRetryVmix(void *pvParameters) {
  int8_t SendValue = 0;
  while(true){
    if (client.connected()){
      vMixConnected = true;
    }else {
      vMixConnected = false;
      xQueueSend(xQueueConnectVMix, &SendValue, portMAX_DELAY);
    }
    
    delay(1000);
  }
}

static void TaskButtonController(void *pvParameters) {
  int8_t SendValue = 0;
  VMixCommandFunction *cmd = new VMixCommandFunction();
  while (1) {
    M5.update();
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    switch(currentScreen){
      case Screen::TALLY:
        if (M5.BtnA.wasPressed()) {
          Serial.println("Button A pressed. Changing to Settings screen...");
          xQueueSend(xQueueShowSettings, &SendValue, portMAX_DELAY);
        }
        else if (M5.BtnB.wasPressed()) {
          cmd->Function = "PreviewInput";
          cmd->Query.clear();
          cmd->Query += "Input=" + String(tallyTarget);
          Serial.println("Button B pressed. Sending Function to vMix API...");
          Serial.printf("Function: %s Query:%s\n", cmd->Function, cmd->Query);
          xQueueSend(xQueueVMixSendFunction, &cmd, portMAX_DELAY);
        }
        else if (M5.BtnC.wasPressed()) {
          cmd->Function = "Cut";
          cmd->Query.clear();
          // cmd->Query += "Input=" + String(tallyTarget);
          Serial.println("Button C pressed. Sending Function to vMix API...");
          Serial.printf("Function: %s Query:%s\n", cmd->Function, cmd->Query);
          xQueueSend(xQueueVMixSendFunction, &cmd, portMAX_DELAY);
        }
        break;
      case Screen::SETTINGS:
        if (M5.BtnA.wasPressed()) {
          Serial.println("Button A pressed. Changing to Tally screen...");
          // TODO: タリーの保存・復元処理
          Tally SendValue = Tally::UNKNOWN;
          xQueueSend(xQueueShowTally, &SendValue, portMAX_DELAY);
        }
        else if (M5.BtnB.wasPressed()) {
          Serial.println("Button B pressed. Changing to Tally Set screen...");
          xQueueSend(xQueueChangeSettings, &tallyTarget, portMAX_DELAY);
        }
        else if (M5.BtnC.wasPressed()) {
          Serial.println("Button C pressed. Changing to Settings QR Code screen...");
          xQueueSend(xQueueShowSettingsQRCode, &SendValue, portMAX_DELAY);
        }
        break;
      case Screen::TALLY_SET:
        if (M5.BtnA.wasPressed()) {
          Serial.println("Button A pressed. Changing to Settings screen...");
          xQueueSend(xQueueShowSettings, &SendValue, portMAX_DELAY);
        }
        else if (M5.BtnB.wasPressed()) {
          Serial.println("Button B pressed. Changing to Tally Set screen...");
          tallyTarget--;
          xQueueSend(xQueueChangeSettings, &tallyTarget, portMAX_DELAY);
        }
        else if (M5.BtnC.wasPressed()) {
          Serial.println("Button C pressed. Changing to Tally Set screen...");
          tallyTarget++;
          xQueueSend(xQueueChangeSettings, &tallyTarget, portMAX_DELAY);
        }
        break;
    }
    xSemaphoreGive(serialSemaphore);

    delay(1);
  }
}

static void TaskVMixReceiveClient(void *pvParameters) {
  String data;
  Tally current;

  xTaskCreatePinnedToCore(TaskShowTally, "ShowTally", 4096, NULL, 1, &xTaskShowTallyHandle, 1);

  while (1) {
    if(!vMixConnected) {
      delay(1);
      continue;
    }

    if (!client.available()) {
      delay(1);
      continue;
    }
    // take semaphore
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);
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
      tallyTarget = preferences.getUInt("tally");
      preferences.end();

      data = data.substring(sizeof("TALLY OK"));
      current = parseTallyInt(data.charAt(tallyTarget -1));
      // 別の画面にいた場合ignoreする
      if (currentScreen == Screen::TALLY) {
        xQueueSend(xQueueShowTally, &current, portMAX_DELAY);
      }
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

static void TaskVMixSendClient(void *pvParameters) {
  VMixCommandFunction *ReceivedValue;
  while (1) {
    if (xQueueReceive(xQueueVMixSendFunction, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(clientSemaphore, portMAX_DELAY);

    // これはTCP APIに接続されてるかどうかの確認であっているのか？
    if (vMixConnected){
      // データ送信処理
      // tally/activators専用のqueueからデータを受け取り、vMixに送信する
      Serial.println("Sending data to vMix...");
      Serial.printf("Function: %s\n", ReceivedValue->Function.c_str());
      Serial.printf("Query: %s\n", ReceivedValue->Query.c_str());
      client.printf("FUNCTION %s %s\r\n", ReceivedValue->Function.c_str(), ReceivedValue->Query.c_str());
    }

    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(clientSemaphore);

    delay(1);
  }
}

static void printBtnA(String s){
  sprite.setCursor(40, 220);
  sprite.print(s);
}

static void printBtnB(String s){
  sprite.setCursor(145, 220);
  sprite.print(s);
}

static void printBtnC(String s){
  sprite.setCursor(240, 220);
  sprite.print(s);
}

static void TaskConnectVMix(void *pvParameters) {
  int8_t ReceivedValue = 0;
  const Tally tally = Tally::DISCONNECTED;

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
    
    Serial.println("Connecting to vMix...");
    
    if (vMixConnected) {
      Serial.println("Already connected to vMix");
      // release semaphore
      xSemaphoreGive(serialSemaphore);
      xSemaphoreGive(clientSemaphore);
      delay(1);
      continue;
    }
    if (!client.connect(VMIX_IP.c_str(), 8099, 50)){
      Serial.println("Failed to connect to vMix");

      // release semaphore
      xSemaphoreGive(serialSemaphore);
      xSemaphoreGive(clientSemaphore);
      vMixConnected = false;
      delay(1);
      continue;
    }

    vMixConnected = client.connected();
    Serial.println("Connected to vMix!");
    Serial.println("------------");
    delay(1);

    // Subscribe to the tally events
    client.println("SUBSCRIBE TALLY");
    client.println("SUBSCRIBE ACTS");
    delay(1);

    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(clientSemaphore);

    xTaskCreatePinnedToCore(TaskVMixReceiveClient, "VMixReceiveClient", 4096, NULL, 1,NULL, 1);
    xTaskCreatePinnedToCore(TaskVMixSendClient, "VMixSendClient", 4096, NULL, 1, NULL, 1);

    delay(1);
  }
}


static void TaskConnectToWiFi(void *pvParameters) {
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
  int vMixRetry = 5;
  
  int8_t ReceivedValue = 0;
  int8_t SendValue = 0;
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

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (shouldRetry && !isWiFiConnected){
      if (WiFi.status() == WL_CONNECTED){
        shouldRetry = false;
        isWiFiConnected = true;
        delay(1);
        break;
      }
      Serial.println("WiFi Retrying...");

      // TODO: Loading screen
      sprite.print('.');
      sprite.pushSprite(0, 0);
      if(++retry > 20){
        sprite.println("Failed to connect to WiFi");
        sprite.pushSprite(0, 0);

        // QRコード表示タスクへ切り替える
        xQueueSend(xQueueShowSettingsQRCode, &SendValue, portMAX_DELAY);
        shouldRetry = false;
        isWiFiConnected = false;
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

    // release semaphore
    xSemaphoreGive(spriteSemaphore);
    xSemaphoreGive(xSemaphoreWiFi);
    xSemaphoreGive(serialSemaphore);

    // Start vMix task
    xTaskCreatePinnedToCore(TaskConnectVMix, "ConnectVMix", 4096, NULL, 1, &xTaskConnectVMixHandle, 1);
    xTaskCreatePinnedToCore(TaskRetryVmix, "RetryVMix", 4096, NULL, 1, &xTaskRetryVmixHandle, 1);

    delay(5000);
    if(!vMixConnected){
      if (xTaskShowTallyHandle == NULL){
        xTaskCreatePinnedToCore(TaskShowTally, "ShowTally", 4096, NULL, 1, &xTaskShowTallyHandle, 1);
      }
      Tally SendValue = Tally::DISCONNECTED;
      xQueueSend(xQueueShowTally, &SendValue, portMAX_DELAY);
    }

    delay(1);
  };
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

static void TaskShowTallySet(void *pvParameters) {
  // キューを受信
  // 設定を変更(Target inputの変更, etc)
  // 画面を更新(入力はButtonControllerで行う)
  uint32_t ReceivedValue = 0;
  while (1) {
    if (xQueueReceive(xQueueChangeSettings, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);

    Serial.println("Changing Settings");

    preferences.begin("vMixTally", false);
    preferences.putUInt("tally", ReceivedValue);
    preferences.end();
    tallyTarget = ReceivedValue;

    sprite.fillScreen(TFT_BLACK);
    sprite.setTextSize(2);
    sprite.setTextColor(WHITE, BLACK);
    sprite.setCursor(0, 0);
    sprite.println("Settings(EDIT)");

    sprite.println();
    sprite.println();
    sprite.println("vMix");
    sprite.printf("  TALLY TARGET: %d\n", tallyTarget);
    sprite.println();
    sprite.pushSprite(0, 0);
    
    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(preferencesSemaphore);
    xSemaphoreGive(spriteSemaphore);

    currentScreen = Screen::TALLY_SET;

    delay(1);
  }
}

static void TaskShowSettings(void *pvParameters) {
  // 設定画面表示タスク
  int8_t ReceivedValue = 0;
  while (1) {
    if (xQueueReceive(xQueueShowSettings, &ReceivedValue, portMAX_DELAY) != pdPASS){
      xSemaphoreTake(serialSemaphore, portMAX_DELAY);
      Serial.println("Failed to receive queue");
      xSemaphoreGive(serialSemaphore);
      delay(1);
      continue;
    }

    // take semaphore
    xSemaphoreTake(serialSemaphore, portMAX_DELAY);
    xSemaphoreTake(spriteSemaphore, portMAX_DELAY);
    xSemaphoreTake(preferencesSemaphore, portMAX_DELAY);

    Serial.println("Showing Settings");
    preferences.begin("vMixTally", false);
    sprite.fillScreen(TFT_BLACK);
    sprite.setTextSize(2);
    sprite.setTextColor(WHITE, BLACK);
    sprite.setCursor(0, 0);
    sprite.println("Settings");

    sprite.println();
    sprite.println();
    sprite.println("vMix");
    sprite.printf("  IP: %s\n", preferences.getString("vmix_ip"));
    sprite.printf("  TALLY TARGET: %d\n", preferences.getUInt("tally"));
    sprite.println();

    sprite.println("Network");
    sprite.printf("  SSID: %s\n", preferences.getString("wifi_ssid").c_str()); 
    sprite.printf("  Password: %s\n", preferences.getString("wifi_pass").c_str());
    sprite.println();
    sprite.pushSprite(0, 0);
    
    preferences.end();

    // release semaphore
    xSemaphoreGive(serialSemaphore);
    xSemaphoreGive(spriteSemaphore);
    xSemaphoreGive(preferencesSemaphore);

    currentScreen = Screen::SETTINGS;

    delay(1);
  }
}

static void TaskShowSetingsQRCode(void *pvParameters) {
  // QRコード表示タスク
  // WiFiをAPとしてスタートし、接続用QRコードを表示する
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

    WiFi.disconnect();
    Serial.println("Starting WiFi AP...");
    WiFi.mode(WIFI_AP);
    const char *ssid = "vMixTally";
    const char *password = "vMixTally";
    // TODO: use generated ssid and password instead
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
    vTaskDelete(xTaskConnectVMixHandle);
    xTaskCreatePinnedToCore(TaskDNSServer, "DNSServer", 4096, NULL, 1,NULL, 1);
    xTaskCreatePinnedToCore(TaskHTTPServer, "HTTPServer", 4096, NULL, 1,NULL, 1);

    // captivePortalについて
    // なぜか実際に開かれるまでかかる時間が非常に長い...
    currentScreen = Screen::SETTINGS_QR;
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
  xTaskCreatePinnedToCore(TaskShowSettings, "ShowSettings", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskShowTallySet, "ChangeSettings", 4096, NULL, 1,NULL, 1);
  xTaskCreatePinnedToCore(TaskButtonController, "ButtonController", 4096, NULL, 1,NULL, 1);
  
  // xQueueConnectWiFiに値を送信することでTaskConnectToWiFiを開始する
  int8_t SendValue = 0;
  xQueueSend(xQueueConnectWiFi, &SendValue, portMAX_DELAY);
}

void loop() {}