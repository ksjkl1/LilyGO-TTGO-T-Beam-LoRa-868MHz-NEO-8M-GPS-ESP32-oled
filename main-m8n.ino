/*
  Main module
  # Modified by Kyle T. Gabriel to fix issue with incorrect GPS data for TTNMapper
  Copyright (C) 2018 by Xose Pérez <xose dot perez at gmail dot com>
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "configuration.h"
#include "rom/rtc.h"
#include <TinyGPS++.h>
#include <Wire.h>

#include "axp20x.h"
AXP20X_Class axp;
bool pmu_irq = false;
String baChStatus = "No charging";

bool ssd1306_found = false;
bool axp192_found = false;

bool packetSent, packetQueued;

#if defined(PAYLOAD_USE_FULL)
  // includes number of satellites and accuracy
  static uint8_t txBuffer[10];
#elif defined(PAYLOAD_USE_CAYENNE)
  // CAYENNE DF
  static uint8_t txBuffer[11] = {0x03, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#endif

// deep sleep support
RTC_DATA_ATTR int bootCount = 0;
esp_sleep_source_t wakeCause; // the reason we booted this time

// -----------------------------------------------------------------------------
// Application
// -----------------------------------------------------------------------------

void buildPacket(uint8_t txBuffer[]); // needed for platformio

/**
 * If we have a valid position send it to the server.
 * @return true if we decided to send.
 */
bool trySend() {
  packetSent = false;
  // We also wait for altitude being not exactly zero, because the GPS chip generates a bogus 0 alt report when first powered on
  if (0 < gps_hdop() && gps_hdop() < 50 && gps_latitude() != 0 && gps_longitude() != 0 && gps_altitude() != 0)
  {
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "Latitude: %10.6f\n", gps_latitude());
    screen_print(buffer);
    snprintf(buffer, sizeof(buffer), "Longitude: %10.6f\n", gps_longitude());
    screen_print(buffer);
    snprintf(buffer, sizeof(buffer), "Error: %4.2fm\n", gps_hdop());
    screen_print(buffer);

    buildPacket(txBuffer);

#if LORAWAN_CONFIRMED_EVERY > 0
    bool confirmed = (ttn_get_count() % LORAWAN_CONFIRMED_EVERY == 0);
    if (confirmed){ Serial.println("confirmation enabled"); }
#else
    bool confirmed = false;
#endif

    packetQueued = true;
    ttn_send(txBuffer, sizeof(txBuffer), LORAWAN_PORT, confirmed);
    return true;
  }
  else {
    return false;
  }
}


void doDeepSleep(uint64_t msecToWake)
{
    Serial.printf("Entering deep sleep for %llu seconds\n", msecToWake / 1000);

    // not using wifi yet, but once we are this is needed to shutoff the radio hw
    // esp_wifi_stop();

    screen_off(); // datasheet says this will draw only 10ua
    LMIC_shutdown(); // cleanly shutdown the radio
    
    if(axp192_found) {
        // turn on after initial testing with real hardware
        axp.setPowerOutPut(AXP192_LDO2, AXP202_OFF); // LORA radio
        axp.setPowerOutPut(AXP192_LDO3, AXP202_OFF); // GPS main power
    }

    // FIXME - use an external 10k pulldown so we can leave the RTC peripherals powered off
    // until then we need the following lines
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Only GPIOs which are have RTC functionality can be used in this bit map: 0,2,4,12-15,25-27,32-39.
    uint64_t gpioMask = (1ULL << BUTTON_PIN);

    // FIXME change polarity so we can wake on ANY_HIGH instead - that would allow us to use all three buttons (instead of just the first)
    gpio_pullup_en((gpio_num_t) BUTTON_PIN);

    esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ALL_LOW);

    esp_sleep_enable_timer_wakeup(msecToWake * 1000ULL); // call expects usecs
    esp_deep_sleep_start();                              // TBD mA sleep current (battery)
}


void sleep() {
#if SLEEP_BETWEEN_MESSAGES

  // If the user has a screen, tell them we are about to sleep
  if (ssd1306_found) {
    // Show the going to sleep message on the screen
    char buffer[20];
    snprintf(buffer, sizeof(buffer), "Sleeping in %3.1fs\n", (MESSAGE_TO_SLEEP_DELAY / 1000.0));
    screen_print(buffer);

    // Wait for MESSAGE_TO_SLEEP_DELAY millis to sleep
    delay(MESSAGE_TO_SLEEP_DELAY);

    // Turn off screen
    screen_off();
  }

  // Set the user button to wake the board
  sleep_interrupt(BUTTON_PIN, LOW);

  // We sleep for the interval between messages minus the current millis
  // this way we distribute the messages evenly every SEND_INTERVAL millis
  uint32_t sleep_for = (millis() < SEND_INTERVAL) ? SEND_INTERVAL - millis() : SEND_INTERVAL;
  doDeepSleep(sleep_for);

#endif
}


void callback(uint8_t message) {
  bool ttn_joined = false;
  if (EV_JOINED == message) {
    ttn_joined = true;
  }
  if (EV_JOINING == message) {
    if (ttn_joined) {
      screen_print("TTN joining...\n");
    } else {
      screen_print("Joined TTN!\n");
    }
  }
  if (EV_JOIN_FAILED == message) screen_print("TTN join failed\n");
  if (EV_REJOIN_FAILED == message) screen_print("TTN rejoin failed\n");
  if (EV_RESET == message) screen_print("Reset TTN connection\n");
  if (EV_LINK_DEAD == message) screen_print("TTN link dead\n");
  if (EV_ACK == message) screen_print("ACK received\n");
  if (EV_PENDING == message) screen_print("Message discarded\n");
  if (EV_QUEUED == message) screen_print("Message queued\n");

  // We only want to say 'packetSent' for our packets (not packets needed for joining)
  if (EV_TXCOMPLETE == message && packetQueued) {
    screen_print("Message sent\n");
    packetQueued = false;
    packetSent = true;
  }

  if (EV_RESPONSE == message) {

    screen_print("[TTN] Response: ");

    size_t len = ttn_response_len();
    uint8_t data[len];
    ttn_response(data, len);

    char buffer[6];
    for (uint8_t i = 0; i < len; i++) {
      snprintf(buffer, sizeof(buffer), "%02X", data[i]);
      screen_print(buffer);
    }
    screen_print("\n");
  }
}


void scanI2Cdevice(void)
{
    byte err, addr;
    int nDevices = 0;
    for (addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        err = Wire.endTransmission();
        if (err == 0) {
            Serial.print("I2C device found at address 0x");
            if (addr < 16)
                Serial.print("0");
            Serial.print(addr, HEX);
            Serial.println(" !");
            nDevices++;

            if (addr == SSD1306_ADDRESS) {
                ssd1306_found = true;
                Serial.println("ssd1306 display found");
            }
            if (addr == AXP192_SLAVE_ADDRESS) {
                axp192_found = true;
                Serial.println("axp192 PMU found");
            }
        } else if (err == 4) {
            Serial.print("Unknow error at address 0x");
            if (addr < 16)
                Serial.print("0");
            Serial.println(addr, HEX);
        }
    }
    if (nDevices == 0)
        Serial.println("No I2C devices found\n");
    else
        Serial.println("done\n");
}

/**
 * Init the power manager chip
 * 
 * axp192 power 
    DCDC1 0.7-3.5V @ 1200mA max -> OLED // If you turn this off you'll lose comms to the axp192 because the OLED and the axp192 share the same i2c bus, instead use ssd1306 sleep mode
    DCDC2 -> unused
    DCDC3 0.7-3.5V @ 700mA max -> ESP32 (keep this on!)
    LDO1 30mA -> charges GPS backup battery // charges the tiny J13 battery by the GPS to power the GPS ram (for a couple of days), can not be turned off
    LDO2 200mA -> LORA
    LDO3 200mA -> GPS
 */

void axp192Init() {
    if (axp192_found) {
        if (!axp.begin(Wire, AXP192_SLAVE_ADDRESS)) {
            Serial.println("AXP192 Begin PASS");
        } else {
            Serial.println("AXP192 Begin FAIL");
        }
        // axp.setChgLEDMode(LED_BLINK_4HZ);
        Serial.printf("DCDC1: %s\n", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("DCDC2: %s\n", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("LDO2: %s\n", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("LDO3: %s\n", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("DCDC3: %s\n", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("Exten: %s\n", axp.isExtenEnable() ? "ENABLE" : "DISABLE");
        Serial.println("----------------------------------------");

        axp.setPowerOutPut(AXP192_LDO2, AXP202_ON); // LORA radio
        axp.setPowerOutPut(AXP192_LDO3, AXP202_ON); // GPS main power
        axp.setPowerOutPut(AXP192_DCDC2, AXP202_ON);
        axp.setPowerOutPut(AXP192_EXTEN, AXP202_ON);
        axp.setPowerOutPut(AXP192_DCDC1, AXP202_ON);
        axp.setDCDC1Voltage(3300); // for the OLED power

        Serial.printf("DCDC1: %s\n", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("DCDC2: %s\n", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("LDO2: %s\n", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("LDO3: %s\n", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("DCDC3: %s\n", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
        Serial.printf("Exten: %s\n", axp.isExtenEnable() ? "ENABLE" : "DISABLE");

        pinMode(PMU_IRQ, INPUT_PULLUP);
        attachInterrupt(PMU_IRQ, [] {
            pmu_irq = true;
        }, FALLING);

        axp.adc1Enable(AXP202_BATT_CUR_ADC1, 1);
        axp.enableIRQ(AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_BATT_REMOVED_IRQ | AXP202_BATT_CONNECT_IRQ, 1);
        axp.clearIRQ();

        if (axp.isChargeing()) {
            baChStatus = "Charging";
        }
    } else {
        Serial.println("AXP192 not found");
    }
}


// Perform power on init that we do on each wake from deep sleep
void initDeepSleep() {
    bootCount++;
    wakeCause = esp_sleep_get_wakeup_cause(); 
    /* 
    Not using yet because we are using wake on all buttons being low
    wakeButtons = esp_sleep_get_ext1_wakeup_status();       // If one of these buttons is set it was the reason we woke
    if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 && !wakeButtons) // we must have been using the 'all buttons rule for waking' to support busted boards, assume button one was pressed
        wakeButtons = ((uint64_t)1) << buttons.gpios[0];
    */

    Serial.printf("booted, wake cause %d (boot count %d)\n", wakeCause, bootCount);
}


void setup() {
  // Debug
#ifdef DEBUG_PORT
  DEBUG_PORT.begin(SERIAL_BAUD);
#endif

  initDeepSleep();

  Wire.begin(I2C_SDA, I2C_SCL);
  scanI2Cdevice();

  axp192Init();

  // Buttons & LED
  pinMode(BUTTON_PIN, INPUT_PULLUP);

#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
#endif

  // Hello
  DEBUG_MSG(APP_NAME " " APP_VERSION "\n");

  // Don't init display if we don't have one or we are waking headless due to a timer event
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER)
    ssd1306_found = false; // forget we even have the hardware

  if (ssd1306_found) screen_setup();

  // Init GPS
  gps_setup();

  // Show logo on first boot after removing battery

#ifndef ALWAYS_SHOW_LOGO 
  if (bootCount == 0) {
#endif
    screen_print(APP_NAME " " APP_VERSION, 0, 0);
    screen_show_logo();
    screen_update();
    delay(LOGO_DELAY);
 #ifndef ALWAYS_SHOW_LOGO
 }
#endif

  // TTN setup
  if (!ttn_setup()) {
    screen_print("[ERR] Radio module not found!\n");

    if (REQUIRE_RADIO) {
      delay(MESSAGE_TO_SLEEP_DELAY);
      screen_off();
      sleep_forever();
    }
  }
  else {
    ttn_register(callback);
    ttn_join();
    ttn_adr(LORAWAN_ADR);
  }
}

void loop() {
  gps_loop();
  ttn_loop();
  screen_loop();

  if (packetSent) {
    packetSent = false;
    sleep();
  }

  // if user presses button for more than 3 secs, discard our network prefs and reboot (FIXME, use a debounce lib instead of this boilerplate)
  static bool wasPressed = false;
  static uint32_t minPressMs; // what tick should we call this press long enough
  if (!digitalRead(BUTTON_PIN)) {
    if (!wasPressed) { // just started a new press
      Serial.println("pressing");
      wasPressed = true;
      minPressMs = millis() + 3000;
    } 
  } else if (wasPressed) {
    // we just did a release
    wasPressed = false;
    if(millis() > minPressMs) {
      // held long enough
#ifndef PREFS_DISCARD
      screen_print("Discarding prefs disabled\n");
#endif 
#ifdef PREFS_DISCARD
      screen_print("Discarding prefs!\n");
      ttn_erase_prefs();
      delay(5000); // Give some time to read the screen
      ESP.restart();
#endif
    }
  }

  // Send every SEND_INTERVAL millis
  static uint32_t last = 0;
  static bool first = true;
  if (0 == last || millis() - last > SEND_INTERVAL) {
    if (trySend()) {
      last = millis();
      first = false;
      Serial.println("TRANSMITTED");
    } else {
      if (first) {
        screen_print("Waiting GPS lock\n");
        first = false;
      }

#ifdef GPS_WAIT_FOR_LOCK
      if (millis() > GPS_WAIT_FOR_LOCK) {
        sleep();
      }
#endif

      // No GPS lock yet, let the OS put the main CPU in low power mode for 100ms (or until another interrupt comes in)
      // i.e. don't just keep spinning in loop as fast as we can.
      delay(100);
    }
  }
}
