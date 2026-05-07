#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <rom/crc.h>
#include <string.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"

// --- KONFIGURACJA SIECI (STA) ---
const char* ssid = "motorola";
const char* password = "japko123456";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

// --- KONFIGURACJA SPRZĘTU ---
#define WDT_TIMEOUT_SEC 10
#define RED_LED_PIN 33 
#define MANAGER_BUTTON_PIN 13 

#define LED_ON()  digitalWrite(RED_LED_PIN, LOW)
#define LED_OFF() digitalWrite(RED_LED_PIN, HIGH)

const uint64_t MIN_FREE_SPACE_BYTES = 10ULL * 1024ULL * 1024ULL; 

typedef struct __attribute__((packed)) struct_message {
    uint32_t seq_num; uint32_t uptime; char text_data[140]; uint32_t crc32;
} struct_message;

// --- ZMIENNE GLOBALNE ---
QueueHandle_t rxQueue; 
File logFile;
char currentFileName[32];
bool sdMounted = false; 
bool triggerFlag = false; 

AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;

String apPassword;
bool isCaptivePortalMode = false;
bool bootCountCleared = false;

uint32_t recordsBuffered = 0;
unsigned long lastFlushTime = 0;
const unsigned long FLUSH_INTERVAL = 10000; 
const uint32_t FLUSH_RECORD_COUNT = 50;     

unsigned long lastRxTime = 0; 
unsigned long lastSerialPrintTime = 0; 
unsigned long lastNtpSyncTime = 0;
const unsigned long NTP_RESYNC_INTERVAL = 86400000; 
unsigned long lastHeartbeatTime = 0;

unsigned long ledTurnOffTime = 0;
unsigned long errorBlinkTimer = 0;
bool errorLedState = false;
unsigned long managerDisconnectTimer = 0;

unsigned long lastApStationCheck = 0;
const unsigned long AP_CHECK_INTERVAL = 20; 

// [POPRAWKA] Usunięto stan WAIT_MANAGER
enum SystemState { INIT_WIFI, SYNC_NTP, INIT_SD, INIT_AP, OPEN_FILE, INIT_ESPNOW, RUNNING, MANAGER_MODE, SYSTEM_ERROR, ERROR_SD_FULL };
SystemState currentState = INIT_WIFI;

// --- FUNKCJE POMOCNICZE ---
bool checkSDFreeSpace() { return ((SD_MMC.totalBytes() - SD_MMC.usedBytes()) > MIN_FREE_SPACE_BYTES); }

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    struct_message rxPacket;
    if (len == sizeof(rxPacket)) {
        memcpy(&rxPacket, incomingData, sizeof(rxPacket));
        xQueueSendFromISR(rxQueue, &rxPacket, NULL);
    }
}

// --- SZTYWNE "ODWIESZANIE" SYSTEMÓW OS (FAST REDIRECT) ---
void registerFastCaptivePortalRoutes() {
    auto redirectHandler = [](AsyncWebServerRequest *request) {
        request->redirect("http://192.168.4.1/");
    };
    server.on("/generate_204", HTTP_ANY, redirectHandler);        
    server.on("/hotspot-detect.html", HTTP_ANY, redirectHandler); 
    server.on("/ncsi.txt", HTTP_ANY, redirectHandler);            
    server.on("/connecttest.txt", HTTP_ANY, redirectHandler);     
    server.on("/redirect", HTTP_ANY, redirectHandler);            
    server.onNotFound(redirectHandler);
}

// --- SETUP ROUTINGU ASYNCHRONICZNEGO ---
void configureAsyncServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        // [POPRAWKA] Usunięto warunek currentState != WAIT_MANAGER
        if (currentState != MANAGER_MODE) {
            request->send(503, "text/plain", "System LOGUJE. Rozlacz sie i polacz ponownie, aby wywolac Menadzera.");
            return;
        }
        if (!sdMounted) { request->send(500, "text/html", "<h2>BŁĄD! Karta SD nie zamontowana.</h2>"); return; }
        
        uint32_t totalMB = SD_MMC.totalBytes() / (1024 * 1024);
        uint32_t usedMB = SD_MMC.usedBytes() / (1024 * 1024);
        
        String html; html.reserve(4096);
        html += "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<title>TVOC Manager</title><style>body{font-family:Arial,sans-serif; background:#f4f7f6; color:#333; margin:0; padding:20px;}";
        html += ".card{background:#fff; border-radius:10px; padding:20px; box-shadow:0 4px 6px rgba(0,0,0,0.1); max-width:600px; margin:auto;}";
        html += "h2{color:#2c3e50; margin-top:0;} .stats{background:#e8f4f8; padding:15px; border-radius:8px; margin-bottom:20px;}";
        html += ".btn{display:inline-block; padding:10px 15px; text-decoration:none; color:#fff; border-radius:5px; font-weight:bold; cursor:pointer; border:none; margin-bottom:5px;}";
        html += ".btn-blue{background:#3498db;} .btn-green{background:#2ecc71;} .btn-red{background:#e74c3c;}";
        html += "ul{list-style:none; padding:0;} li{background:#fdfdfd; border:1px solid #ddd; padding:10px; margin-bottom:8px; border-radius:5px; display:flex; justify-content:space-between; align-items:center;}";
        html += "</style><script>function dlAll() { let links = document.querySelectorAll('.dl-link'); if(links.length === 0) return;";
        html += "if(!confirm('Pobieranie w tle (Zero-RAM)...')) return; let d = 0; links.forEach(l => { setTimeout(() => {";
        html += "let i = document.createElement('iframe'); i.style.display='none'; i.src=l.href; document.body.appendChild(i);";
        html += "}, d); d += 1500; }); } function confirmClear() { if(confirm('USUNĄĆ WSZYSTKO?')) window.location.href='/clear'; }</script></head><body>";
        
        html += "<div class='card'><h2>Rejestrator TVOC</h2><div class='stats'><strong>Zajętość SD:</strong><br>";
        html += String(usedMB) + " MB zajęte z " + String(totalMB) + " MB</div>";
        html += "<button class='btn btn-green' onclick='dlAll()'>⬇ POBIERZ WSZYSTKIE</button> ";
        html += "<button class='btn btn-red' onclick='confirmClear()'>✖ WYCZYŚĆ KARTĘ</button><hr><ul>";
        
        File root = SD_MMC.open("/"); File file = root.openNextFile(); bool hasFiles = false;
        while(file) {
            String fName = String(file.name());
            if (fName.endsWith(".csv")) {
                hasFiles = true;
                html += "<li><div><strong>" + fName + "</strong><br><small>" + String(file.size() / 1024) + " KB</small></div><div>";
                html += "<a href='/download?file=" + fName + "' class='btn btn-blue dl-link'>Pobierz</a> ";
                html += "<a href='/delete?file=" + fName + "' class='btn btn-red' onclick=\"return confirm('Na pewno?');\">Usuń</a></div></li>";
            }
            file = root.openNextFile();
        }
        if (!hasFiles) html += "<li style='justify-content:center;'>Brak logów na karcie.</li>";
        html += "</ul><p style='text-align:center; color:#7f8c8d;'><small>Aby wznowić logowanie, rozłącz sieć WiFi.</small></p></div></body></html>";
        
        request->send(200, "text/html", html);
    });

    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
        // [POPRAWKA] Usunięto warunek currentState != WAIT_MANAGER
        if (currentState != MANAGER_MODE) { request->send(503); return; }
        if (!request->hasParam("file")) { request->send(400, "text/plain", "Brak parametru"); return; }
        String path = "/" + request->getParam("file")->value();
        request->send(SD_MMC, path, "application/octet-stream", true); 
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        // [POPRAWKA] Usunięto warunek currentState != WAIT_MANAGER
        if (currentState != MANAGER_MODE) { request->send(503); return; }
        if (request->hasParam("file")) SD_MMC.remove("/" + request->getParam("file")->value());
        request->redirect("/");
    });

    server.on("/clear", HTTP_GET, [](AsyncWebServerRequest *request){
        // [POPRAWKA] Usunięto warunek currentState != WAIT_MANAGER
        if (currentState != MANAGER_MODE) { request->send(503); return; }
        File root = SD_MMC.open("/"); File file = root.openNextFile();
        while(file) {
            String path = "/" + String(file.name());
            if (path.endsWith(".csv")) SD_MMC.remove(path);
            file = root.openNextFile();
        }
        request->redirect("/");
    });

    registerFastCaptivePortalRoutes();
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    
    // START MAC ADDRESS - Wybudzenie interfejsu STA
    WiFi.mode(WIFI_STA); 
    
    pinMode(RED_LED_PIN, OUTPUT); pinMode(MANAGER_BUTTON_PIN, INPUT_PULLUP);
    LED_OFF();
    
    prefs.begin("tvoc_sys", false);
    uint32_t bootCount = prefs.getUInt("bc", 0);
    
    if (bootCount >= 3) {
        Serial.println("\n[!!!] 3-KROTNY RESTART - Captive Portal Konfiguracji.");
        isCaptivePortalMode = true; prefs.putUInt("bc", 0); 
    } else {
        prefs.putUInt("bc", bootCount + 1); 
        apPassword = prefs.getString("pass", "admin123"); 
    }

    rxQueue = xQueueCreate(30, sizeof(struct_message));
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true); esp_task_wdt_add(NULL);

    if (isCaptivePortalMode) {
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.softAP("TVOC_Receptor_Reset"); 
        dnsServer.start(53, "*", WiFi.softAPIP());
        
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
            String html = "<html><body style='text-align:center; padding:50px; font-family:sans-serif;'><h2>Reset Urządzenia</h2><form action='/setpass' method='POST'>";
            html += "<input type='text' name='p' required minlength='8' placeholder='Nowe haslo (min 8)' style='padding:10px;'><br><br>";
            html += "<input type='submit' value='ZAPISZ' style='padding:10px 20px;'></form></body></html>";
            request->send(200, "text/html", html);
        });
        server.on("/setpass", HTTP_POST, [](AsyncWebServerRequest *request){
            if (request->hasParam("p", true)) {
                prefs.putString("pass", request->getParam("p", true)->value());
                request->send(200, "text/html", "<h2>Zapisano! System startuje...</h2>");
                delay(2000); ESP.restart();
            }
        });
        registerFastCaptivePortalRoutes();
        server.begin();
        while(true) { dnsServer.processNextRequest(); esp_task_wdt_reset(); yield(); }
    } else {
        configureAsyncServer();
        server.begin(); 
    }
}

// --- PĘTLA GŁÓWNA ---
void loop() {
    unsigned long currentMillis = millis();
    esp_task_wdt_reset();

    if (!bootCountCleared && currentMillis > 5000) { prefs.putUInt("bc", 0); bootCountCleared = true; }

    // Przycisk Ręcznego Wyzwolenia Menedżera
    if (digitalRead(MANAGER_BUTTON_PIN) == LOW && currentState != MANAGER_MODE) {
        Serial.println("\n[!] PRZYCISK: Wymuszam tryb MANAGER_MODE...");
        delay(300); 
        if (!sdMounted) if (SD_MMC.begin("/sdcard", true)) sdMounted = true;
        if (logFile) { logFile.flush(); logFile.close(); }
        if (currentState == RUNNING) { esp_now_deinit(); }
        
        managerDisconnectTimer = 0; currentState = MANAGER_MODE;
        
        // KRYTYCZNE POPRAWKI MAC: Tryb AP_STA pozwala zachować nasłuchujący interfejs STA
        WiFi.mode(WIFI_AP_STA);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); // Wymuszenie kanału 1 dla ESP-NOW
        esp_wifi_set_promiscuous(false);
        
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.softAP("TVOC_Receptor", apPassword.c_str(), 1);
        dnsServer.start(53, "*", WiFi.softAPIP());
    }

    if (ledTurnOffTime > 0 && currentMillis >= ledTurnOffTime) { LED_OFF(); ledTurnOffTime = 0; }

    switch (currentState) {
        case INIT_WIFI:
            Serial.println("\n=========================================");
            Serial.print("ADRES MAC TEJ PŁYTKI (Wpisz do Sondy!): ");
            Serial.println(WiFi.macAddress());
            Serial.println("=========================================");
            Serial.println("[1] Lączenie z WiFi...");
            
            WiFi.begin(ssid, password);
            { unsigned long s = millis(); while (WiFi.status() != WL_CONNECTED && millis()-s < 10000) { delay(500); Serial.print("."); esp_task_wdt_reset(); } }
            currentState = (WiFi.status() == WL_CONNECTED) ? SYNC_NTP : INIT_SD; 
            break;

        case SYNC_NTP:
            Serial.println("\n[2] Synchronizacja NTP...");
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
            { struct tm ti; unsigned long s = millis(); while(millis()-s < 5000) { if(getLocalTime(&ti, 10)) { lastNtpSyncTime = millis(); break; } esp_task_wdt_reset(); } }
            WiFi.disconnect(); // Łagodne rozłączenie z domowym routerem, bez ucinania interfejsu STA
            currentState = INIT_SD; 
            break;

        case INIT_SD:
            Serial.println("\n[3] Inicjalizacja Kary SD...");
            if (!sdMounted) if (SD_MMC.begin("/sdcard", true)) sdMounted = true;
            if (sdMounted) currentState = checkSDFreeSpace() ? INIT_AP : ERROR_SD_FULL;
            else currentState = SYSTEM_ERROR;
            break;

        case INIT_AP:
            Serial.println("\n[4] Start AP (TVOC_Receptor). Rozpoczynam logowanie (AP działa w tle).");
            
            // KRYTYCZNE POPRAWKI MAC: Tryb AP_STA pozwala zachować nasłuchujący interfejs STA
            WiFi.mode(WIFI_AP_STA);
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_promiscuous(false);
            
            WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
            WiFi.setTxPower(WIFI_POWER_8_5dBm); 
            WiFi.softAP("TVOC_Receptor", apPassword.c_str(), 1); 
            dnsServer.start(53, "*", WiFi.softAPIP());
            
            // [POPRAWKA] Bezpośrednie przejście do tworzenia pliku, ominięcie WAIT_MANAGER
            currentState = OPEN_FILE;
            break;

        // [POPRAWKA] Całkowicie usunięto case WAIT_MANAGER

        case OPEN_FILE:
            {
                struct tm ti; getLocalTime(&ti);
                snprintf(currentFileName, 32, "/LOG_%04d%02d%02d_%02d%02d.csv", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday, ti.tm_hour, ti.tm_min);
                logFile = SD_MMC.open(currentFileName, FILE_APPEND);
                if (!logFile) currentState = SYSTEM_ERROR;
                else {
                    if (logFile.size() == 0) logFile.println("TIMESTAMP,VERDICT,SEQ_NUM,PAYLOAD");
                    lastFlushTime = currentMillis; currentState = INIT_ESPNOW;
                }
            }
            break;

        case INIT_ESPNOW:
            esp_now_deinit(); 
            if (esp_now_init() == ESP_OK) {
                esp_now_register_recv_cb(OnDataRecv);
                Serial.println("\n>>> SYSTEM ODBIORCZY GOTOWY I ZABEZPIECZONY <<<");
                currentState = RUNNING;
            } else currentState = SYSTEM_ERROR;
            break;

        case SYSTEM_ERROR:
        case ERROR_SD_FULL:
            if (currentMillis - errorBlinkTimer > 100) { errorBlinkTimer = currentMillis; errorLedState = !errorLedState; if(errorLedState) LED_ON(); else LED_OFF(); }
            break;

        case RUNNING:
        {   
            if (currentMillis - lastApStationCheck > AP_CHECK_INTERVAL) {
                lastApStationCheck = currentMillis;
                if (WiFi.softAPgetStationNum() > 0) {
                    Serial.println("\n[!] KLIENT WIFI - Przejście do MANAGER_MODE.");
                    logFile.flush(); logFile.close(); esp_now_deinit();     
                    dnsServer.start(53, "*", WiFi.softAPIP()); 
                    managerDisconnectTimer = 0; currentState = MANAGER_MODE; break; 
                }
            }

            struct_message rxPacket; bool packetReceived = false;
            while (xQueueReceive(rxQueue, &rxPacket, 0) == pdTRUE) {
                packetReceived = true; unsigned long loopM = millis(); lastRxTime = loopM; 
                uint32_t calc_crc = crc32_le(0, (uint8_t*)&rxPacket, offsetof(struct_message, crc32));
                bool isDataValid = (calc_crc == rxPacket.crc32);

                struct tm ti; getLocalTime(&ti); char timeStr[24];
                snprintf(timeStr, 24, "%04d-%02d-%02d %02d:%02d:%02d", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
                char logVerdict[15];
                if (isDataValid) { snprintf(logVerdict, 15, "VALID"); LED_ON(); ledTurnOffTime = loopM + 3; } else snprintf(logVerdict, 15, "CRC_ERR");

                logFile.print(timeStr); logFile.print(","); logFile.print(logVerdict); logFile.print(","); logFile.print(rxPacket.seq_num); logFile.print(","); logFile.println(rxPacket.text_data);
                recordsBuffered++;
                if (strstr(rxPacket.text_data, "TRIGGER")) triggerFlag = true;

                if (loopM - lastSerialPrintTime >= 1000) {
                    lastSerialPrintTime = loopM;
                    if (Serial && Serial.availableForWrite() >= 64) {
                        Serial.printf("%s[RX SD: %02u/%02u] %s | %s | SEQ:%X | %s\n", triggerFlag ? ">>> [TRIGGERED] " : "", recordsBuffered, FLUSH_RECORD_COUNT, timeStr, logVerdict, rxPacket.seq_num, rxPacket.text_data);
                    }
                    triggerFlag = false; 
                }
            }

            if (!packetReceived && (currentMillis - lastHeartbeatTime >= 1000)) {
                lastHeartbeatTime = currentMillis;
                if (currentMillis - lastRxTime > 1000 && Serial && Serial.availableForWrite() >= 80)
                    Serial.printf("[NASŁUCH] Oczekuję na ramki z sondy... Brak sygnału od: %lu s.\n", (currentMillis - lastRxTime)/1000);
            }

            if (recordsBuffered > 0 && (recordsBuffered >= FLUSH_RECORD_COUNT || (currentMillis - lastFlushTime >= FLUSH_INTERVAL))) {
                uint32_t savedRec = recordsBuffered;
                logFile.flush(); lastFlushTime = currentMillis; recordsBuffered = 0;
                if (Serial && Serial.availableForWrite() >= 64) Serial.printf(">>> [SD] Wykonano fizyczny zrzut pamięci NAND (%u ramek).\n", savedRec);
            }

            if (currentMillis - lastNtpSyncTime >= NTP_RESYNC_INTERVAL) {
                logFile.flush(); logFile.close(); esp_now_deinit(); currentState = INIT_WIFI; 
            }
            break;
        }

        case MANAGER_MODE:
            dnsServer.processNextRequest();
            
            if (currentMillis - lastApStationCheck > AP_CHECK_INTERVAL) {
                lastApStationCheck = currentMillis;
                if (WiFi.softAPgetStationNum() == 0) {
                    if (managerDisconnectTimer == 0) managerDisconnectTimer = currentMillis; 
                    else if (currentMillis - managerDisconnectTimer > 5000) {
                        Serial.println("\n[!] Klient opuścił sieć. Wznawiam logowanie...");
                        managerDisconnectTimer = 0;
                        dnsServer.stop(); 
                        currentState = OPEN_FILE; 
                    }
                } else {
                    managerDisconnectTimer = 0; 
                }
            }
            break;
    }
}