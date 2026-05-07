const int ledPin = 2;
int cpuLoad = 0;
unsigned long prevMillis = 0;
bool ledState = false;

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  
  // ส่ง handshake ทันทีที่บูต ให้ Linux driver detect ได้
  delay(500); // รอให้ Serial พร้อม
  Serial.println("ESP32 Monitoring Device");
}

void loop() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    // รับ handshake ตอบกลับจาก Linux
    if (input == "ACK") {
      Serial.println("ESP32 Monitoring Device"); // ยืนยันตัวตนอีกครั้ง
      return;
    }

    cpuLoad = input.toInt();
    if (cpuLoad < 0)   cpuLoad = 0;
    if (cpuLoad > 100) cpuLoad = 100;
  }

  if (cpuLoad == 0) {
    digitalWrite(ledPin, LOW);
  } else if (cpuLoad >= 100) {
    digitalWrite(ledPin, HIGH);
  } else {
    int interval = map(cpuLoad, 1, 99, 1000, 50);
    unsigned long currentMillis = millis();
    if (currentMillis - prevMillis >= (unsigned long)interval) {
      prevMillis = currentMillis;
      ledState = !ledState;
      digitalWrite(ledPin, ledState);
    }
  }
}