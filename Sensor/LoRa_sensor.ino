#include <Arduino.h>
#include <SoftwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>
#include <Preferences.h> // CHANGE: persistent sensor ID storage

// #define LoRaConfig 1

#define WDT_TIMEOUT 19
esp_err_t ESP32_ERROR;

const int oneWireBus = 32;
OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);

#define LRx 14
#define LTx 12
SoftwareSerial LoRa(LRx, LTx);
int LoRaPowerStatus;

// CHANGE: BOOT and LED pins can be changed if the ESP32 board uses different connections
#ifndef BOOT_PIN
#define BOOT_PIN 0
#endif
#ifndef LED_PIN
#define LED_PIN 2
#endif

// CHANGE: protocol and scheduling parameters
// Frame magic = two raw bytes 0x02 0xE3 (binary, not ASCII text).
const uint8_t FRAME_MAGIC[2] = { 0x02, 0xE3 };
const uint8_t FACTORY_ID = 0;
const uint8_t CONTROLLER_ID = 9; // CHANGE: per the protocol example "X2T9#"
const uint32_t MEASUREMENT_INTERVAL_MS = 20000UL;
const uint32_t RANDOM_DELAY_MAX_MS = 2000UL;
const uint32_t ACK_TIMEOUT_MS = 2000UL;
const uint32_t NO_ACK_REBOOT_MS = 120000UL; // CHANGE: periodic reboot after 2 minutes without acknowledgement
const uint32_t BOOT_HOLD_MS = 5000UL;
const int STANDARD_POWER = 1;
const int HIGH_POWER = 3; // CHANGE: power level 2 dB above standard, using the available module commands

Preferences preferences; // CHANGE
uint8_t nodeId = FACTORY_ID; // CHANGE
uint32_t nextMeasurementAt = 0; // CHANGE
uint32_t lastSendAt = 0; // CHANGE
uint32_t lastAckAt = 0; // CHANGE
bool awaitingAck = false; // CHANGE
bool bootResetHandled = false; // CHANGE

// CHANGE: receive buffer for processing multiple frames present in the LoRa module buffer
const size_t RX_BUFFER_SIZE = 96;
char rxBuffer[RX_BUFFER_SIZE];
size_t rxLength = 0;

void LoRaSendStr(const char *s) { // CHANGE: const parameter and no unnecessary strlen call in the loop
  Serial.print("Sending: ");
  Serial.println(s);
  LoRa.print(s);
}

// CHANGE: send a complete frame: <magic bytes><payload>T<ID>#
void LoRaSendFrame(const char *payload) {
  char frame[40];
  size_t n = 0;
  frame[n++] = (char)FRAME_MAGIC[0];
  frame[n++] = (char)FRAME_MAGIC[1];
  n += (size_t)snprintf(frame + n, sizeof(frame) - n - 1, "%sT%u#", payload, nodeId);
  Serial.print("Frame: ");
  Serial.printf("\\x%02X\\x%02X", (uint8_t)FRAME_MAGIC[0], (uint8_t)FRAME_MAGIC[1]);
  Serial.print(payload);
  Serial.print("T");
  Serial.print(nodeId);
  Serial.println("#");
  LoRa.write((const uint8_t *)frame, n);
}

void saveNodeId(uint8_t id) { // CHANGE
  preferences.putUChar("nodeId", id);
  nodeId = id;
}

void loadNodeId() { // CHANGE
  preferences.begin("sensor", false);
  nodeId = preferences.getUChar("nodeId", FACTORY_ID);
  if (nodeId > 8) {
    nodeId = FACTORY_ID;
    preferences.putUChar("nodeId", nodeId);
  }
}

void LoRaPower(int power) {
  Serial.println("LoRaPower: power change");
  digitalWrite(25, HIGH);
  digitalWrite(26, HIGH);
  delay(200);

  if (power == HIGH_POWER) {
    LoRaSendStr("AT+POWER=3");
    LoRaPowerStatus = HIGH_POWER;
  } else {
    LoRaSendStr("AT+POWER=1");
    LoRaPowerStatus = STANDARD_POWER;
  }

  delay(500);
  LoRaSendStr("AT+RESET");
  delay(3000);
  digitalWrite(25, LOW);
  digitalWrite(26, LOW);
}

void scheduleNextMeasurement() { // CHANGE
  nextMeasurementAt = millis() + MEASUREMENT_INTERVAL_MS + random(0, RANDOM_DELAY_MAX_MS + 1);
}

void sendMeasurement() { // CHANGE
  sensors.requestTemperatures();
  float temperatureC = sensors.getTempCByIndex(0);

  char payload[24];
  // CHANGE: A DS18B20 error (-127 = no device / CRC error) is reported as
  // the reserved "ERR" payload so the controller never treats it as a real
  // temperature.
  if (temperatureC <= -126.0f) {
    snprintf(payload, sizeof(payload), "ERR");
  } else {
    snprintf(payload, sizeof(payload), "%02.3f", temperatureC);
  }
  Serial.print("Temperature: ");
  Serial.println(temperatureC, 3);
  LoRaSendFrame(payload); // CHANGE: magic as raw bytes 0x02 0xE3

  lastSendAt = millis();
  awaitingAck = true;
  scheduleNextMeasurement();
}

bool isFrameComplete() { // CHANGE
  return rxLength > 0 && rxBuffer[rxLength - 1] == '#';
}

// CHANGE: frame = <0x02 0xE3><payload>T<ID>#. Magic is matched as raw bytes.
bool parseFrame(const char *frame, size_t length, char *payload, size_t payloadSize, uint8_t *senderId) {
  if (length < 6 || length >= RX_BUFFER_SIZE) return false;  // min: magic(2)+payload(1)+T+id(1)+#
  if ((uint8_t)frame[0] != FRAME_MAGIC[0] || (uint8_t)frame[1] != FRAME_MAGIC[1]) return false;
  if (frame[length - 1] != '#') return false;

  const char *t = strrchr(frame, 'T');
  if (t == nullptr || t <= frame + 2) return false;

  char idText[4] = {0};
  size_t idLength = length - (size_t)(t - frame) - 2;
  if (idLength == 0 || idLength >= sizeof(idText)) return false;
  memcpy(idText, t + 1, idLength);
  *senderId = (uint8_t)atoi(idText);
  if (*senderId > 99) return false;

  size_t payloadLength = (size_t)(t - (frame + 2));
  if (payloadLength == 0 || payloadLength >= payloadSize) return false;
  memcpy(payload, frame + 2, payloadLength);
  payload[payloadLength] = '\0';
  return true;
}

void handleFrame(const char *frame, size_t length) { // CHANGE
  char payload[24];
  uint8_t senderId;

  if (!parseFrame(frame, length, payload, sizeof(payload), &senderId)) {
    Serial.print("Ignoring invalid frame: ");
    for (size_t i = 0; i < length && i < 40; i++) Serial.printf("%02X ", (uint8_t)frame[i]);
    Serial.println();
    return;
  }

  // CHANGE: accept an acknowledgement only for this sensor and after a measurement was sent
  char expectedAck[8];
  snprintf(expectedAck, sizeof(expectedAck), "X%u", nodeId);
  if (awaitingAck && senderId == CONTROLLER_ID && strcmp(payload, expectedAck) == 0) {
    awaitingAck = false;
    lastAckAt = millis();
    if (LoRaPowerStatus != STANDARD_POWER) {
      LoRaPower(STANDARD_POWER);
    }
    Serial.println("Temperature measurement acknowledged");
    return;
  }

  // CHANGE: pairing is allowed only for a sensor with factory ID 0
  if (nodeId == FACTORY_ID && senderId == CONTROLLER_ID && strncmp(payload, "PR", 2) == 0) {
    int requestedId = atoi(payload + 2);
    if (requestedId >= 1 && requestedId <= 8) {
      saveNodeId((uint8_t)requestedId);
      Serial.print("Paired, new ID: ");
      Serial.println(nodeId);
    }
  }
}

void readLoRaFrames() { // CHANGE
  while (LoRa.available()) {
    char c = (char)LoRa.read();
    Serial.write(c);

    if (rxLength >= RX_BUFFER_SIZE - 1) {
      // CHANGE: buffer full with no '#' terminator — a malformed frame. Drop
      // everything except the tail of a possibly restarted frame (magic),
      // so the next valid frame is not swallowed by garbage.
      size_t resync = 0;
      for (size_t i = 0; i + 1 < rxLength; i++) {
        if ((uint8_t)rxBuffer[i] == FRAME_MAGIC[0] && (uint8_t)rxBuffer[i + 1] == FRAME_MAGIC[1]) { resync = i; break; }
      }
      if (resync > 0) {
        size_t keep = rxLength - resync;
        memmove(rxBuffer, rxBuffer + resync, keep);
        rxLength = keep;
      } else {
        rxLength = 0;
      }
      rxBuffer[rxLength] = '\0';
    }

    rxBuffer[rxLength++] = c;
    rxBuffer[rxLength] = '\0';

    if (c == '#') {
      handleFrame(rxBuffer, rxLength);
      rxLength = 0;
    }
  }
}

void handleBootButton() { // CHANGE
  static uint32_t pressedSince = 0;
  bool pressed = digitalRead(BOOT_PIN) == LOW;

  if (pressed && pressedSince == 0) {
    pressedSince = millis();
  }

  if (!pressed) {
    pressedSince = 0;
    bootResetHandled = false;
    digitalWrite(LED_PIN, LOW);
    return;
  }

  if (!bootResetHandled && millis() - pressedSince >= BOOT_HOLD_MS) {
    digitalWrite(LED_PIN, HIGH);
    saveNodeId(FACTORY_ID);
    bootResetHandled = true;
    Serial.println("Factory ID reset to 0");
  }
}

void setup() {
  byte RxData;
  int pinM = 0;

#ifdef LoRaConfig
  Serial.begin(9600);
  pinM = 1;
#else
  Serial.begin(115200);
#endif

  delay(100);
  Serial.println("Starting sensor");

  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
#ifndef LoRaConfig
  ESP32_ERROR = esp_task_wdt_init(&wdt_config);
  Serial.print("WDT: ");
  Serial.println(esp_err_to_name(ESP32_ERROR));
  esp_task_wdt_add(NULL);
#endif

  pinMode(BOOT_PIN, INPUT_PULLUP); // CHANGE
  pinMode(LED_PIN, OUTPUT); // CHANGE
  digitalWrite(LED_PIN, LOW); // CHANGE
  pinMode(25, OUTPUT);
  pinMode(26, OUTPUT);
  digitalWrite(25, HIGH);
  digitalWrite(26, HIGH);

  loadNodeId(); // CHANGE
  randomSeed((uint32_t)esp_random()); // CHANGE
  sensors.begin();
  LoRa.begin(9600);

  LoRaSendStr("AT+POWER=3");
  delay(200);
  LoRaSendStr("AT+RESET");
  delay(4000);
  digitalWrite(25, pinM);
  digitalWrite(26, pinM);
  LoRaPowerStatus = HIGH_POWER;

  scheduleNextMeasurement(); // CHANGE
  lastAckAt = millis(); // CHANGE
  Serial.print("Ready, ID: ");
  Serial.println(nodeId);

#ifdef LoRaConfig
  while (true) {
    if (Serial.available()) {
      RxData = Serial.read();
      LoRa.write(RxData);
    }
    if (LoRa.available()) {
      RxData = LoRa.read();
      Serial.write(RxData);
    }
  }
#endif
}

void loop() {
  esp_task_wdt_reset();
  handleBootButton(); // CHANGE
  readLoRaFrames(); // CHANGE

  uint32_t now = millis();

  if (!awaitingAck && (int32_t)(now - nextMeasurementAt) >= 0) { // CHANGE
    sendMeasurement();
    now = millis(); // FIX: sendMeasurement() sets lastSendAt=millis(); refresh now so the timeout check below cannot underflow
  }

  // CHANGE: increase power after 2 seconds without acknowledgement; the
  // measurement slot is freed so the 20 s cadence is never blocked (an ACK
  // arriving later still lowers the power). Reboot is based on lastAckAt
  // alone, so it also fires when frames stopped being sent entirely.
  if (awaitingAck && (int32_t)(now - lastSendAt) >= (int32_t)ACK_TIMEOUT_MS) { // FIX: signed compare, stale-now proof
    if (LoRaPowerStatus != HIGH_POWER) {
      LoRaPower(HIGH_POWER);
    }
    awaitingAck = false; // CHANGE: do not stall the measurement loop
  }

  if (now - lastAckAt >= NO_ACK_REBOOT_MS) { // CHANGE
    Serial.println("No acknowledgement for too long, restarting");
    delay(100);
    ESP.restart();
  }

  delay(2);
}
