// SportLineBot - ESP32-S3 firmware
//
// Board         : ESP32-S3 Dev Module
// Link          : WiFi, WebSocket server on port 81 (text protocol, see README)
// Motion model  : open loop, time based (MS_PER_CM / MS_PER_DEG must be calibrated)

#include <ESP32Servo.h>
#include <WiFi.h>
#include <WebSocketsServer.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD (copy secrets.example.h)

WebSocketsServer webSocket(81);

// --- Servo (ultrasonic sensor pan) ---------------------------------------
#define SERVO_PIN 14
Servo servo;

// --- DC motors through an L298N driver ------------------------------------
#define IN1 5    // left motor forward
#define IN2 18   // left motor backward
#define IN3 21   // right motor forward
#define IN4 4    // right motor backward

// --- HC-SR04 ultrasonic sensor ---------------------------------------------
#define TRIG_PIN 13
#define ECHO_PIN 12  // 5 V echo: use a 1k/2k voltage divider for the 3.3 V GPIO

// --- Peripherals ----------------------------------------------------------
#define PUMP_PIN    26
#define LEVEL_PIN    1   // analog paint level sensor (ADC)
#define LED_GREEN    6
#define LED_RED      2
#define BUZZER_PIN   3

// --- State ------------------------------------------------------------------
String command        = "";
bool   pumpActive     = false;
bool   pauseActive    = false;
float  posX           = 0.0;
float  posY           = 0.0;
float  currentAngle   = 0.0;
int    clientId       = -1;

// --- Calibration (measure on your own robot) ------------------------------------
int MS_PER_CM  = 35;  // milliseconds to drive 1 cm
int MS_PER_DEG = 10;  // milliseconds to rotate 1 degree

// --- Field dimensions in cm (scaled-down prototype) -------------------------------
float fieldLength = 100.0;
float fieldWidth  =  60.0;

// Safety thresholds
const int OBSTACLE_MIN_CM   = 15;  // stop when an obstacle is closer than this
const int PAINT_EMPTY_PCT   =  3;  // refuse to run the pump at or below this level
const int PAINT_LOW_PCT     = 10;  // warn at or below this level

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  stopMotors();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(PUMP_PIN,   OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(PUMP_PIN, LOW);

  servo.attach(SERVO_PIN);
  servo.write(90);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
    beep(1, 200);
    digitalWrite(LED_GREEN, HIGH);
    delay(500);
    digitalWrite(LED_GREEN, LOW);
  } else {
    Serial.println("\nWiFi connection failed.");
    digitalWrite(LED_RED, HIGH);
    beep(3, 300);
  }

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81");
}

// ----------------------------------------------------------------------------
void loop() {
  webSocket.loop();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    digitalWrite(LED_RED, HIGH);
    WiFi.reconnect();
    delay(2000);
    if (WiFi.status() == WL_CONNECTED) {
      digitalWrite(LED_RED, LOW);
      Serial.println("WiFi reconnected: " + WiFi.localIP().toString());
    }
  }

  // Commands can also be typed in the serial monitor (debugging).
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      command.trim();
      if (command.length() > 0) handleCommand(command);
      command = "";
    } else {
      command += c;
    }
  }
}

// --- WebSocket events ----------------------------------------------------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      clientId = num;
      Serial.printf("Client %u connected\n", num);
      webSocket.sendTXT(num, "READY");
      beep(2, 100);
      break;

    case WStype_DISCONNECTED:
      Serial.printf("Client %u disconnected\n", num);
      if (clientId == num) clientId = -1;
      stopMotors();
      setPump(false);
      break;

    case WStype_TEXT:
      command = String((char*)payload);
      command.trim();
      if (command.length() > 0) handleCommand(command);
      break;

    default:
      break;
  }
}

// Print to serial and send to the connected client.
void reply(String msg) {
  Serial.println(msg);
  if (clientId >= 0) {
    webSocket.sendTXT(clientId, msg);
    delay(10);  // small gap so back-to-back messages do not saturate the socket
  }
}

// --- Command handling --------------------------------------------------------
void handleCommand(String cmd) {
  Serial.println("CMD: " + cmd);

  if (cmd.startsWith("START:")) {                 // paint while driving N cm
    float cm = cmd.substring(6).toFloat();
    setPump(true);
    driveCM(cm);
    setPump(false);
    reply("OK:START");
  }

  else if (cmd.startsWith("MOVE:")) {             // drive N cm without painting
    setPump(false);
    driveCM(cmd.substring(5).toFloat());
    reply("OK:MOVE");
  }

  else if (cmd.startsWith("TURN_RIGHT:")) {
    turnRight(cmd.substring(11).toFloat());
    reply("OK:TURN_RIGHT");
  }

  else if (cmd.startsWith("TURN_LEFT:")) {
    turnLeft(cmd.substring(10).toFloat());
    reply("OK:TURN_LEFT");
  }

  else if (cmd == "STOP") {
    stopMotors();
    setPump(false);
    pauseActive = false;
    digitalWrite(LED_GREEN, LOW);
    reply("OK:STOP");
  }

  else if (cmd == "PAUSE") {
    pauseActive = true;
    stopMotors();
    setPump(false);
    reply("OK:PAUSE");
  }

  else if (cmd == "RESUME") {
    pauseActive = false;
    reply("OK:RESUME");
  }

  else if (cmd == "STOP_PUMP") {
    setPump(false);
    reply("OK:STOP_PUMP");
  }

  else if (cmd.startsWith("CIRCLE:")) {
    drawCircle(cmd.substring(7).toFloat());
    reply("OK:CIRCLE");
  }

  else if (cmd.startsWith("HALF_CIRCLE:")) {      // HALF_CIRCLE:<radius>:<RIGHT|LEFT>
    String p      = cmd.substring(12);
    int    sep    = p.indexOf(':');
    float  radius = p.substring(0, sep).toFloat();
    String side   = p.substring(sep + 1);
    drawHalfCircle(radius, side == "RIGHT");
    reply("OK:HALF_CIRCLE");
  }

  else if (cmd.startsWith("ARC:")) {              // ARC:<radius>:<start deg>:<end deg>
    String p  = cmd.substring(4);
    int s1 = p.indexOf(':');
    int s2 = p.lastIndexOf(':');
    float radius   = p.substring(0, s1).toFloat();
    int startAngle = p.substring(s1 + 1, s2).toInt();
    int endAngle   = p.substring(s2 + 1).toInt();
    drawArc(radius, startAngle, endAngle);
    reply("OK:ARC");
  }

  else if (cmd == "SCAN") {
    scanSurroundings();
  }

  else if (cmd == "STATUS") {                     // STATUS:<paint %>:<distance cm>
    int  level = readPaintLevel();
    long dist  = readUltrasonic();
    reply("STATUS:" + String(level) + ":" + String(dist));
  }

  else if (cmd == "PUMP_TEST") {
    Serial.println("Pump test, 2 s...");
    digitalWrite(PUMP_PIN, HIGH);
    delay(2000);
    digitalWrite(PUMP_PIN, LOW);
    reply("OK:PUMP_TEST");
  }

  else if (cmd.startsWith("CALIB_CM:")) {
    MS_PER_CM = cmd.substring(9).toInt();
    reply("OK:CALIB_CM:" + String(MS_PER_CM));
  }

  else if (cmd.startsWith("CALIB_DEG:")) {
    MS_PER_DEG = cmd.substring(10).toInt();
    reply("OK:CALIB_DEG:" + String(MS_PER_DEG));
  }

  else if (cmd.startsWith("DIMENSIONS:")) {       // DIMENSIONS:<length cm>:<width cm>
    String p   = cmd.substring(11);
    int    sep = p.indexOf(':');
    fieldLength = p.substring(0, sep).toFloat();
    fieldWidth  = p.substring(sep + 1).toFloat();
    reply("OK:DIMENSIONS:" + String(fieldLength) + ":" + String(fieldWidth));
  }

  else if (cmd == "FOOTBALL_FIELD") {
    drawFootballField();
  }

  else if (cmd == "IP") {
    reply("IP:" + WiFi.localIP().toString());
  }

  else {
    reply("ERR:UNKNOWN_COMMAND:" + cmd);
  }
}

// --- Motion -------------------------------------------------------------------
void driveCM(float cm) {
  if (pauseActive) return;

  long distance = readUltrasonic();
  if (distance > 0 && distance < OBSTACLE_MIN_CM) {
    obstacleAlert();
    reply("ERR:OBSTACLE");
    return;
  }

  Serial.println("DRIVE " + String(cm, 1) + " cm");
  digitalWrite(LED_GREEN, HIGH);

  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  delay((long)(cm * MS_PER_CM));

  stopMotors();
  digitalWrite(LED_GREEN, LOW);

  posX += cm * cos(currentAngle * PI / 180.0);
  posY += cm * sin(currentAngle * PI / 180.0);

  reply("POS:" + String(posX, 1) + ":" + String(posY, 1));
}

void turnRight(float degrees) {
  if (pauseActive) return;
  Serial.println("TURN RIGHT " + String(degrees, 0) + " deg");

  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  delay((long)(degrees * MS_PER_DEG));
  stopMotors();

  currentAngle -= degrees;
  while (currentAngle < 0) currentAngle += 360;
}

void turnLeft(float degrees) {
  if (pauseActive) return;
  Serial.println("TURN LEFT " + String(degrees, 0) + " deg");

  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  delay((long)(degrees * MS_PER_DEG));
  stopMotors();

  currentAngle += degrees;
  while (currentAngle >= 360) currentAngle -= 360;
}

void stopMotors() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

// --- Geometry -------------------------------------------------------------------
// Curves are approximated by short straight segments followed by a small turn.
void drawCircle(float radius) {
  int   segments = 36;
  float segment  = (2.0 * PI * radius) / segments;
  setPump(true);
  for (int i = 0; i < segments; i++) {
    if (pauseActive) { setPump(false); return; }
    driveCM(segment);
    turnRight(10.0);
  }
  setPump(false);
}

void drawHalfCircle(float radius, bool turnToRight) {
  int   segments = 18;
  float segment  = (PI * radius) / segments;
  setPump(true);
  for (int i = 0; i < segments; i++) {
    if (pauseActive) { setPump(false); return; }
    driveCM(segment);
    if (turnToRight) turnRight(10.0);
    else             turnLeft(10.0);
  }
  setPump(false);
}

void drawArc(float radius, int startAngle, int endAngle) {
  int totalDeg = endAngle - startAngle;
  int segments = totalDeg / 10;
  if (segments <= 0) return;
  float segment = (2.0 * PI * radius * totalDeg / 360.0) / segments;
  setPump(true);
  for (int i = 0; i < segments; i++) {
    if (pauseActive) { setPump(false); return; }
    driveCM(segment);
    turnRight((float)totalDeg / segments);
  }
  setPump(false);
}

// --- Football field --------------------------------------------------------------
// Seven steps, announced to the dashboard with STEP:<n>:<name>.
void drawFootballField() {
  float L  = fieldLength;
  float W  = fieldWidth;
  float halfL = L / 2.0;
  float halfW = W / 2.0;

  float penaltyL = L * 0.20;   // penalty area depth
  float penaltyW = W * 0.60;   // penalty area width
  float goalL    = L * 0.10;   // goal area depth
  float goalW    = W * 0.30;   // goal area width
  float circleR  = W * 0.15;   // centre circle radius

  reply("BEGIN:FOOTBALL_FIELD");
  Serial.println("Drawing football field...");

  // 1. Outer rectangle
  reply("STEP:1:Outer rectangle");
  setPump(true);
  driveCM(L); turnRight(90);
  driveCM(W); turnRight(90);
  driveCM(L); turnRight(90);
  driveCM(W); turnRight(90);
  setPump(false);

  // 2. Halfway line
  reply("STEP:2:Halfway line");
  driveCM(halfL);
  turnRight(90);
  setPump(true);
  driveCM(W);
  setPump(false);
  turnLeft(90);
  driveCM(halfL);

  // 3. Centre circle
  reply("STEP:3:Centre circle");
  driveCM(halfL);
  turnRight(90);
  driveCM(halfW - circleR);
  turnLeft(90);
  drawCircle(circleR);
  turnRight(90);
  driveCM(halfW - circleR);
  turnLeft(90);

  // 4. Left penalty area
  reply("STEP:4:Left penalty area");
  driveCM(penaltyL);
  turnLeft(90);
  driveCM((W - penaltyW) / 2);
  turnLeft(90);
  setPump(true);
  driveCM(penaltyL); turnRight(90);
  driveCM(penaltyW); turnRight(90);
  driveCM(penaltyL);
  setPump(false);
  turnLeft(90);
  driveCM(penaltyW);
  turnLeft(90);
  driveCM(penaltyL);
  turnRight(90);
  driveCM((W - penaltyW) / 2);
  turnRight(90);

  // 5. Left goal area
  reply("STEP:5:Left goal area");
  driveCM(goalL);
  turnLeft(90);
  driveCM((W - goalW) / 2);
  turnLeft(90);
  setPump(true);
  driveCM(goalL); turnRight(90);
  driveCM(goalW); turnRight(90);
  driveCM(goalL);
  setPump(false);
  turnLeft(90);
  driveCM(goalW);
  turnLeft(90);
  driveCM(goalL);
  turnRight(90);
  driveCM((W - goalW) / 2);
  turnRight(90);

  // 6. Right penalty area
  reply("STEP:6:Right penalty area");
  driveCM(L - penaltyL);
  turnLeft(90);
  driveCM((W - penaltyW) / 2);
  turnRight(90);
  setPump(true);
  driveCM(penaltyL); turnRight(90);
  driveCM(penaltyW); turnRight(90);
  driveCM(penaltyL);
  setPump(false);
  turnLeft(90);
  driveCM(penaltyW);
  turnLeft(90);
  driveCM(penaltyL);
  turnRight(90);
  driveCM((W - penaltyW) / 2);
  turnLeft(90);
  driveCM(L - penaltyL);

  // 7. Right goal area
  reply("STEP:7:Right goal area");
  driveCM(L - goalL);
  turnLeft(90);
  driveCM((W - goalW) / 2);
  turnRight(90);
  setPump(true);
  driveCM(goalL); turnRight(90);
  driveCM(goalW); turnRight(90);
  driveCM(goalL);
  setPump(false);
  turnLeft(90);
  driveCM(goalW);
  turnLeft(90);
  driveCM(goalL);
  turnRight(90);
  driveCM((W - goalW) / 2);
  turnLeft(90);
  driveCM(L - goalL);

  reply("DONE:FOOTBALL_FIELD");
  Serial.println("Field finished.");
  beep(3, 200);
  digitalWrite(LED_GREEN, HIGH);
  delay(2000);
  digitalWrite(LED_GREEN, LOW);
}

// --- Ultrasonic sensor ------------------------------------------------------------
// Returns the distance in cm, or 999 when nothing is echoed.
long readUltrasonic() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

void scanSurroundings() {
  String result = "SCAN:";
  for (int angle = 0; angle <= 180; angle += 30) {
    servo.write(angle);
    delay(300);
    long d = readUltrasonic();
    result += String(angle) + "=" + String(d) + ",";
    Serial.println("SCAN " + String(angle) + " deg: " + String(d) + " cm");
  }
  servo.write(90);
  reply(result);
}

// --- Pump and paint level -------------------------------------------------------
void setPump(bool on) {
  if (on) {
    int level = readPaintLevel();
    if (level <= PAINT_EMPTY_PCT) {
      paintEmptyAlert();
      return;
    }
    if (level <= PAINT_LOW_PCT) {
      reply("ALERT:PAINT_LOW:" + String(level));
    }
  }
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
  pumpActive = on;
}

// Average of 5 ADC readings, mapped to 0-100 %.
int readPaintLevel() {
  long sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += analogRead(LEVEL_PIN);
    delay(2);
  }
  int value = sum / 5;
  return map(value, 0, 4095, 0, 100);
}

// --- Alerts -------------------------------------------------------------------
void obstacleAlert() {
  stopMotors();
  setPump(false);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  Serial.println("Obstacle detected, robot stopped");
  beep(5, 100);
  reply("ALERT:OBSTACLE");
  delay(500);
  digitalWrite(LED_RED, LOW);
}

void paintEmptyAlert() {
  stopMotors();
  digitalWrite(PUMP_PIN, LOW);
  pumpActive = false;
  digitalWrite(LED_RED, HIGH);
  Serial.println("Paint tank empty");
  beep(10, 100);
  reply("ALERT:PAINT_EMPTY");
}

void beep(int count, int duration) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    delay(duration);
  }
}
