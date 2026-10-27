#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiS3.h>
#include <PubSubClient.h>

// ---------- Board ----------
const char* BOARD_ID = "someoneinthemall";

// ---------- WiFi ----------
const char* WIFI_SSID     = "someoneinthephone";
const char* WIFI_PASSWORD = "10101010";

// ---------- MQTT ----------
const char* MQTT_BROKER = "mqtt-dashboard.com";
const int   MQTT_PORT   = 1883;
String wordTopic;                  // pc/morse/<board id>/word
String statusTopic;                // pc/morse/<board id>/status

LiquidCrystal_I2C lcd(0x27, 16, 2);    // change to 0x3F if nothing shows
WiFiClient wifiClient;
PubSubClient client(wifiClient);

const int micPin = A0;
const int potPin = A1;                 // mic on/off knob (middle leg)
const int ledPin = 2;
const int deletePin = 3;               // delete button (other leg to GND)
const int sendPin = 4;                 // send button (other leg to GND)
const int sampleWindow = 50;           // ms
const int threshold = 750;             // adjust for your room

const int potOn  = 600;                // knob above this = mic ON
const int potOff = 400;                // knob below this = mic OFF

const unsigned long dashTime     = 400;   // longer than this = "-", shorter = "."
const unsigned long soundEndGap  = 150;   // silence this long ends a sound
const unsigned long letterGap    = 900;   // silence this long ends a letter
const unsigned long wordGap      = 2000;  // silence this long = space (Serial only)
const unsigned long debounceMs   = 50;

const bool debug = false;

const char* letters[] = {
  ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
  "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
  "..-", "...-", ".--", "-..-", "-.--", "--.."
};
const char* digits[] = {
  "-----", ".----", "..---", "...--", "....-",
  ".....", "-....", "--...", "---..", "----."
};

bool micOn = true;
bool inSound = false;
bool wordSpaceSent = true;
unsigned long soundStart = 0, lastLoud = 0, lastEnd = 0;
unsigned long statusUntil = 0;
String code = "";
String message = "";

struct Button {
  int pin;
  bool lastReading;
  bool state;
  unsigned long lastChange;
};
Button deleteBtn = {deletePin, HIGH, HIGH, 0};
Button sendBtn   = {sendPin,   HIGH, HIGH, 0};

// Returns true once each time the button is pressed (debounced)
bool pressedEvent(Button &b) {
  bool reading = digitalRead(b.pin);
  unsigned long now = millis();
  if (reading != b.lastReading) b.lastChange = now;
  b.lastReading = reading;

  if (now - b.lastChange > debounceMs && reading != b.state) {
    b.state = reading;
    if (b.state == LOW) return true;
  }
  return false;
}

char decodeMorse(const String &c) {
  for (int i = 0; i < 26; i++) if (c == letters[i]) return 'A' + i;
  for (int i = 0; i < 10; i++) if (c == digits[i])  return '0' + i;
  return '?';
}

// Shows the last 16 decoded letters on row 2
void updateLCD() {
  String shown = message;
  if (shown.length() > 16) shown = shown.substring(shown.length() - 16);
  lcd.setCursor(0, 1);
  lcd.print(shown);
  for (int i = shown.length(); i < 16; i++) lcd.print(' ');
}

void addToMessage(char c) {
  message += c;
  if (message.length() > 100) message.remove(0, message.length() - 100);
}

// Shows a short status on row 1 for 1.5 seconds
void showStatus(const char *text) {
  lcd.setCursor(0, 0);
  lcd.print(text);
  for (int i = strlen(text); i < 16; i++) lcd.print(' ');
  statusUntil = millis() + 1500;
}

void clearStatusIfDue() {
  if (statusUntil && millis() > statusUntil) {
    lcd.setCursor(0, 0);
    lcd.print("                ");
    statusUntil = 0;
  }
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed.");
    return false;
  }
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool connectMQTT() {
  if (client.connected()) return true;

  Serial.print("Connecting to MQTT...");
  String clientId = String("uno_r4_morse_") + BOARD_ID;
  // last will: if this board disappears, the broker publishes "offline"
  if (client.connect(clientId.c_str(), NULL, NULL, statusTopic.c_str(), 0, true, "offline")) {
    Serial.println("connected");
    client.publish(statusTopic.c_str(), "online", true);   // retained, so the game page sees it
    return true;
  }
  Serial.print("failed, rc=");
  Serial.println(client.state());
  return false;
}

// Connects to WiFi and MQTT if needed. Returns true if ready to send.
bool connectNetwork() {
  return connectWiFi() && connectMQTT();
}

void sendWord() {
  if (message.length() == 0) {
    Serial.println("Nothing to send.");
    showStatus("Nothing to send");
    return;
  }

  showStatus("Sending...");
  if (!connectNetwork()) {
    showStatus("Send failed");
    return;
  }

  if (client.publish(wordTopic.c_str(), message.c_str())) {
    Serial.print("Sent to ");
    Serial.print(wordTopic);
    Serial.print(": ");
    Serial.println(message);
    message = "";                      // start a fresh word
    updateLCD();
    showStatus("Sent!");
  } else {
    Serial.println("Publish failed.");
    showStatus("Send failed");
  }
}

// Checks both buttons
void checkButtons() {
  if (pressedEvent(deleteBtn) && message.length() > 0) {
    message.remove(message.length() - 1);
    Serial.print("Deleted. Text is now: ");
    Serial.println(message);
    updateLCD();
  }
  if (pressedEvent(sendBtn)) {
    sendWord();
  }
}

// Reads the knob and turns the mic ON/OFF
void checkMicSwitch() {
  analogRead(potPin);                  // dummy read lets the ADC settle
  int v = analogRead(potPin);

  if (!micOn && v > potOn) {
    micOn = true;
    Serial.println("Mic ON");
  } else if (micOn && v < potOff) {
    micOn = false;
    inSound = false;
    code = "";
    wordSpaceSent = true;
    digitalWrite(ledPin, LOW);
    Serial.println("Mic OFF");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(deletePin, INPUT_PULLUP);
  pinMode(sendPin, INPUT_PULLUP);
  analogReadResolution(10);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  wordTopic   = String("pc/morse/") + BOARD_ID + "/word";
  statusTopic = String("pc/morse/") + BOARD_ID + "/status";
  client.setServer(MQTT_BROKER, MQTT_PORT);

  lcd.setCursor(0, 0);
  lcd.print("Connecting...");
  if (connectNetwork()) showStatus("Online");
  else showStatus("Offline");

  micOn = true;                        // start ON, then let the knob decide
  checkMicSwitch();

  Serial.print("Board ID: ");
  Serial.println(BOARD_ID);
  Serial.println("Ready. Short sound = dot, long sound = dash.");
  Serial.println("Button 3 = delete, button 4 = send, knob = mic on/off.");
}

void loop() {
  client.loop();                       // keeps the MQTT connection alive
  clearStatusIfDue();
  checkMicSwitch();

  // Mic is off: ignore sound, but the buttons still work
  if (!micOn) {
    checkButtons();
    delay(10);
    return;
  }

  // Measure loudness over 50 ms (buttons are checked during this too)
  unsigned long start = millis();
  int signalMax = 0, signalMin = 1023;
  while (millis() - start < sampleWindow) {
    int s = analogRead(micPin);
    if (s > signalMax) signalMax = s;
    if (s < signalMin) signalMin = s;
    checkButtons();
  }
  int level = signalMax - signalMin;
  if (debug) Serial.println(level);

  bool loud = level > threshold;
  unsigned long now = millis();
  digitalWrite(ledPin, loud ? HIGH : LOW);

  if (loud) {
    if (!inSound) {
      inSound = true;
      soundStart = now - sampleWindow;
    }
    lastLoud = now;
  }

  // Sound ended -> decide dot or dash
  if (inSound && now - lastLoud > soundEndGap) {
    unsigned long duration = lastLoud - soundStart;
    char symbol = (duration < dashTime) ? '.' : '-';
    if (code.length() < 7) code += symbol;
    Serial.print(symbol);
    Serial.print("  (");
    Serial.print(duration);
    Serial.println(" ms)");
    inSound = false;
    lastEnd = lastLoud;
    wordSpaceSent = false;
  }

  // Quiet long enough -> decode the letter and show it
  if (!inSound && code.length() > 0 && now - lastEnd > letterGap) {
    char letter = decodeMorse(code);
    Serial.print(code);
    Serial.print("  ->  ");
    Serial.println(letter);
    addToMessage(letter);
    code = "";
    updateLCD();
  }

  // Quiet much longer -> word space (Serial Monitor only)
  if (!inSound && code.length() == 0 && !wordSpaceSent && now - lastEnd > wordGap) {
    Serial.println("[space]");
    wordSpaceSent = true;
  }
}
