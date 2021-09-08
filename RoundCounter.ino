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
// To read/write to EEPROM
#include <EEPROM.h>

#define VERSION "1.0.0"

// Connects to Reed switch or Hall sensor
#define RELOAD_SENSOR_PIN 2
// A1 and A0 for the rotary encoder and A2 for the push button connection
#define CLICK_ENCODER_PINS A1, A0, A2
// Buzzer
#define BUZZER_PIN 8

// LCD wiring:
// - VCC: 5V
// - GND: GND
// - SDA: A4
// - SCL: A5
LiquidCrystal_I2C _lcd(0x3F, 16, 2); // set the LCD address to 0x3F for a 16 chars and 2 line display

// LCD backlight on duration
const unsigned long LcdOnDurationMillis = 10000;
const unsigned long LcdFlashDurationMillis = 500;
// Maximum supported count (due to LCD size)
const unsigned long MaxCount = 9999;
// Maximum supported total count (accumulated over time)
const unsigned long MaxTotalCount = 999999;
// The EEPROM address to store total count
const unsigned long TotalCountRomAddress = 1;
// Indicator if EEPROM has been initialized
const unsigned long InitFlagRomAddress = 0;
const unsigned int BuzzerFrequency = 3000;
// Boot finished time
unsigned long _bootingEndMillis = 0;
// LCD backlight on/off time
unsigned long _lcdLastSetMillis = 0;

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
bool _isTargetReached = false;
bool _isBacklightOn = false;
bool _showTotalCount = false;

void setup() {
    Serial.begin(115200);
    setState(booting);

    _lcd.init();
    msg("Made by Ken", "Version " VERSION); // Print a message to the LCD.
    tone(BUZZER_PIN, BuzzerFrequency, 500);

    setupEncoder();
    
    _reloadSensor.attach(RELOAD_SENSOR_PIN, INPUT_PULLUP);
    _reloadSensor.interval(5);

    pinMode(BUZZER_PIN, OUTPUT);

    if (EEPROM.read(InitFlagRomAddress) != 'X') {
        Serial.println("Init EEPROM");
        writeTotalCount(0);
        EEPROM.write(InitFlagRomAddress, 'X');
    }

    _bootingEndMillis = millis() + 3000;
}

void loop() {
    processLcdBacklight();

    // Run states
    switch (_state) {
        case booting:
            bootingState();
            break;
        case idle:
            idleState();
            break;
        case reloading:
            reloadingState();
            break;
        case paused:
            pausedState();
            break;
    }
}

void processLcdBacklight() {
    // Process LCD flashing and backlight delayed off
    if (_isTargetReached && _lcdLastSetMillis != 0 && millis() - _lcdLastSetMillis >= LcdFlashDurationMillis) {
        _isBacklightOn = !_isBacklightOn;
        _lcd.setBacklight(_isBacklightOn);
        _lcdLastSetMillis = millis();
    } else if (_lcdLastSetMillis != 0 && millis() - _lcdLastSetMillis >= LcdOnDurationMillis && _state != reloading && _state != paused) {
        _lcd.noBacklight();
        _isBacklightOn = false;
        _lcdLastSetMillis = 0;
    }
}

void bootingState() {
    ClickEncoder::Button encoderButton = _encoder->getButton();
    if (encoderButton == ClickEncoder::Held && readTotalCount() != 0) {
        writeTotalCount(0);
        msg("TOTAL ROUNDS", "CLEARED");
    }
    if (millis() >= _bootingEndMillis) {
        setState(idle);
        updateIdleLcd();
    }
}

void idleState() {
    setIsTargetReached(false);
    long newTarget = _target + _encoder->getValue();
    if (newTarget < 0) {
        newTarget = 0;
    }
    if (newTarget > MaxCount) {
        newTarget = MaxCount;
    }
    if (newTarget != _target) {
        _target = newTarget;
        updateIdleLcd();
    }
    ClickEncoder::Button encoderButton = _encoder->getButton();
    switch (encoderButton) {
        case ClickEncoder::Clicked:
            _showTotalCount = !_showTotalCount;
            updateIdleLcd();
            break;
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
}

void reloadingState() {
    setIsTargetReached(_count >= _target && _target != 0);
    if (isReloadSensorTriggered()) {
        ++_count;
        if (_count > MaxCount) {
            _count = MaxCount;
        }
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
            break;
        case ClickEncoder::Released:
            setState(idle);
            updateIdleLcd();
            break;
    }
}

void pausedState() {
    setIsTargetReached(false);
    int newCount = _count + _encoder->getValue();
    if (newCount < 0) {
        newCount = 0;
    }
    if (newCount > MaxCount) {
        newCount = MaxCount;
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
}

void timerIsr() {
    // Run the encoder service. It needs to be run in the timer
    _encoder->service();
}

void setIsTargetReached(bool isTargetReached) {
    _isTargetReached = isTargetReached;
    if (isTargetReached) {
        tone(BUZZER_PIN, BuzzerFrequency);
    } else {
        noTone(BUZZER_PIN);
    }
}

void updateIdleLcd() {
    char line1[17];
    sprintf(line1, "TARGET: %ld", _target);
    char line2[17];
    if (_showTotalCount) {
        sprintf(line2, "TOTAL %ld RDS", readTotalCount());
    } else {
        sprintf(line2, "RELOAD TO START");
    }
    msg(line1, line2);
}

void updateReloadingOrPausedLcd() {
    unsigned long totalSeconds = _durationMillis / 1000;
    unsigned int hours = totalSeconds / 60 / 60;
    unsigned int minutes = totalSeconds / 60 % 60;
    unsigned int seconds = totalSeconds % 60;

    int roundsPerHour = totalSeconds == 0 ? 0 : _count * 3600 / totalSeconds;

    if (roundsPerHour > MaxCount) {
        roundsPerHour = MaxCount;
    }

    char line2[17];
    sprintf(line2, "%02u:%02u:%02u %4d/HR", hours, minutes, seconds, roundsPerHour);

    char line1[17];
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
        
        // Write EEPROM for total count
        unsigned long totalCount = readTotalCount() + 1;
        Serial.print("Total count to EEPROM: ");
        Serial.println(totalCount);
        writeTotalCount(totalCount);
    }
    return reloadSensorValue;
}

void setupEncoder() {
    _encoder = new ClickEncoder(CLICK_ENCODER_PINS);
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

unsigned long readTotalCount() {
    unsigned long totalCount;
    EEPROM.get(TotalCountRomAddress, totalCount);
    return totalCount;
}

void writeTotalCount(unsigned long totalCount) {
    EEPROM.put(TotalCountRomAddress, totalCount);
}

void lcdlight() {
    _lcd.backlight();
    _isBacklightOn = true;
    _lcdLastSetMillis = millis();
}

void msg(const char line1[], const char line2[]) {
    Serial.print('[');
    Serial.print(line1);
    Serial.println(']');
    Serial.print('[');
    Serial.print(line2);
    Serial.println(']');
    _lcd.clear();
    lcdlight();
    _lcd.print(line1);
    _lcd.setCursor(0,1); //newline
    _lcd.print(line2);
}
