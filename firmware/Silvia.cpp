// This #include statement was automatically added by the Particle IDE.
#include "cloud_storage.h"

// Non-volatile storage
#include "storage.h"
#include "helpers.h"

// Bubble display
#include "retro_led.h"
#include "ht16k33.h"

// Thermocouple amplifier
#include "MAX6675.h"

// Mirror RGB LED
#include "external_led.h"

// For resetting the I2C bus
#include "i2c_hal.h"

// For direct access to delay
#include "delay_hal.h"

// Precise timer thread timing
#include "periodic_work.h"

// Debounced button
#include "clickButton.h"

SYSTEM_MODE(SEMI_AUTOMATIC);

void startHardwareThread();
void startButtonThread();
void setupCloud();
void setupRelay();
int enterDFU(String arg);
void publishData();
void updateCals();

void processButtonClicks();
void hardwareSetup();
void hardwareLoop();
void resetDisplay();
void toggleSleep();
void processSleep();
void readTemperature();
void controlTemperature();
void commandRelay();

Thread hardwareThread;

const unsigned int PERIOD = 1000; /* ms */

bool temperatureValid = false;
double temperature = 0;
double relayDc = 0;
char cals[64] = "";

MAX6675 thermo(SCK, SS, MISO);

#ifdef USE_SWD_JTAG
// Dummy pins while the debugger is connected
const int RELAY_PIN = A0;
const int RELAY_POWER_PIN = A1;
const int EXTERNAL_LED_PIN = A2;
#else
const int RELAY_PIN = D5;
const int RELAY_POWER_PIN = D6;
const int EXTERNAL_LED_PIN = D3;
#endif

ClickButton sleepButton(D2, LOW, CLICKBTN_PULLUP);
bool gotoSleep = false;
enum ClickButtonStates {
    SINGLE_LONG_CLICK = -1,
    SINGLE_CLICK = 1
};

const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

RetroLED display;

Storage storage;

ExternalLed externalLed(EXTERNAL_LED_PIN);

void startHardwareThread() {
    hardwareThread = Thread("hardware", []() {
        hardwareSetup();

        // 1s periodic task
        PeriodicWork work(PERIOD);
        for(;;) {
            work.start();
            hardwareLoop();
            work.end();
        }
    }, OS_THREAD_PRIORITY_DEFAULT + 1);
}

void startButtonThread() {
    Thread("button", []() {
        // 10ms periodic task
        PeriodicWork work(10);

        for(;;) {
            work.start();
            sleepButton.Update();
            processButtonClicks();
            work.end();
        }
    }, OS_THREAD_PRIORITY_DEFAULT + 1);
}

void processButtonClicks() {
    switch(sleepButton.clicks) {
        case SINGLE_CLICK: gotoSleep = true; break;
        case SINGLE_LONG_CLICK: System.dfu(); break;
    }
}
void hardwareSetup() {
    setupRelay();
    display.begin();
}

void setupRelay() {
    pinMode(RELAY_POWER_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_POWER_PIN, HIGH);
    digitalWrite(RELAY_PIN, RELAY_OFF);
}

void hardwareLoop() {
    resetDisplay();
    if(gotoSleep) {
        toggleSleep();
        gotoSleep = false;
    }

    if(storage.getSleep()) {
        processSleep();
    } else {
        readTemperature();
        controlTemperature();
    }
    commandRelay();
}

void resetDisplay() {
    HAL_I2C_End(HAL_I2C_INTERFACE1, NULL);
    HAL_Delay_Milliseconds(1);
    HAL_I2C_Begin(HAL_I2C_INTERFACE1, I2C_MODE_MASTER, 0x00, NULL);

}

const double temperatureCalibration = -2.0;

void readTemperature() {
    double readTemperature = thermo.readCelsius();
    if(readTemperature == 0.0) {
        temperatureValid = false;
        display.printDashes();
    } else {
        temperatureValid = true;
        temperature = thermo.readCelsius() + temperatureCalibration;
        display.print(temperature);
    }
}

inline double limit(double value, double min, double max) {
    if(value < min) {
        return min;
    } else if(value > max) {
        return max;
    } else {
        return value;
    }
}

double error = 0;
double pTerm = 0;
double iTerm = 0;
const double iTermSaturation = 10; /* Integral anti-windup */
const double integralErrorBand = 10; /* Don't compute integral outside this band */

void controlTemperature() {
    if(!temperatureValid) {
        relayDc = 0;
        return;
    }

    error = storage.getTargetTemperature() - temperature;
    pTerm = storage.getKp() * error;

    double band = storage.getIntegralErrorBand();
    if(error < band && error > -band) {
        double sat = storage.getiTermSaturation();
        iTerm = limit(storage.getKi() * error + iTerm, -sat, sat);
    }
    relayDc = limit(pTerm + iTerm + storage.getKo(), 0, 100);
}

void commandRelay() {
    unsigned int onTime = (unsigned int)(relayDc * PERIOD / 100);
    if(onTime > 0) {
        digitalWrite(RELAY_PIN, RELAY_ON);
        display.setLastDot(true);
        // Bypass system event pump in usual delay()
        HAL_Delay_Milliseconds(onTime);
        digitalWrite(RELAY_PIN, RELAY_OFF);
        display.setLastDot(false);
    } else {
        digitalWrite(RELAY_PIN, RELAY_OFF);
    }
}

void toggleSleep() {
    if(storage.getSleep()) {
        storage.setSleep(0);
    } else {
        const long SECONDS_PER_DAY = 24 * 60 * 60;

        long sleepDuration = (long)storage.getTwakeup() - Time.now();
        if(sleepDuration < 0) {
            long additionalDays = (-sleepDuration / SECONDS_PER_DAY + 1) * SECONDS_PER_DAY;
            storage.setTwakeup(storage.getTwakeup() + additionalDays);
        }

        storage.setSleep(1);
    }
}

void processSleep() {
    display.printDodo();
    if(Time.now() > (long)storage.getTwakeup()) {
        storage.setSleep(0);
    }

    relayDc = 0;
}

void setup() {
    Serial.begin(115200);
    storage.read();
    startHardwareThread();
    startButtonThread();
    setupCloud();
}

void setupCloud() {
    Particle.connect();
    Particle.variable("temp", &temperature, DOUBLE);
    Particle.variable("dc", &relayDc, DOUBLE);
    Particle.variable("cals", cals, STRING);
    Particle.function("dfu", enterDFU);
    cloudStorage.setup(&storage);    
}

int enterDFU(String) {
    System.dfu();
    return 0;
}

void loop() {
    publishData();
    updateCals();
    delay(PERIOD);
}

void publishData() {
    if(Particle.connected()) {
        Particle.publish("coffee", "{"
                "\"temp\":" + String(temperature, 1) + ","
                "\"dc\":" + String(relayDc, 1) + ","
                "\"e\":" + String(error, 1) + ","
                "\"p\":" + String(pTerm, 1) + ","
                "\"i\":" + String(iTerm, 1) + ","
                "\"s\":" + String(storage.getSleep(), 0)
                + "}", 60, PRIVATE);
    }
}

void updateCals() {
    String calsValue = String("{") +
        "\"sp\":" + String(storage.getTargetTemperature(), 1) + ","
        "\"Kp\":" + String(storage.getKp(), 1) + ","
        "\"Ki\":" + String(storage.getKi(), 3) + ","
        "\"Ko\":" + String(storage.getKo(), 1) + ","
        "\"iSat\":" + String(storage.getiTermSaturation(), 1)
        + "}";

    // Set global char array
    calsValue.toCharArray(cals, countof(cals));
}

// vim: sw=4
