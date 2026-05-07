#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h> 
#include <esp_task_wdt.h>
#include <esp_sleep.h>      
#include "driver/gpio.h"    // [ZMIANA C3_RTC] Używamy driver/gpio.h (zamiast rtc_io.h), tu w ESP-IDF dla C3 znajduje się definicja gpio_hold_en()
#include <rom/crc.h> 
#include "Adafruit_SGP30.h"
#include "Adafruit_SHTC3.h"

// --- KONFIGURACJA SPRZĘTOWA ---
#define I2C_SDA 3
#define I2C_SCL 4
#define WDT_TIMEOUT_SEC 5
#define I2C_ERROR_LIMIT 5 

#define LED_PIN 8
#define LED_ON()  digitalWrite(LED_PIN, LOW)
#define LED_OFF() digitalWrite(LED_PIN, HIGH)

// [ZMIANA C3_RTC] Przeniesienie przycisku na piny należące do sprzętowej domeny RTC w architekturze ESP32-C3 (GPIO0-GPIO5)
#define BUTTON_PIN_SENSE 1 
#define BUTTON_PIN_GND 2   

uint8_t receiverMAC[] = {0xAC, 0x67, 0xB2, 0x1B, 0xE0, 0xC0};

Adafruit_SGP30 sgp;
Adafruit_SHTC3 shtc2 = Adafruit_SHTC3();

enum NodeRole { ROLE_UNKNOWN, ROLE_SENSOR_AND_TRIGGER, ROLE_TRIGGER_ONLY };
RTC_DATA_ATTR NodeRole myRole = ROLE_UNKNOWN;

enum SystemState { INIT_I2C, WARMUP, RUNNING, RECOVER_I2C, SYSTEM_ERROR };
SystemState currentState = INIT_I2C;
unsigned long stateStartTime = 0;
const long WARMUP_DURATION = 15000;
bool initRetryFlag = false; 

unsigned long previousFastMillis = 0;
const long fastInterval = 200;  
unsigned long previousSlowMillis = 0;
const long slowInterval = 1000; 
uint8_t i2cErrorCount = 0;

unsigned long lastSerialPrintTime = 0; 
static char slowDataStr[64] = "";

typedef struct __attribute__((packed)) struct_message {
    uint32_t seq_num;     
    uint32_t uptime;      
    char text_data[140];  
    uint32_t crc32;       
} struct_message;

struct_message txPacket;
esp_now_peer_info_t peerInfo;

RTC_DATA_ATTR uint32_t sequenceNumber = 0;
volatile bool tx_radio_busy = false; 
volatile uint32_t dropped_frames = 0; 

unsigned long ledTurnOffTime = 0;
unsigned long errorBlinkTimer = 0;
bool errorLedState = false;

bool slow_dirty = false;
bool fast_dirty = false;
bool pending_trigger_tx = false; 

float cachedTemp = 0.0, cachedRH = 0.0, cachedAbsHum = 0.0;
uint16_t cachedTVOC = 0, cachedeCO2 = 0;

float getAbsoluteHumidityFloat(float temperature, float humidity) {
    if(temperature == 0.0 && humidity == 0.0) return 0.0;
    return 216.7f * ((humidity / 100.0f) * 6.112f * exp((17.62f * temperature) / (243.12f + temperature)) / (273.15f + temperature));
}

void enterDeepSleep() {
    if(Serial) {
        Serial.println("\n[SLEEP] Zapadanie w Deep Sleep. Czekam na guzik na GPIO1...");
        Serial.flush();
    }
    
    // [ZMIANA C3_RTC] Wymuszamy ostateczny stan pinów przed snem. SENSE musi mieć Pull-Up na 3V3, a GND musi być twardym 0V.
    pinMode(BUTTON_PIN_SENSE, INPUT_PULLUP);
    pinMode(BUTTON_PIN_GND, OUTPUT);
    digitalWrite(BUTTON_PIN_GND, LOW);

    // [ZMIANA C3_RTC] Zamrażamy zasilanie obu pinów domeny RTC w zaprogramowanych powyżej stanach.
    gpio_hold_en((gpio_num_t)BUTTON_PIN_GND); 
    gpio_hold_en((gpio_num_t)BUTTON_PIN_SENSE); 
    
    // [ZMIANA C3_RTC] Zmiana API wybudzania. W ESP32-C3 wybudzanie na pinach odbywa się przez dedykowaną maskę GPIO, a nie przez klasyczne EXT1 znane ze starych ESP.
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN_SENSE, ESP_GPIO_WAKEUP_GPIO_LOW);
    
    esp_deep_sleep_start(); 
}

void recoverI2CBus() {
    if(Serial) Serial.println("\n[CRITICAL] Bit-Banging: Odblokowywanie I2C...");
    Wire.end();
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, OUTPUT);
    for(int i = 0; i < 9; i++) {
        digitalWrite(I2C_SCL, LOW); delayMicroseconds(10);
        digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
        if(digitalRead(I2C_SDA) == HIGH) break; 
    }
    pinMode(I2C_SDA, OUTPUT);
    digitalWrite(I2C_SDA, LOW); delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
    digitalWrite(I2C_SDA, HIGH); delayMicroseconds(10);
    Wire.begin(I2C_SDA, I2C_SCL);
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    tx_radio_busy = false; 
    if (status != ESP_NOW_SEND_SUCCESS) {
        dropped_frames++;
    }
}

void pollButton(unsigned long currentMillis) {
    static bool lastRawState = HIGH;
    static bool validatedState = HIGH;
    static unsigned long lastDebounceTime = 0;
    const unsigned long DEBOUNCE_DELAY = 20; 
    
    bool rawState = digitalRead(BUTTON_PIN_SENSE);

    if (rawState != lastRawState) {
        lastDebounceTime = currentMillis;
    }

    if ((currentMillis - lastDebounceTime) >= DEBOUNCE_DELAY) {
        if (rawState != validatedState) {
            validatedState = rawState;
            if (validatedState == LOW) {
                if(Serial && (currentMillis - lastSerialPrintTime >= 50)) {
                    Serial.println("\n>>> GUZIK WCIŚNIĘTY! Zgłaszam żądanie TX <<<");
                }
                pending_trigger_tx = true; 
            }
        }
    }
    lastRawState = rawState;
}

void setup() {
    Serial.begin(500000);
    
    // [ZMIANA C3_RTC] Odblokowanie pinów. Po wybudzeniu piny wciąż mogą być "zamrożone" sprzętowo, zwalniamy je, aby cyfrowy odczyt działał poprawnie.
    gpio_hold_dis((gpio_num_t)BUTTON_PIN_GND);
    gpio_hold_dis((gpio_num_t)BUTTON_PIN_SENSE);

    WiFi.mode(WIFI_STA); 
    WiFi.setTxPower(WIFI_POWER_8_5dBm); 

    pinMode(LED_PIN, OUTPUT);
    LED_OFF();

    pinMode(BUTTON_PIN_GND, OUTPUT);
    digitalWrite(BUTTON_PIN_GND, LOW);
    pinMode(BUTTON_PIN_SENSE, INPUT_PULLUP);

    // [ZMIANA C3_RTC] Sprawdzenie powodu pobudki - dla C3 używamy ESP_SLEEP_WAKEUP_GPIO w miejsce dawnego EXT1.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        pending_trigger_tx = true; 
    }

    esp_task_wdt_init(WDT_TIMEOUT_SEC, true); 
    esp_task_wdt_add(NULL); 
    
    if (esp_now_init() != ESP_OK) {
        currentState = SYSTEM_ERROR;
    } else {
        esp_now_register_send_cb(OnDataSent);
        memcpy(peerInfo.peer_addr, receiverMAC, 6);
        peerInfo.channel = 1; 
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
        
        if (myRole == ROLE_TRIGGER_ONLY) {
            currentState = RUNNING; 
        } else {
            currentState = INIT_I2C;
        }
    }
}

void loop() {
    unsigned long currentMillis = millis();
    esp_task_wdt_reset(); 

    if (ledTurnOffTime > 0 && currentMillis >= ledTurnOffTime) {
        LED_OFF();
        ledTurnOffTime = 0;
    }

    pollButton(currentMillis);

    switch (currentState) {
        case INIT_I2C:
            Wire.begin(I2C_SDA, I2C_SCL);
            if (shtc2.begin(&Wire) && sgp.begin(&Wire)) {
                if(Serial) Serial.println("\n[OK] I2C Podłączone. Tryb: SENSOR + GUZIK.");
                myRole = ROLE_SENSOR_AND_TRIGGER;
                stateStartTime = currentMillis;
                currentState = WARMUP;
            } else {
                if (!initRetryFlag) {
                    initRetryFlag = true;
                    recoverI2CBus();
                } else {
                    if(Serial) Serial.println("\n[INFO] Brak I2C. Tryb: TYLKO GUZIK.");
                    myRole = ROLE_TRIGGER_ONLY;
                    currentState = RUNNING; 
                }
            }
            break;

        case WARMUP:
            if (currentMillis - stateStartTime >= WARMUP_DURATION) {
                currentState = RUNNING;
                previousFastMillis = currentMillis;
                previousSlowMillis = currentMillis;
            } else {
                if (currentMillis - previousSlowMillis >= slowInterval) {
                    previousSlowMillis = currentMillis;
                    sensors_event_t humidity, temp;
                    shtc2.getEvent(&humidity, &temp);
                    sgp.setHumidity(static_cast<uint32_t>(getAbsoluteHumidityFloat(temp.temperature, humidity.relative_humidity) * 256.0f));
                    sgp.IAQmeasure(); 
                    
                    if(Serial) {
                        Serial.print("\rRozgrzewanie warstwy MOX... Pozostało: "); 
                        Serial.print((WARMUP_DURATION - (currentMillis - stateStartTime))/1000); 
                        Serial.print("s   ");
                    }
                }
            }
            break;

        case RECOVER_I2C:
            // ... (bez zmian)
            recoverI2CBus();
            currentState = INIT_I2C; 
            break;

        case SYSTEM_ERROR:
            // ... (bez zmian)
            break;

        case RUNNING:
            if (myRole == ROLE_SENSOR_AND_TRIGGER) {
                // ... (PĘTLA 1 Hz i 5 Hz bez zmian, usunięto dla czystości podglądu)
                if (currentMillis - previousSlowMillis >= slowInterval) {
                    if (slow_dirty) dropped_frames++; 
                    previousSlowMillis = currentMillis;
                    sensors_event_t humidity, temp;
                    if (shtc2.getEvent(&humidity, &temp)) {
                        cachedTemp = temp.temperature;
                        cachedRH = humidity.relative_humidity;
                        cachedAbsHum = getAbsoluteHumidityFloat(cachedTemp, cachedRH);
                        sgp.setHumidity(static_cast<uint32_t>(cachedAbsHum * 256.0f));
                        if (sgp.IAQmeasure()) {
                            cachedTVOC = sgp.TVOC;
                            cachedeCO2 = sgp.eCO2;
                            slow_dirty = true; 
                            i2cErrorCount = 0; 
                        } else i2cErrorCount++;
                    } else i2cErrorCount++;
                }

                if (currentMillis - previousFastMillis >= fastInterval) {
                    if (fast_dirty) dropped_frames++; 
                    previousFastMillis = currentMillis;
                    if (!sgp.IAQmeasureRaw()) {
                        i2cErrorCount++;
                    } else {
                        i2cErrorCount = 0;
                        fast_dirty = true; 
                    }
                    if (i2cErrorCount >= I2C_ERROR_LIMIT) {
                        currentState = RECOVER_I2C;
                        i2cErrorCount = 0;
                    }
                }
            }
            break;
    }

    if (!tx_radio_busy) {
        if (pending_trigger_tx) {
            sequenceNumber++;
            txPacket.seq_num = sequenceNumber;
            txPacket.uptime = currentMillis;
            snprintf(txPacket.text_data, sizeof(txPacket.text_data), "SEQ:%u TYPE:TRIGGER ACTION:CLICK", sequenceNumber);
            txPacket.crc32 = crc32_le(0, (uint8_t*)&txPacket, offsetof(struct_message, crc32));

            tx_radio_busy = true;
            if (esp_now_send(receiverMAC, (uint8_t *) &txPacket, sizeof(txPacket)) == ESP_OK) {
                LED_ON(); ledTurnOffTime = millis() + 150; 
            } else { tx_radio_busy = false; dropped_frames++; }
            
            pending_trigger_tx = false; 
        }
        else if (myRole == ROLE_SENSOR_AND_TRIGGER && currentState == RUNNING && (fast_dirty || slow_dirty)) {
            if (slow_dirty) {
                snprintf(slowDataStr, sizeof(slowDataStr), "T:%.2f RH:%.2f AH:%.2f TVOC:%u CO2:%u ", 
                         cachedTemp, cachedRH, cachedAbsHum, cachedTVOC, cachedeCO2);
            }
            sequenceNumber++;
            txPacket.seq_num = sequenceNumber;
            txPacket.uptime = currentMillis;
            snprintf(txPacket.text_data, sizeof(txPacket.text_data), 
                "S:%u %srH2:%u rEtOH:%u",
                sequenceNumber, slowDataStr, sgp.rawH2, sgp.rawEthanol);
            txPacket.crc32 = crc32_le(0, (uint8_t*)&txPacket, offsetof(struct_message, crc32));

            tx_radio_busy = true;
            if (esp_now_send(receiverMAC, (uint8_t *) &txPacket, sizeof(txPacket)) == ESP_OK) {
                LED_ON(); ledTurnOffTime = millis() + 10;  
            } else { tx_radio_busy = false; dropped_frames++; }
            slow_dirty = false;
            fast_dirty = false;
        }
    } 

    if (myRole == ROLE_TRIGGER_ONLY && currentState == RUNNING) {
        if (!tx_radio_busy && !pending_trigger_tx) {
            if (ledTurnOffTime == 0 || currentMillis >= ledTurnOffTime) {
                enterDeepSleep();
            }
        }
    }
}