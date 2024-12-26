#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <DNSserver.h>
#include <ArxSmartPtr.h>
#include <TaskManager.h>
#include <Preferences.h>
#include <PinButton.h>
#include <Webserver.h>
#include <Ministache.h>
#include <stdio.h>
#include <string>

// types...
enum class Screen {
  TALLY,
  NETWORK,
  SETTINGS,
  SETTINGS_QR,
  AP,
  _,
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

class Engine : public Task::Base {
  // buttons
  PinButton btnA;
  PinButton btnB;
  PinButton btnC;

  // WiFi(server)
  String ssid_prefix = "vt-";
  char ssid[12] = "";
  char password[20] = "";
  // HTTP Header
  String header;

  // state
  bool vmix_connected = false;
  bool vmix_connecting = false;
  Screen currentState;
  Tally currentTally = Tally::UNKNOWN;
  int tally_target = 0;
  int current_input = 0; // only available in ACTS mode or XML API

  // Instances
  DNSServer dnsServer;
  WiFiClient client;
  WebServer server;
  std::shared_ptr<M5Canvas> sprite;

  // settings
  Preferences preferences;
  Mode mode = Mode::TALLY;
  const int VMIX_PORT = 8099;
  String ACTS_EVENT = "InputPreview";
  int ACTS_EVENT_NR = 1;
  int ACTS_EVENT_TARGET = 1;

  // methods
  template <size_t N>
  void generateRandomString(char (&buffer)[N]) {
      const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
      const size_t lettersCount = sizeof(letters) - 1; // ヌル文字を除外

      for (size_t i = 0; i < N - 1; i++) { // 終端文字を除く
          buffer[i] = letters[random(0, lettersCount)];
      }
      buffer[N - 1] = '\0'; // 終端文字を追加
  }


  // Clear LCD Screen
  void clearLCD() {
    sprite->fillScreen(TFT_BLACK);
    sprite->setCursor(0, 0);
  }

  // buttons
  void printBtnA(String s){
    sprite->setCursor(40, 220);
    sprite->print(s);
  }
  void printBtnB(String s){
    sprite->setCursor(145, 220);
    sprite->print(s);
  }
  void printBtnC(String s){
    sprite->setCursor(240, 220);
    sprite->print(s);
  }

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

  // Handle Tally State
  void displayTallyState(uint16_t bgcolor, uint16_t color, int x, int y, String state){
    sprite->fillScreen(bgcolor);
    sprite->setTextColor(color, bgcolor);
    sprite->setCursor(x, y);
    sprite->println(state);
  }

  // Menus
  // Show the current network settings
  void showNetworkScreen() {
    preferences.begin("vMixTally", true);
    auto WIFI_SSID = preferences.getString("wifi_ssid", "");
    preferences.end();

    Serial.println("Showing Network screen");
    currentState = Screen::NETWORK;
    clearLCD();
    sprite->fillScreen(TFT_BLACK);
    sprite->setTextSize(2);
    sprite->setTextColor(WHITE, BLACK);
    sprite->println();
    sprite->printf("SSID: %s\n", WIFI_SSID);
    sprite->printf("IP Address: %s\n", WiFi.localIP().toString());
    sprite->printf("Camera Num: %d\n", tally_target);
    printBtnA("BACK");
  }

  void showSettingsQRCode() {
    if (vmix_connecting) {
      return;
    }
    Serial.println("Showing Settings QR Code");
    currentState = Screen::SETTINGS_QR;
    clearLCD();
    sprite->fillScreen(TFT_BLACK);
    sprite->setTextSize(2);
    sprite->setTextColor(WHITE, BLACK);
    sprite->println();
    sprite->println("Scan QR Code to configure");
    sprite->printf("  SSID:%s\n PW:%s\n", ssid, password);
    sprite->pushSprite(0, 0);

    // TODO: Split method / destructor

    char buf[61];
    auto qr = sprintf(buf, "WIFI:T:WPA;S:%s;P:%s;H:false;;", ssid, password);
    Serial.printf("QR Code: %s\n", buf);
    sprite->println();
    sprite->println();
    // 表示位置: 中央=(画面横幅/2)-(QRコードの幅/2)
    auto width = sprite->width()/3;
    sprite->qrcode(buf, 0, (sprite->width()/2)-(width/2), width, 3);
    sprite->pushSprite(0, 0);
    printBtnA("BACK");
  }

  void showTallyScreen() {
    clearLCD();
    Serial.println("Showing Tally Screen");

    if (!vmix_connected) {
      return;
    }

    currentState = Screen::TALLY;
    sprite->setTextSize(10);
    if (mode == Mode::TALLY) {
    Serial.println("Tally Mode");
      switch (currentTally) {
        case SAFE:
          displayTallyState(BLACK,WHITE,70,90,"SAFE");
          break;
        case PGM:
          displayTallyState(RED,WHITE,90,90,"PGM");
          break;
        case PRV:
          displayTallyState(GREEN,BLACK,90,90,"PRV");
          break;
        case UNKNOWN:
          displayTallyState(BLACK,WHITE,80,90,"?");
          break;
        default:
          displayTallyState(BLACK,WHITE,80,90,"?");
      }
      Serial.println("Tally Mode process done");
    }
    Serial.println("Drawing sprite");
    // ACTS mode...
    sprite->setTextSize(2);
    Serial.println("Set TextSize done");
    sprite->setCursor(0, 0);
    Serial.println("Set Cursor done");
    sprite->printf("TARGET: %d\n", tally_target);
    Serial.println("Print Target done");
    sprite->printf("ACTIVE: %d\n", current_input);
    Serial.println("Print Active done");
    Serial.println("Draw sprite done. Drawing Buttons...");
    printBtnA("TALLY");
    printBtnB("SET");
    printBtnC("WIFI");
    Serial.println("Draw sprite done. Pushing!");
    sprite->pushSprite(0, 0);
  }

  void showTallySetScreen() {
    Serial.println("Showing Tally Set Screen");
    currentState = Screen::TALLY_SET;

    sprite->fillScreen(TFT_BLACK);
    sprite->setTextSize(2);
    sprite->setTextColor(WHITE, BLACK);
    sprite->setCursor(20,20);
    sprite->printf("Current Target: %d\n", tally_target);
    sprite->setCursor(20,60);

    printBtnA("OK");
    printBtnB("-");
    printBtnC("+");
  }

boolean connectToWifi() {
  int count = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (count > 9) {
      return false;
    }
    Serial.println("Awaiting WiFi connection...");
    count++;
    sprite->printf("WiFi failed(%d) retry...\n", count);
    sprite->pushSprite(0, 0);
    delay(5000);
  }
  Serial.println("Connected to WiFi!");
  count = 0;
  return true;
}

boolean connectTovMix() {
  if (vmix_connecting) {
    return false;
  }
  if (vmix_connected) {
    return true;
  }
  vmix_connecting = true;
  
  clearLCD();
  sprite->println("Connecting to vMix...");

  preferences.begin("vMixTally", true);
  
  auto VMIX_IP = preferences.getString("vmix_ip"); 
  preferences.end();
  int count = 0;
  while (0 == client.connect(VMIX_IP.c_str(),VMIX_PORT)) {
    Serial.printf("failed to connect vMix. at %s. Retrying...\n", VMIX_IP);
    count++;
    sprite->printf("vMix Connection failed(%d) retry...\n", count);
    delay(1000);
    if (count > 10) {
      vmix_connecting = false;
      return false;
    }
    sprite->pushSprite(0, 0);
  }
  vmix_connected = true;
  Serial.println("Connected to vMix!");
  Serial.println("------------");

  // Subscribe to the tally events
  client.println("SUBSCRIBE TALLY");
  client.println("SUBSCRIBE ACTS");
  vmix_connecting = false;
  return true;
}

// Handle incoming data
void handleData(String data) {
    Serial.printf("EVENT(RAW): %s\n",data);
  // Check if server data is tally data
  if (data.indexOf("TALLY OK") == 0) {
    // Pick the state of the current tally
    data = data.substring(sizeof("TALLY OK"));
    auto target = data.charAt(tally_target -1);
    auto newState = parseTallyInt(target);
    Serial.printf("target:%c newState:%c\n", target, newState);
    currentTally = newState;
    
    if (mode != Mode::TALLY) {
      return;
    }
    
    if (currentState == Screen::TALLY) {
      showTallyScreen();
      return;
    }
  }

  // Check if server data is ACTS data
  else if (data.indexOf("ACTS OK") == 0) {
    // Throw away first 8 characters(ACTS OK )
    data = data.substring(8);

    int spaceIndex = data.indexOf(" ");
    String event = data.substring(0, spaceIndex);
    Serial.printf("event:%s ", &(event[0]));

    auto remainingData = data.substring(spaceIndex + 1, data.length());
    spaceIndex = remainingData.indexOf(" ");
    int input = remainingData.substring(0, spaceIndex).toInt(); // sometimes float
    Serial.printf("input:%d ", input);

    remainingData = remainingData.substring(spaceIndex + 1, remainingData.length());
    int target = remainingData.toInt();
    Serial.printf("target:%d\n", target);

    if (event == "Input" && target == 1) {
      current_input = input;
    }
    
    Serial.printf("Apply ACTS Tally for target: %d\n", target);
    if (currentState == Screen::TALLY) {
      if (target == ACTS_EVENT_TARGET) {
        showTallyScreen();
        return;
      }
      showTallyScreen();
    }
  }
  else {
    Serial.print("Response from vMix: ");
    Serial.println(data);
  }
}

void showMsg(const char* msg){
  clearLCD();
  sprite->setTextSize(1);
  sprite->setTextColor(WHITE,BLACK);
  sprite->fillScreen(BLACK);
  sprite->println(msg);
}

void showTallyNum(String msg){
  clearLCD();
  sprite->setTextSize(5);
  sprite->setTextColor(WHITE,BLACK);
  sprite->fillScreen(BLACK);
  sprite->println(msg);
}

void showSettingsScreen() {
  Serial.println("Showing Settings screen");
  currentState = Screen::SETTINGS;
  clearLCD();

  preferences.begin("vMixTally", false);
  
  sprite->fillScreen(TFT_BLACK);
  sprite->setTextSize(2);
  sprite->setTextColor(WHITE, BLACK);
  sprite->println("Settings");
  
  sprite->println();
  sprite->println();
  sprite->println("vMix");
  sprite->printf("  IP: %s\n", preferences.getString("vmix_ip"));
  sprite->printf("  CAMERA: %d\n", preferences.getUInt("tally"));
  // sprite->printf("  STATUS: %d\n", preferences.getUInt("tally")); // CONNECTED
  sprite->println();
  
  sprite->println("Network");
  sprite->printf("  SSID: %s\n", preferences.getString("wifi_ssid")); 
  sprite->printf("  Password: %s\n", preferences.getString("wifi_pass"));
  // sprite->printf("  STATUS: %d\n", preferences.getUInt("tally")); // CONNECTED
  sprite->println();
  
  preferences.end();

  printBtnA("BACK");
  printBtnC("EDIT");
}

void updateTallyNR(int tally){
  preferences.begin("vMixTally", false);
  if(tally >= 1) {
    tally_target =  tally;  
    preferences.putUInt("tally", tally_target);
  }
  preferences.end();
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    WiFi.reconnect();
  }
}

public:
    Engine(const String& name)
    : Task::Base(name), btnA(0), btnB(0), btnC(0) {
        currentState = Screen::TALLY;
  
        Serial.println("beginning preferences...");
        preferences.begin("vMixTally", true);
        String ssid = preferences.getString("wifi_ssid");

        // TODO: WIFI AP

        tally_target = preferences.getUInt("tally");
        auto VMIX_IP = preferences.getString("vmix_ip");

        preferences.end();
        Serial.println("finished preferences...");
    }

    virtual ~Engine() {}

    // method chain
    Engine* ButtonA(const PinButton button) {
        btnA = button;
        return this;
    }
    Engine* ButtonB(const PinButton button) {
        btnB = button;
        return this;
    }
    Engine* ButtonC(const PinButton button) {
        btnC = button;
        return this;
    }
    Engine* Sprite(const std::shared_ptr<M5Canvas> lcd, int w, int h) {
        sprite = lcd;
        Serial.printf("Initializing sprite... width:%d, height:%d\n", w, h);
        sprite->setColorDepth(8);
        void *p = sprite->createSprite(w, h);
        if ( p == NULL ) {
          Serial.println("メモリが足りなくて確保できない");
        }
        return this;
    }

    void handleCaptivePortal() {
    // TODO: compress
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
    }

    virtual void enter() override {
        // WIFI settings
        WiFi.mode(WIFI_MODE_APSTA);
        generateRandomString(ssid);
        generateRandomString(password);
        Serial.printf("Generated SSID:%s password:%s\n", ssid, password);
        
        preferences.begin("vMixTally", true);
        auto WIFI_SSID = preferences.getString("wifi_ssid");
        auto WIFI_PASS = preferences.getString("wifi_pass");
        auto VMIX_IP = preferences.getString("vmix_ip"); 
        preferences.end();

        Serial.printf("Connecting to vMix. IP: %s\n", VMIX_IP);
        if (WIFI_SSID == "" || WIFI_PASS == "" || VMIX_IP == "") {
          showSettingsQRCode();
          return;
        }
        Serial.printf("Starting WiFi connection. SSID:%s PW:%s", WIFI_SSID, WIFI_PASS);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        Serial.printf("Starting WiFi. SSID:%s, Password:%s\n", ssid, password);
        if (!WiFi.softAP(ssid, password)) {
          sprite->println("failed to start WiFi AP");
          return;
        }
        delay(300);
        // Fixed IPs
        IPAddress local_IP(192, 168, 4,22);
        IPAddress gateway(192, 168, 4,9);  
        IPAddress subnet(255, 255, 255,0); 
        if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
            sprite->println("WiFi AP configuration failed");
            return;
        };
        sprite->printf("IP: %s\n", local_IP.toString());

        Serial.printf("Starting DNS server. IP:%s Port:%d\n", WiFi.softAPIP().toString(), 53);
        if (!dnsServer.start(53, "*", WiFi.softAPIP())) {
          sprite->println("failed to start DNS Server");
          return;
        }
    
        Serial.println("Starting Web server...");
        // HTTP Server

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
          server.send(200, "text/plain", "Success");
        });
        server.begin();

        sprite->fillScreen(TFT_BLACK);
        sprite->setTextSize(2);
        sprite->println("Initialized Engine...");
        sprite->pushSprite(0, 0);
        sprite->printf("CPU: %d MHz\n", getCpuFrequencyMhz());
        sprite->pushSprite(0, 0);
        delay(1000);

        Serial.println("STARTING...");
        while (!connectToWifi()) {
          showSettingsQRCode();
        }
        if (!connectTovMix()) {
          sprite->println("Fail(vMix TCP API timed out). Retrying...");
          sprite->pushSprite(0, 0);
          delay(5000);
        }
        Serial.println("Initialization complete. Showing TALLY screen");
        showTallyScreen();
    }

    virtual void update() override {
      bool shouldPushSprite = false;
      // update buttons
      btnA.update();
      btnB.update();
      btnC.update();

      server.handleClient();
      // handle WIFI server/client connection
      // TODO: Task
      if(client.available()) {
        String data = client.readStringUntil('\r\n');
        handleData(data);
      }

      dnsServer.processNextRequest();

      switch(currentState) {
        case Screen::TALLY:
          if (btnA.isClick()) {
            showTallySetScreen();
            shouldPushSprite = true;
          }

          if (btnB.isClick()) {
            showSettingsScreen();
            shouldPushSprite = true;
          }

          if (btnC.isClick()) {
            showNetworkScreen();
            shouldPushSprite = true;
          }

          break;
        case Screen::SETTINGS:
          if (btnA.isClick()) {
            showTallyScreen();
            shouldPushSprite = true;
          }

          if (btnC.isClick()) {
            showSettingsQRCode();
            shouldPushSprite = true;
          }

          break;

        case Screen::SETTINGS_QR:
          if (btnA.isClick()) {
            showSettingsScreen();
            shouldPushSprite = true;
          }

          return;
        case Screen::AP:
          if (btnA.isClick()) {
            showTallyScreen();
            shouldPushSprite = true;
          }
          
          break;
        case Screen::NETWORK:
          if (btnA.isClick()) {
            showTallyScreen();
            shouldPushSprite = true;
          }
          
          break;
        case Screen::TALLY_SET:
          if (btnA.isClick()) {
            showTallyScreen();
            shouldPushSprite = true;
          }

          if (btnB.isClick()) {
            updateTallyNR(tally_target - 1);
            currentTally = Tally::UNKNOWN;
            showTallySetScreen();
            shouldPushSprite = true;
          }

          if (btnC.isClick()) {
            updateTallyNR(tally_target + 1);
            currentTally = Tally::UNKNOWN;
            showTallySetScreen();
            shouldPushSprite = true;
          }
          break;
      }
      
      if(shouldPushSprite) {
        sprite->pushSprite(0, 0);
      }
      
      if(shouldPushSprite) {
        sprite->pushSprite(0, 0);
      }
    }
};

void setup() {
  // begin
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  auto cfg = M5.config();
  M5.begin(cfg);
  
  // configure buttons
  PinButton btnA(39);
  PinButton btnB(38);
  PinButton btnC(37);

  auto display = std::make_shared<M5Canvas>(&M5.Lcd);
  Tasks.add<Engine>("Engine")->
    ButtonA(btnA)->ButtonB(btnB)->ButtonC(btnC)->
    Sprite(display, M5.Lcd.width(), M5.Lcd.height())->
    startFps(60);
}

void loop() {
  Tasks.update();
}