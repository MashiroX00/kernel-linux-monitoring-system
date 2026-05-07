#include <Arduino.h>

const int ledPin = 2;

// ใช้ volatile เพื่อบอก Compiler ว่าตัวแปรนี้อาจถูกเปลี่ยนค่าโดย Task อื่นได้ตลอดเวลา
volatile int cpuLoad = 0;
volatile int blinkInterval = 1000; 

void setup() {
  // 1. ลด CPU Frequency ลงเหลือ 80MHz เพื่อประหยัดพลังงานและลดความร้อน
  setCpuFrequencyMhz(80); 

  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // 2. สร้าง FreeRTOS Tasks
  // xTaskCreate(ฟังก์ชัน, ชื่อ, ขนาด Stack, พารามิเตอร์, Priority, ตัวจัดการ)
  xTaskCreate(taskSerial, "SerialTask", 2048, NULL, 1, NULL);
  xTaskCreate(taskLED, "LEDTask", 1024, NULL, 1, NULL);

  delay(500); 
  Serial.println("ESP32 Monitoring Device");
}

void loop() {
  // 3. คืน Memory ให้ระบบ เนื่องจากเราแยกงานไปให้ FreeRTOS หมดแล้ว
  vTaskDelete(NULL); 
}
void taskSerial(void *pvParameters) {
  char buffer[32]; // ใช้ char array แทน String เพื่อป้องกัน Heap Fragmentation
  int index = 0;

  while (true) {
    if (Serial.available() > 0) {
      char c = Serial.read();
      
      // เมื่อเจอตัวจบบรรทัด (Enter)
      if (c == '\n' || c == '\r') {
        buffer[index] = '\0'; // ปิดสตริง
        
        if (index > 0) {
          if (strcmp(buffer, "ACK") == 0) {
            Serial.println("ESP32 Monitoring Device");
          } else {
            int val = atoi(buffer); // แปลง char array เป็น integer ทันที
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            cpuLoad = val;

            // คำนวณ Interval แค่ตอนที่ค่าเปลี่ยน ไม่ต้องคำนวณใหม่ทุกรอบ
            if (cpuLoad > 0 && cpuLoad < 100) {
              blinkInterval = map(cpuLoad, 1, 99, 1000, 50);
            }
          }
          index = 0; // เคลียร์ buffer เริ่มใหม่
        }
      } else if (index < 31) {
        buffer[index++] = c;
      }
    }
    // Yield ให้ CPU ไปทำงานอื่น ป้องกัน Watchdog Timer เด้ง
    vTaskDelay(pdMS_TO_TICKS(10)); 
  }
}


void taskLED(void *pvParameters) {
  bool ledState = false;

  while (true) {
    if (cpuLoad == 0) {
      if (ledState) {
        ledState = false;
        digitalWrite(ledPin, LOW);
      }
      vTaskDelay(pdMS_TO_TICKS(100)); // เช็คสถานะทุกๆ 100ms
    } 
    else if (cpuLoad >= 100) {
      if (!ledState) {
        ledState = true;
        digitalWrite(ledPin, HIGH);
      }
      vTaskDelay(pdMS_TO_TICKS(100)); // เช็คสถานะทุกๆ 100ms
    } 
    else {
      // สลับสถานะ LED และหน่วงเวลาตาม Interval
      ledState = !ledState;
      digitalWrite(ledPin, ledState);
      
      // vTaskDelay จะหยุดการทำงานของ Task นี้โดยสมบูรณ์ตามเวลาที่กำหนด CPU ไม่ต้องเหนื่อยฟรี
      vTaskDelay(pdMS_TO_TICKS(blinkInterval)); 
    }
  }
}