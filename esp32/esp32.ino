#include <Arduino.h>

// ==============================
// Pin Configuration (กำหนดเองได้)
// ==============================
const int ledPin  = 27;   // LED 1 (ไม่ได้ใช้ใน logic นี้)
const int led2Pin = 33;   // LED 2
const int led3Pin = 32;   // LED 3

// ==============================
// Shared State (volatile สำหรับ Multi-task)
// ==============================
volatile int cpuLoad      = 0;
volatile int blinkInterval = 1000;


// ==============================
// Connection State
//===============================
volatile bool isConnected = false;
volatile unsigned long lastReceived = 0;
const unsigned long TIMEOUT_MS = 5000;

// ==============================
// Setup
// ==============================
void setup() {
  setCpuFrequencyMhz(80);

  Serial.begin(115200);

  pinMode(ledPin,  OUTPUT); digitalWrite(ledPin,  LOW);
  pinMode(led2Pin, OUTPUT); digitalWrite(led2Pin, LOW);
  pinMode(led3Pin, OUTPUT); digitalWrite(led3Pin, LOW);

  xTaskCreate(taskSerial, "SerialTask", 2048, NULL, 1, NULL);
  xTaskCreate(taskLED,    "LEDTask",    1024, NULL, 1, NULL);

  delay(500);
  Serial.println("ESP32 Monitoring Device");
}

void loop() {
  vTaskDelete(NULL);
}

// ==============================
// Task: Serial Input
// ==============================
void taskSerial(void *pvParameters) {
  char buffer[32];
  int index = 0;

  while (true) {
    if (Serial.available() > 0) {
      char c = Serial.read();

      if (c == '\n' || c == '\r') {
        buffer[index] = '\0';

        if (index > 0) {
          if (strcmp(buffer, "ACK") == 0) {
            Serial.println("ESP32 Monitoring Device");
            isConnected = true;
          } else {
            int val = atoi(buffer);
            if (val < 0)   val = 0;
            if (val > 100) val = 100;
            cpuLoad = val;
            lastReceived = millis();
            isConnected = true;
            // คำนวณ interval เฉพาะตอนค่าเปลี่ยน
            if (cpuLoad > 0 && cpuLoad < 100) {
              blinkInterval = map(cpuLoad, 1, 99, 1000, 50);
            }
            
          }
          index = 0;
        }
      } else if (index < 31) {
        buffer[index++] = c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==============================
// Task: LED Control
// ==============================
void taskLED(void *pvParameters) {
  bool led2State = false;
  bool led3State = false;

  while (true) {
    if (!isConnected || (millis() - lastReceived > TIMEOUT_MS)) {
    isConnected = false;
    digitalWrite(led2Pin, LOW);
    digitalWrite(led3Pin, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    continue; // ข้ามรอบนี้
    }
    if (isConnected) {
      digitalWrite(ledPin,HIGH);
      vTaskDelay(pdMS_TO_TICKS(100));
    }else {
      digitalWrite(ledPin,LOW);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    int load = cpuLoad; // อ่านครั้งเดียวต่อรอบ ป้องกัน race condition

    if (load == 0) {
      // ดับทั้งคู่
      digitalWrite(led2Pin, LOW);
      digitalWrite(led3Pin, LOW);
      led2State = false;
      led3State = false;
      vTaskDelay(pdMS_TO_TICKS(100));

    } else if (load >= 1 && load <= 49) {
      // LED2 กระพริบ, LED3 ดับ
      digitalWrite(led3Pin, LOW);
      led3State = false;

      led2State = !led2State;
      digitalWrite(led2Pin, led2State);
      vTaskDelay(pdMS_TO_TICKS(blinkInterval));

    } else if (load >= 50 && load <= 99) {
      // LED2 ติดค้าง, LED3 กระพริบ
      digitalWrite(led2Pin, HIGH);
      led2State = true;

      led3State = !led3State;
      digitalWrite(led3Pin, led3State);
      vTaskDelay(pdMS_TO_TICKS(blinkInterval));

    } else if (load >= 100) {
      // ติดค้างทั้งคู่
      digitalWrite(led2Pin, HIGH);
      digitalWrite(led3Pin, HIGH);
      led2State = true;
      led3State = true;
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}