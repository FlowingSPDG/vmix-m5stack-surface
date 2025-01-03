#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSserver.h>
#include <Webserver.h>
#include <Ministache.h>
#include <StringSplitter.h>

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
QueueHandle_t xQueueConnectWiFi = xQueueCreate( 1, 0 );
QueueHandle_t xQueueConnectVMix = xQueueCreate( 1, 0 );
QueueHandle_t xQueueVMixSendFunction = xQueueCreate( 1, sizeof( VMixCommandFunction * ) );
QueueHandle_t xQueueShowSettingsQRCode = xQueueCreate( 1, 0 );
volatile QueueHandle_t xQueueShowTally = xQueueCreate( 1, sizeof( Tally ) );
volatile QueueHandle_t xQueueShowSettings = xQueueCreate( 1, 0 );
QueueHandle_t xQueueChangeSettings = xQueueCreate( 1, sizeof( uint32_t ) );
TaskHandle_t xTaskShowTallyHandle;
TaskHandle_t xTaskRetryVmixHandle;
TaskHandle_t xTaskConnectVMixHandle;

// instance
WiFiClient client;
Preferences preferences;
WebServer server;
M5Canvas sprite(&M5.Lcd);

// state
volatile Screen currentScreen = Screen::TALLY;
volatile uint32_t tallyTarget = 1;
uint32_t previewInput = 0;
uint32_t activeInput = 0;
boolean isTargetActive = false;
boolean isTargetPreview = false;
bool vMixConnected = false;
Mode currentMode = Mode::TALLY;
Tally current = Tally::UNKNOWN;

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

// TODO: m5unified-support
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

// Task

static void TaskShowTally(void *pvParameters) {
  // Tally表示タスク
  Tally ReceivedValue = Tally::UNKNOWN;
  int8_t SendValue = 0;

  while (1) {
    if (xQueueReceive(xQueueShowTally, &ReceivedValue, portMAX_DELAY) != pdPASS){
      delay(1);
      continue;
    }
    
    // TODO: 二重描画対策...

    sprite.setTextSize(5);
    switch (ReceivedValue){
      case Tally::SAFE:
        sprite.fillSprite(TFT_BLACK);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("SAFE", sprite.width()/2, sprite.height()/2, 5);
        break;
      case Tally::PGM:
        sprite.fillSprite(TFT_RED);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("PGM", sprite.width()/2, sprite.height()/2, 5);
        break;
      case Tally::PRV:
        sprite.fillSprite(TFT_GREEN);
        sprite.setTextColor(TFT_BLACK);
        sprite.drawCentreString("PRV", sprite.width()/2, sprite.height()/2, 5);
        break;
      case Tally::UNKNOWN:
        sprite.fillSprite(TFT_WHITE);
        sprite.setTextColor(TFT_BLACK);
        sprite.drawCentreString("UNKNOWN", sprite.width()/2, sprite.height()/2, 5);
        break;
      case Tally::DISCONNECTED:
        sprite.fillSprite(TFT_DARKGRAY);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawCentreString("DC", sprite.width()/2, sprite.height()/2, 5);
        break;
    }
    sprite.setCursor(0, 0);
    sprite.setTextSize(2);
    sprite.printf("TGT: %d\n", tallyTarget);
    sprite.printf("PRV: %d\n", previewInput);
    sprite.printf("PGM: %d\n", activeInput);

    printBtnA("SET");
    printBtnB("Prv");
    printBtnC("Cut");

    sprite.pushSprite(0, 0);

    current = ReceivedValue;
    currentScreen = Screen::TALLY;

    delay(1);
  }
}

static void TaskRetryVmix(void *pvParameters) {
  while(true){
    if (client.connected()){
      vMixConnected = true;
    }else {
      vMixConnected = false;
      xQueueSend(xQueueConnectVMix, NULL, portMAX_DELAY);
    }
    
    delay(1000);
  }
}

static void IRAM_ATTR onButtonA() {
  switch (currentScreen) {
    case Screen::TALLY:
      xQueueSendFromISR(xQueueShowSettings, NULL, NULL);
      break;
    case Screen::SETTINGS:
      xQueueSendFromISR(xQueueShowTally, &current, NULL);
      break;
    case Screen::TALLY_SET:
      xQueueSendFromISR(xQueueShowSettings, NULL, NULL);
      break;
  }
}

static void IRAM_ATTR onButtonB() {
  VMixCommandFunction *cmd = new VMixCommandFunction();
  switch (currentScreen) {
    case Screen::TALLY:
      cmd->Function = "PreviewInput";
      cmd->Query.clear();
      cmd->Query += "Input=" + String(tallyTarget);
      xQueueSendFromISR(xQueueVMixSendFunction, &cmd, NULL);
      return;
    case Screen::SETTINGS:
      xQueueSendFromISR(xQueueChangeSettings, (const void *)&tallyTarget, NULL);
      break;
    case Screen::TALLY_SET:
      tallyTarget--;
      if (tallyTarget < 1) {
        tallyTarget = 1;
      }
      xQueueSendFromISR(xQueueChangeSettings, (const void *)&tallyTarget, NULL);
      break;
  }
}

static void IRAM_ATTR onButtonC() {
  VMixCommandFunction *cmd = new VMixCommandFunction();
  switch (currentScreen) {
    case Screen::TALLY:
      cmd->Function = "Cut";
      cmd->Query.clear();
      // cmd->Query += "Input=" + String(tallyTarget);
      xQueueSendFromISR(xQueueVMixSendFunction, &cmd, NULL);
      return;
    case Screen::SETTINGS:
      xQueueSendFromISR(xQueueShowSettingsQRCode, NULL, NULL);
      break;
    case Screen::TALLY_SET:
      tallyTarget++;
      xQueueSendFromISR(xQueueChangeSettings, (const void *)&tallyTarget, NULL);
      break;
    case Screen::SETTINGS_QR:
      ESP.restart();
      break;
  }
}

static void TaskVMixReceiveClient(void *pvParameters) {
  String data;

  xTaskCreatePinnedToCore(TaskShowTally, "ShowTally", 4096, NULL, 1, &xTaskShowTallyHandle, PRO_CPU_NUM);

  while (1) {
    if(!vMixConnected) {
      delay(1);
      continue;
    }

    if (!client.available()) {
      delay(1);
      continue;
    }
    data = client.readStringUntil('\r\n');
    // Serial.printf("Received data from vMix: %s\n", data.c_str());

    // データ受信・パース処理
    // 受け取ったデータをパースし、tally/activators専用のqueueに送信する
    // 処理が重い場合、別のタスクに分割する事を検討する

    // TALLY
    if (data.startsWith("TALLY OK")) {
      if (currentMode == Mode::TALLY) {
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
    }

    // ACTS
    else if (data.startsWith("ACTS OK")) {
      // parse data
      data = data.substring(sizeof("ACTS OK"));
      auto splitter = StringSplitter(data.c_str(), ' ', 3);
      auto event = splitter.getItemAtIndex(0);
      auto input = static_cast<uint32_t>(splitter.getItemAtIndex(1).toInt());
      auto status = splitter.getItemAtIndex(2).toInt();

      // update
      if(event == "Input" && status == 1) {
        activeInput = input;
      }
      else if(event == "InputPreview" && status == 1) {
        previewInput = input;
      }

      // apply tally
      if (currentMode == Mode::ACTS) {
        if (input == tallyTarget) {
          if (event == "Input") {
            isTargetActive = (status == 1);
          } else if (event == "InputPreview") {
            isTargetPreview = (status == 1);
          }
        }

        if (isTargetActive) {
          current = Tally::PGM;
        } else if (isTargetPreview) {
          current = Tally::PRV;
        } else{
          current = Tally::SAFE;
        }
        if (currentScreen == Screen::TALLY) {
          xQueueSend(xQueueShowTally, &current, portMAX_DELAY);
        }
      }
    }

    delay(1);
  }
}

static void TaskVMixSendClient(void *pvParameters) {
  VMixCommandFunction *ReceivedValue;
  while (1) {
    if (xQueueReceive(xQueueVMixSendFunction, &ReceivedValue, portMAX_DELAY) != pdPASS){
      delay(1);
      continue;
    }

    // これはTCP APIに接続されてるかどうかの確認であっているのか？
    if (vMixConnected){
      // データ送信処理
      // tally/activators専用のqueueからデータを受け取り、vMixに送信する
      client.printf("FUNCTION %s %s\r\n", ReceivedValue->Function.c_str(), ReceivedValue->Query.c_str());
    } else {
      Serial.println("TaskVMixSendClient failed. vMix not connected");
    }

    delay(1);
  }
}

static void TaskConnectVMix(void *pvParameters) {
  const Tally tally = Tally::DISCONNECTED;

  preferences.begin("vMixTally", true);
  auto VMIX_IP = preferences.getString("vmix_ip"); 
  preferences.end();

  Serial.printf("VMIX_IP:%s\n", VMIX_IP.c_str());

  while(1){
    if (xQueueReceive(xQueueConnectVMix, NULL, portMAX_DELAY) != pdPASS){
      delay(1);
      continue;
    }
    
    Serial.println("Connecting to vMix...");
    
    if (vMixConnected) {
      Serial.println("Already connected to vMix");
      delay(1);
      continue;
    }
    if (!client.connect(VMIX_IP.c_str(), 8099, 50)){
      Serial.println("Failed to connect to vMix");
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
    client.println("TALLY");
    client.println("SUBSCRIBE ACTS");
    client.println("ACTS Input");
    client.println("ACTS InputPreview");
    delay(1);

    xTaskCreatePinnedToCore(TaskVMixReceiveClient, "VMixReceiveClient", 4096, NULL, 1,NULL, PRO_CPU_NUM);
    xTaskCreatePinnedToCore(TaskVMixSendClient, "VMixSendClient", 4096, NULL, 1, NULL, PRO_CPU_NUM);

    delay(1);
  }
}


static void TaskConnectToWiFi(void *pvParameters) {

  // WiFiへ接続し、完了したら自身を削除する
  // 失敗した場合、設定用QRコード表示タスクへ切り替える
  preferences.begin("vMixTally", false);
  auto WIFI_SSID = preferences.getString("wifi_ssid");
  auto WIFI_PASS = preferences.getString("wifi_pass");
  preferences.end();
  
  Serial.printf("WIFI_SSID:%s\n", WIFI_SSID.c_str());
  Serial.printf("WIFI_PASS:%s\n", WIFI_PASS.c_str());

  int retry = 0;
  int vMixRetry = 5;
  
  bool shouldRetry = true;
  bool isWiFiConnected = false;
  while(1){
    if (xQueueReceive(xQueueConnectWiFi, NULL, portMAX_DELAY) != pdPASS){
      delay(1);
      continue;
    }

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
        xQueueSend(xQueueShowSettingsQRCode, NULL, portMAX_DELAY);
        shouldRetry = false;
        isWiFiConnected = false;
        break;
      }
     delay(3000);
    }

    if (!isWiFiConnected && !shouldRetry){
      delay(1);
      continue;
    };

    sprite.println("Connected to WiFi");
    sprite.pushSprite(0, 0);

    Serial.println("Connected to WiFi");

    // Start vMix task
    xTaskCreatePinnedToCore(TaskConnectVMix, "ConnectVMix", 4096, NULL, 1, &xTaskConnectVMixHandle, PRO_CPU_NUM);
    xTaskCreatePinnedToCore(TaskRetryVmix, "RetryVMix", 4096, NULL, 1, &xTaskRetryVmixHandle, PRO_CPU_NUM);

    delay(5000);
    if(!vMixConnected){
      if (xTaskShowTallyHandle == NULL){
        xTaskCreatePinnedToCore(TaskShowTally, "ShowTally", 4096, NULL, 1, &xTaskShowTallyHandle, PRO_CPU_NUM);
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
    Serial.println("Failed to start DNS server!");
    delay(1);
    continue;
  }

  Serial.println("DNS server started!");

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
    preferences.begin("vMixTally", false);
    preferences.putString("vmix_ip", server.arg("ip"));
    preferences.putString("wifi_ssid", server.arg("ssid"));
    preferences.putString("wifi_pass", server.arg("password"));
    preferences.end();

    Serial.printf("Settings saved: %s, %s, %s\n", server.arg("ip").c_str(), server.arg("ssid").c_str(), server.arg("password").c_str());
    
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
      delay(1);
      continue;
    }

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
    printBtnA("BACK");
    printBtnB("-");
    printBtnC("+");
    sprite.pushSprite(0, 0);

    currentScreen = Screen::TALLY_SET;

    delay(1);
  }
}

static void TaskShowSettings(void *pvParameters) {
  // 設定画面表示タスク
  while (1) {
    if (xQueueReceive(xQueueShowSettings, NULL, portMAX_DELAY) != pdPASS){
      delay(1);
      continue;
    }

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
    printBtnA("BACK");
    printBtnB("EDIT");
    printBtnC("QR");
    sprite.pushSprite(0, 0);
    
    preferences.end();

    currentScreen = Screen::SETTINGS;

    delay(1);
  }
}

static void TaskShowSetingsQRCode(void *pvParameters) {
  // QRコード表示タスク
  // WiFiをAPとしてスタートし、接続用QRコードを表示する
  while (1) {
    if (xQueueReceive(xQueueShowSettingsQRCode, NULL, portMAX_DELAY) != pdPASS){
      delay(1);
      continue;
    }

    WiFi.disconnect();
    Serial.println("Starting WiFi AP...");
    WiFi.mode(WIFI_AP);
    const char *ssid = "vMixTally";
    const char *password = "vMixTally";
    // TODO: use generated ssid and password instead
    if (!WiFi.softAP(ssid, password)) {
      sprite.println("failed to start WiFi AP");
      sprite.pushSprite(0, 0);
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
    printBtnC("RESET");
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

    // HTTP/DNSサーバーを起動
    vTaskDelete(xTaskConnectVMixHandle);
    xTaskCreatePinnedToCore(TaskDNSServer, "DNSServer", 4096, NULL, 1,NULL, PRO_CPU_NUM);
    xTaskCreatePinnedToCore(TaskHTTPServer, "HTTPServer", 4096, NULL, 1,NULL, PRO_CPU_NUM);

    // captivePortalについて
    // なぜか実際に開かれるまでかかる時間が非常に長い...
    currentScreen = Screen::SETTINGS_QR;
    delay(1);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(1);
  }

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

  // tasks
  xTaskCreatePinnedToCore(TaskConnectToWiFi, "ConnectToWiFi", 4096, NULL, 1,NULL, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(TaskShowSetingsQRCode, "ShowSettingsQRCode", 4096, NULL, 1,NULL, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(TaskShowSettings, "ShowSettings", 4096, NULL, 1,NULL, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskShowTallySet, "ChangeSettings", 4096, NULL, 1,NULL, APP_CPU_NUM);
  
  // button
  attachInterrupt(39, onButtonA, FALLING);
  attachInterrupt(38, onButtonB, FALLING);
  attachInterrupt(37, onButtonC, FALLING);

  // xQueueConnectWiFiに値を送信することでTaskConnectToWiFiを開始する
  xQueueSend(xQueueConnectWiFi, NULL, portMAX_DELAY);
}

void loop() {}