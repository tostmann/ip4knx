// Serial + Blinky test for ESP32-C6-MINI-1-N4 (TUL32)
// USB CDC Serial + LED on pin 8

#include <Arduino.h>

#define LED_PIN 8

void setup() {
    // Try Serial.begin() for USB-CDC
    Serial.begin(115200);
    Serial.setTimeout(1000);
    delay(500); // Give USB-CDC time to initialize

    Serial.println("Hello from ESP32-C6!");
    Serial.print("Free heap: ");
    Serial.println(esp_get_free_heap_size());

    pinMode(LED_PIN, OUTPUT);
}

void loop() {
    // Double blink pattern: on-off-on-off - pause - repeat
    digitalWrite(LED_PIN, HIGH);
    Serial.println("LED ON");
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    Serial.println("LED ON");
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
    delay(800); // Pause before next double blink
}
