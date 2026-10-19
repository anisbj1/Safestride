#include <Arduino.h>

#include "app/app.h"

void setup() {
    app::run();
}

void loop() {
    // All work happens in FreeRTOS tasks created by app::run().
}
