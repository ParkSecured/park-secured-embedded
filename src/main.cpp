#include <Arduino.h>
#include <ESP32Servo.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "soc/rtc_cntl_reg.h"

// ═══════════════════════════════════════════════════════════════════
//  CONFIGURARE
// ═══════════════════════════════════════════════════════════════════
#define WIFI_SSID          "S22 al utilizatorului Bruh"
#define WIFI_PASS          "cmer6608"
#define CLOUD_API_BASE_URL "https://park-secured-cloud-r62j.onrender.com/api"
#define GATE_API_KEY       "cheie123"

// ─── Pini LED ───────────────────────────────────────────────────
#define PIN_LED_VERDE     25
#define PIN_LED_ROSU      26
#define PIN_LED_GALBEN    27
#define PIN_LED_ALBASTRU  14

// ─── Pini bariere IR ────────────────────────────────────────────
#define PIN_IR_TX1    32   // B1 — exterior (intrare)
#define PIN_IR_RX1    34
#define PIN_IR_TX2    33   // B2 — interior (dupa bara servo)
#define PIN_IR_RX2    35

// ─── Servo ──────────────────────────────────────────────────────
#define PIN_SERVO      13
#define SERVO_INCHIS    0
#define SERVO_DESCHIS  90
#define TIMP_SERVO_MS  3000   // timp pana bratul ajunge la capat (T2/T6)
#define SERVO_PAS_DELAY_MS 20 // delay intre pasi (ms) — mai mare = mai lent

// ─── Timeouturi ─────────────────────────────────────────────────
#define TIMEOUT_BLE_MS      120000
#define TIMEOUT_IR_LIBER_MS  5000  // T4: dupa eliberarea barierei finale, 5s inainte de inchidere

// ─── BLE UUIDs ──────────────────────────────────────────────────
#define SERVICE_UUID  "0000ABCD-0000-1000-8000-00805F9B34FB"
#define CHAR_UUID     "00001234-0000-1000-8000-00805F9B34FB"

// ═══════════════════════════════════════════════════════════════════
//  STARI FSM
// ═══════════════════════════════════════════════════════════════════
enum StarePOARTA {
    S0_INCHISA,           // Stare de veghe, LED galben
    S1_IN_CURS_DESCHIDERE,// Motor ridica bratul, LED verde intermitent
    S2_DESCHISA,          // Bratul sus, LED verde fix, asteapta vehicul
    S3_SIGURANTA_IR,      // Vehicul detectat, LED albastru, asteapta sa treaca
    S4_IN_CURS_INCHIDERE  // Motor coboara bratul, LED rosu
};

StarePOARTA starePoarta = S0_INCHISA;

// Sensul curent — detectat in S0, folosit in S3 si S4
String sensulCurent = "intrare";

// ═══════════════════════════════════════════════════════════════════
//  OBIECTE GLOBALE
// ═══════════════════════════════════════════════════════════════════
Servo servoPoarta;

// ─── BLE state ──────────────────────────────────────────────────
BLEServer*         bleServer          = nullptr;
BLECharacteristic* bleChar            = nullptr;
volatile bool      bleDeviceConnected = false;
volatile bool      bleCodPrimit       = false;
String             bleCodReceptat     = "";

// ═══════════════════════════════════════════════════════════════════
//  LED-URI
// ═══════════════════════════════════════════════════════════════════
void leduriOpresteTot() {
    digitalWrite(PIN_LED_VERDE,    LOW);
    digitalWrite(PIN_LED_ROSU,     LOW);
    digitalWrite(PIN_LED_GALBEN,   LOW);
    digitalWrite(PIN_LED_ALBASTRU, LOW);
}

void ledS0() { leduriOpresteTot(); digitalWrite(PIN_LED_ROSU, HIGH); }

void ledS1Intermitent() {
    static unsigned long lastToggle = 0;
    static bool ledOn = false;
    if (millis() - lastToggle > 300) {
        lastToggle = millis();
        ledOn = !ledOn;
        leduriOpresteTot();
        digitalWrite(PIN_LED_VERDE, ledOn ? HIGH : LOW);
    }
}

void ledS2() { leduriOpresteTot(); digitalWrite(PIN_LED_VERDE, HIGH); }
void ledS3() { leduriOpresteTot(); digitalWrite(PIN_LED_ALBASTRU, HIGH); }
void ledS4() { leduriOpresteTot(); digitalWrite(PIN_LED_ROSU, HIGH); }

// ═══════════════════════════════════════════════════════════════════
//  BARIERE IR
// ═══════════════════════════════════════════════════════════════════
void bariereSetup() {
    ledcSetup(4, 38000, 8); ledcAttachPin(PIN_IR_TX1, 4); ledcWrite(4, 127);
    ledcSetup(5, 38000, 8); ledcAttachPin(PIN_IR_TX2, 5); ledcWrite(5, 127);
    pinMode(PIN_IR_RX1, INPUT);
    pinMode(PIN_IR_RX2, INPUT);
}

bool b1Blocata() { return digitalRead(PIN_IR_RX1) == LOW; }
bool b2Blocata() { return digitalRead(PIN_IR_RX2) == LOW; }

// Bariera "de start" — prima pe care o intalneste vehiculul
// intrare: B1 (exterior), iesire: B2 (interior)
bool barieraStartBlocata() {
    return sensulCurent == "intrare" ? b1Blocata() : b2Blocata();
}

// Bariera "de sfarsit" — ultima pe care o trece vehiculul
// intrare: B2 (dupa bara servo), iesire: B1 (inainte de bara servo)
bool barieraFinalBlocata() {
    return sensulCurent == "intrare" ? b2Blocata() : b1Blocata();
}

// ═══════════════════════════════════════════════════════════════════
//  TIMP (NTP)
// ═══════════════════════════════════════════════════════════════════
void ntpSetup() {
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
    Serial.print("[NTP] Sincronizare");
    struct tm t;
    int tries = 0;
    while (!getLocalTime(&t) && tries++ < 20) { delay(500); Serial.print("."); }
    Serial.println(getLocalTime(&t) ? " OK" : " ESUAT");
}

String getTimestamp() {
    struct tm t;
    if (!getLocalTime(&t)) return "necunoscut";
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
    return String(buf);
}

// ═══════════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════════
void wifiSetup() {
    Serial.print("[WiFi] Conectare la " + String(WIFI_SSID));
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 20) {
        delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Conectat! IP: " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[WiFi] EROARE conectare!");
    }
}

String eventTypeFromSens(String sens) {
    return sens == "iesire" ? "EXIT" : "ENTRY";
}

bool wifiValideazaCod(String cod, String sens) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Deconectat! Reconectare...");
        wifiSetup();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Reconectare esuata.");
            return false;
        }
    }
    Serial.println("[WiFi] Status OK, IP: " + WiFi.localIP().toString());

    cod.trim();
    cod.replace("\r", "");
    cod.replace("\n", "");

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);

    HTTPClient http;
    http.begin(client, String(CLOUD_API_BASE_URL) + "/gate/validate-bluetooth");
    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Gate-Api-Key", GATE_API_KEY);

    String bluetoothCode = cod + ":" + eventTypeFromSens(sens);
    String body = "{\"bluetoothCode\":\"" + bluetoothCode + "\"}";
    Serial.println("[WiFi] Body trimis: " + body);
    int code = http.POST(body);

    bool valid = false;
    if (code == 200) {
        String r = http.getString();
        Serial.println("[WiFi] Raspuns validare: " + r);
        valid = (r.indexOf("\"authorized\":true") >= 0 || r.indexOf("\"authorized\": true") >= 0);
    } else {
        Serial.println("[WiFi] Eroare HTTP validare: " + String(code));
    }
    http.end();
    return valid;
}

void wifiTrimiteStatus(String hardwareState, String hardwareLed) {
    if (WiFi.status() != WL_CONNECTED) { wifiSetup(); }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Nu pot trimite status - offline!");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, String(CLOUD_API_BASE_URL) + "/hardware/update-status");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Gate-Api-Key", GATE_API_KEY);

    String body = String("{\"hardwareState\":\"") + hardwareState + "\","
                + "\"hardwareLed\":\"" + hardwareLed + "\","
                + "\"lastEventAt\":\"" + getTimestamp() + "\"}";

    Serial.println("[WiFi] Status: " + body);
    int code = http.POST(body);
    Serial.println("[WiFi] Status HTTP: " + String(code));
    http.end();
}

// ═══════════════════════════════════════════════════════════════════
//  SERVO
// ═══════════════════════════════════════════════════════════════════
void servoRidica() {
    Serial.println("[SERVO] Ridic bratul...");
    int pozCurenta = SERVO_INCHIS;
    while (pozCurenta < SERVO_DESCHIS) {
        pozCurenta++;
        servoPoarta.write(pozCurenta);
        ledS1Intermitent();
        delay(SERVO_PAS_DELAY_MS);
    }
    Serial.println("[SERVO] Brat ridicat (limitator superior activ).");
}

// Returneaza true daca bratul a fost coborat complet,
// false daca a detectat un vehicul la bariera de start si a reridicat bratul.
bool servoCoboaraNormal() {
    Serial.println("[SERVO] Cobor bratul...");
    int pozCurenta = SERVO_DESCHIS;
    while (pozCurenta > SERVO_INCHIS) {
        // Verifica bariera de start in timpul coborarii
        if (barieraStartBlocata()) {
            Serial.println("[SERVO] Obstacol detectat in timpul inchiderii! Ridic bara...");
            // Ridica bara inapoi la pozitia deschisa
            while (pozCurenta < SERVO_DESCHIS) {
                pozCurenta++;
                servoPoarta.write(pozCurenta);
                ledS1Intermitent();
                delay(SERVO_PAS_DELAY_MS);
            }
            Serial.println("[SERVO] Bara reridica (revenire la S2).");
            return false;
        }
        pozCurenta--;
        servoPoarta.write(pozCurenta);
        leduriOpresteTot();
        digitalWrite(PIN_LED_ALBASTRU, HIGH); // LED albastru in timpul inchiderii
        delay(SERVO_PAS_DELAY_MS);
    }
    Serial.println("[SERVO] Brat coborat (limitator inferior activ).");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  BLE Callbacks
// ═══════════════════════════════════════════════════════════════════
class PoartaServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s) override {
        bleDeviceConnected = true;
        Serial.println("[BLE] Telefon conectat.");
    }
    void onDisconnect(BLEServer* s) override {
        bleDeviceConnected = false;
        Serial.println("[BLE] Telefon deconectat.");
        // Nu repornim advertising automat — il pornim doar cand detectam vehicul
    }
};

class PoartaCharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        String val = c->getValue().c_str();
        val.trim();
        if (val.length() > 0) {
            bleCodReceptat = val;
            bleCodPrimit   = true;
            Serial.println("[BLE] Cod primit: " + val);
        }
    }
};

void bleSetup() {
    BLEDevice::init("ESP32_Poarta");
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new PoartaServerCallbacks());

    BLEService* service = bleServer->createService(SERVICE_UUID);
    bleChar = service->createCharacteristic(CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    bleChar->setCallbacks(new PoartaCharCallbacks());
    bleChar->addDescriptor(new BLE2902());

    service->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    // Nu pornim advertising aici — il pornim doar cand detectam vehicul

    Serial.println("[BLE] Initializat — ESP32_Poarta");
}

// ═══════════════════════════════════════════════════════════════════
//  ASTEAPTA COD BLE
// ═══════════════════════════════════════════════════════════════════
String asteaptaCodBLE() {
    Serial.println("[BLE] Astept cod... (2m timeout)");
    bleCodPrimit   = false;
    bleCodReceptat = "";

    unsigned long start = millis();
    while (millis() - start < TIMEOUT_BLE_MS) {
        bool ledOn = ((millis() - start) % 500) < 250;
        leduriOpresteTot();
        digitalWrite(PIN_LED_GALBEN, ledOn ? HIGH : LOW);
        if (bleCodPrimit) return bleCodReceptat;
        delay(10);
    }

    Serial.println("[BLE] Timeout!");
    return "";
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(1000);

    pinMode(PIN_LED_VERDE,    OUTPUT);
    pinMode(PIN_LED_ROSU,     OUTPUT);
    pinMode(PIN_LED_GALBEN,   OUTPUT);
    pinMode(PIN_LED_ALBASTRU, OUTPUT);
    leduriOpresteTot();

    bariereSetup();

    ESP32PWM::allocateTimer(0);
    servoPoarta.setPeriodHertz(50);
    servoPoarta.attach(PIN_SERVO, 500, 2400);
    servoPoarta.write(SERVO_INCHIS);

    bleSetup();
    wifiSetup();
    ntpSetup();

    starePoarta = S0_INCHISA;
    Serial.println("\n=== SISTEM GESTIUNE ACCES PORNIT ===\n");
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP — masina de stari FSM
// ═══════════════════════════════════════════════════════════════════
void loop() {
    switch (starePoarta) {

        // ── S0: INCHISA — stare de veghe ────────────────────────
        case S0_INCHISA: {
            ledS0();

            // Asteapta prima schimbare pe oricare bariera
            // B1 blocata prima → intrare, B2 blocata prima → iesire
            if (!b1Blocata() && !b2Blocata()) { delay(10); break; }

            if (b1Blocata()) {
                sensulCurent = "intrare";
            } else {
                sensulCurent = "iesire";
            }

            Serial.println("[FSM] S0 → detectat vehicul, sens: " + sensulCurent);

            // Porneste BLE advertising — telefoanele pot acum sa se conecteze
            bleCodPrimit   = false;
            bleCodReceptat = "";
            BLEDevice::startAdvertising();
            Serial.println("[BLE] Advertising pornit.");

            // Asteapta cod BLE de la telefon
            String cod = asteaptaCodBLE();

            // Opreste advertising — nu mai acceptam conexiuni noi
            BLEDevice::getAdvertising()->stop();
            Serial.println("[BLE] Advertising oprit.");

            if (cod == "") {
                Serial.println("[FSM] Niciun cod — refuzat.");
                ledS4();
                wifiTrimiteStatus("Inchisa", "Rosu");
                delay(3000);
                starePoarta = S0_INCHISA;
                break;
            }

            bool valid = wifiValideazaCod(cod, sensulCurent);
            wifiTrimiteStatus(valid ? "In curs de deschidere" : "Inchisa", valid ? "Verde" : "Rosu");

            if (!valid) {
                Serial.println("[FSM] Cod invalid — refuzat.");
                ledS4();
                delay(3000);
                starePoarta = S0_INCHISA;
                break;
            }

            Serial.println("[FSM] S0 → S1 (autorizat)");
            starePoarta = S1_IN_CURS_DESCHIDERE;
            break;
        }

        // ── S1: IN CURS DE DESCHIDERE — motor ridica bratul ─────
        case S1_IN_CURS_DESCHIDERE: {
            Serial.println("[FSM] S1 — ridic bratul");
            int pozCurenta = SERVO_INCHIS;
            bool masinaATrecut = false;

            while (pozCurenta < SERVO_DESCHIS) {
                pozCurenta++;
                servoPoarta.write(pozCurenta);
                ledS1Intermitent();
                delay(SERVO_PAS_DELAY_MS);

                if (barieraFinalBlocata()) {
                    masinaATrecut = true;
                    break;
                }
            }

            // Termina de ridicat chiar daca masina a trecut deja
            while (pozCurenta < SERVO_DESCHIS) {
                pozCurenta++;
                servoPoarta.write(pozCurenta);
                delay(SERVO_PAS_DELAY_MS);
            }

            wifiTrimiteStatus("Deschisa", "Verde");

            if (masinaATrecut) {
                Serial.println("[FSM] S1 → S3 (vehicul a trecut in timpul deschiderii)");
                wifiTrimiteStatus("Siguranta IR", "Albastru");
                starePoarta = S3_SIGURANTA_IR;
            } else {
                Serial.println("[FSM] S1 → S2 (limitator superior)");
                starePoarta = S2_DESCHISA;
            }
            break;
        }

        // ── S2: DESCHISA — asteapta vehicul sa intre in zona ────
        case S2_DESCHISA: {
            ledS2();

            // T3: bariera de start blocata → vehicul a intrat in zona → S3
            if (barieraFinalBlocata()) {
                Serial.println("[FSM] S2 → S3 (vehicul detectat, sens: " + sensulCurent + ")");
                wifiTrimiteStatus("Siguranta IR", "Albastru");
                starePoarta = S3_SIGURANTA_IR;
            }
            delay(10);
            break;
        }

        // ── S3: SIGURANTA IR — asteapta ca vehiculul sa treaca complet ──
        case S3_SIGURANTA_IR: {
            ledS3();

            // Re-trigger: daca bariera de START detecteaza o noua masina
            // (in timp ce bariera finala e inca blocata sau imediat dupa),
            // bara ramane deschisa si revenim in S2 sa asteptam trecerea ei.
            if (barieraStartBlocata()) {
                Serial.println("[FSM] S3 → S2 (noua masina detectata la bariera de start)");
                wifiTrimiteStatus("Deschisa", "Verde");
                starePoarta = S2_DESCHISA;
                break;
            }

            // Asteapta ca bariera FINALA sa se elibereze
            // intrare: B2 (masina a trecut de bara servo)
            // iesire:  B1 (masina a trecut de bara servo in sens invers)
            if (barieraFinalBlocata()) { delay(10); break; }

            // T4: bariera finala libera → timer fix 5s → S4
            Serial.println("[FSM] Bariera finala libera, astept 5s...");
            unsigned long freeStart = millis();
            while (millis() - freeStart < TIMEOUT_IR_LIBER_MS) {
                ledS3();
                // Re-trigger si in timpul timerului de 5s
                if (barieraStartBlocata()) {
                    Serial.println("[FSM] S3 → S2 (noua masina detectata in fereastra 5s)");
                    wifiTrimiteStatus("Deschisa", "Verde");
                    starePoarta = S2_DESCHISA;
                    break;
                }
                delay(10);
            }
            if (starePoarta == S2_DESCHISA) break;

            Serial.println("[FSM] S3 → S4 (bariera finala libera + 5s)");
            wifiTrimiteStatus("In curs de inchidere", "Rosu");
            starePoarta = S4_IN_CURS_INCHIDERE;
            break;
        }

        // ── S4: IN CURS DE INCHIDERE — motor coboara bratul ─────
        case S4_IN_CURS_INCHIDERE: {
            Serial.println("[FSM] S4 — cobor bratul");
            bool inchisComplet = servoCoboaraNormal();

            if (!inchisComplet) {
                // Bariera de start detectata in timpul coborarii — bara a fost reridica automat
                Serial.println("[FSM] S4 → S2 (vehicul detectat in timpul inchiderii)");
                wifiTrimiteStatus("Deschisa", "Verde");
                starePoarta = S2_DESCHISA;
                break;
            }

            Serial.println("[FSM] S4 → S0 (limitator inferior)");
            wifiTrimiteStatus("Inchisa", "Rosu");
            starePoarta = S0_INCHISA;
            break;
        }
    }
}
