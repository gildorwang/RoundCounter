#include <SPI.h>
// Install the Bounce2 library
#include <Bounce2.h>
#include <Wire.h>
// Install the LiquidCrystal_I2C library https://github.com/johnrickman/LiquidCrystal_I2C
#include <LiquidCrystal_I2C.h>
// For rotary encoder with button
// Install from https://github.com/0xPIT/encoder
#include <ClickEncoder.h>
// Install TimerOne library
#include <TimerOne.h>

#define VERSION "1.0.0"

#define RELOAD_SENSOR_PIN 2

// LCD wiring:
// - VCC: 5V
// - GND: GND
// - SDA: A4
// - SCL: A5
LiquidCrystal_I2C _lcd(0x3F, 16, 2); // set the LCD address to 0x3F for a 16 chars and 2 line display

// LCD backlight on duration
const unsigned long LcdOnDurationMillis = 10000;
// Boot finished time
unsigned long _bootingEndMillis = 0;
// Scheduled LCD backlight off time
unsigned long _lcdOffMillis = 0;

// State of the system
enum State {
    booting,
    idle,
    reloading,
    paused
};

State _state;

// Reader for the reload sensor
Bounce _reloadSensor = Bounce();

// Click encoder - the main input control
ClickEncoder *_encoder;

// state variables
long _count = 0;
long _target = 0;
unsigned long _durationMillis = 0;
unsigned long _lastReloadMillis = 0;

void setup() {
    Serial.begin(115200);
    setState(booting);
    // initialize the lcd
    _lcd.init();
    msg("Made by Ken", "Version " VERSION); // Print a message to the LCD.

    setupEncoder();
    
    _reloadSensor.attach(RELOAD_SENSOR_PIN, INPUT_PULLUP);
    _reloadSensor.interval(5);

    _bootingEndMillis = millis() + 3000;
}

void loop()
{
    if (_lcdOffMillis != 0 && millis() > _lcdOffMillis && _state != reloading) {
        _lcd.noBacklight();
        Serial.println("Backlight OFF");
        _lcdOffMillis = 0;
    }

    switch (_state) {
        case booting:
            if (millis() >= _bootingEndMillis) {
                setState(idle);
                updateIdleLcd();
                return;
            }
            break;
        case idle: {
            long newTarget = _target + _encoder->getValue();
            if (newTarget < 0) {
                newTarget = 0;
            }
            if (newTarget >= 10000) {
                newTarget = 9999;
            }
            if (newTarget != _target) {
                _target = newTarget;
                updateIdleLcd();
            }
            if (isReloadSensorTriggered()) {
                setState(reloading);
                _count = 1;
                _durationMillis = 0;
                _lastReloadMillis = millis();
                updateReloadingOrPausedLcd();
                return;
            }
            // TODO: reboot when millis() is halfway to overflow
            break;
        }
        case reloading: {
            if (isReloadSensorTriggered()) {
                ++_count;
                if (_lastReloadMillis != 0) {
                    _durationMillis += millis() - _lastReloadMillis;
                }
                _lastReloadMillis = millis();
                updateReloadingOrPausedLcd();
            }
            ClickEncoder::Button encoderButton = _encoder->getButton();
            switch (encoderButton) {
                case ClickEncoder::Clicked:
                    setState(paused);
                    updateReloadingOrPausedLcd();
                    return;
                case ClickEncoder::Released:
                    setState(idle);
                    updateIdleLcd();
                    return;
            }
            break;
        }
        case paused: {
            int newCount = _count + _encoder->getValue();
            if (newCount < 0) {
                newCount = 0;
            }
            if (newCount > 9999) {
                newCount = 9999;
            }
            if (newCount != _count) {
                _count = newCount;
                updateReloadingOrPausedLcd();
            }
            ClickEncoder::Button encoderButton = _encoder->getButton();
            switch (encoderButton) {
                case ClickEncoder::Clicked:
                    setState(reloading);
                    // clear last reload timestamp so it won't start counting time until one reload op
                    _lastReloadMillis = 0;
                    updateReloadingOrPausedLcd();
                    return;
                case ClickEncoder::Released:
                    setState(idle);
                    updateIdleLcd();
                    return;
            }
            break;
        }
    }
}

void timerIsr() {
    // const short BlinkPeriodInMs = 2000;
    // const short DelayBeforeBlinkInMs = 1000;

    // unsigned long now = millis();
    // if (_state == SETTING && now % BlinkPeriodInMs < (BlinkPeriodInMs / 2) && now - _minutesLastUpdatedByUserMillis > DelayBeforeBlinkInMs) {
    //     _sevseg.blank();
    // } else {
    //     displayMinutes();
    // }
    // Run the encoder service. It needs to be run in the timer
    _encoder->service();
}

void updateIdleLcd() {
    char line1[17];
    sprintf(line1, "TARGET: %ld", _target);
    msg(line1, "RELOAD TO START");
}

void updateReloadingOrPausedLcd() {
    unsigned long totalSeconds = _durationMillis / 1000;
    unsigned int hours = totalSeconds / 60 / 60;
    unsigned int minutes = totalSeconds / 60 % 60;
    unsigned int seconds = totalSeconds % 60;

    int roundsPerHour = totalSeconds == 0 ? 0 : _count * 3600 / totalSeconds;

    if (roundsPerHour >= 10000) {
        roundsPerHour = 9999;
    }

    char line2[17];
    sprintf(line2, "%02u:%02u:%02u %4d/HR", hours, minutes, seconds, roundsPerHour);

    char line1[17];
    Serial.print("_target=");
    Serial.println(_target);
    if (_target <= 0) {
        sprintf(line1, _state == reloading ? "%ld ROUNDS" : "%ld PAUSED", _count);
    } else {
        sprintf(line1, _state == reloading ? "%ld/%ld ROUNDS" : "%ld/%ld PAUSED", _count, _target);
    }
    msg(line1, line2);
}

int isReloadSensorTriggered() {
    _reloadSensor.update();
    int reloadSensorValue = _reloadSensor.fell();
    if (reloadSensorValue) {
        Serial.print("Reload sensor: ");
        Serial.println(reloadSensorValue);
    }
    return reloadSensorValue;
}

void setupEncoder() {
    _encoder = new ClickEncoder(A1, A0, A2);
    Timer1.initialize(1000);
    Timer1.attachInterrupt(timerIsr);
}

void setState(State newState) {
    Serial.print("State=");
    switch(newState) {
        case booting:
            Serial.println("booting");
            break;
        case idle:
            Serial.println("idle");
            break;
        case reloading:
            Serial.println("reloading");
            break;
        case paused:
            Serial.println("paused");
            break;
    }
    _state = newState;
}

void lcdlight() {
    _lcd.backlight();
    _lcdOffMillis = millis() + LcdOnDurationMillis;
}

void msg(const char line1[], const char line2[]) {
    Serial.println(line1);
    Serial.println(line2);
    _lcd.clear();
    lcdlight();
    _lcd.print(line1);
    _lcd.setCursor(0,1); //newline
    _lcd.print(line2);
}
