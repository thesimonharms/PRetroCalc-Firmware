#include <Arduino.h>

extern "C" void pretro_main(void);

void setup() {
    Serial.begin(115200);
    delay(200);
    pretro_main();
}

void loop() {}
