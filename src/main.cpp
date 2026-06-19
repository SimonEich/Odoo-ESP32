#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ── config ────────────────────────────────────────────────────────────────────
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

#define ODOO_HOST        "https://your-odoo-instance.com"  // no trailing slash
#define ODOO_DB          "your_database_name"
#define ODOO_USER        "your@email.com"
#define ODOO_PASSWORD    "your_odoo_password"

#define PN532_IRQ_PIN    4   // PN532 IRQ     → ESP32 GPIO 4
#define PN532_RESET_PIN  5   // PN532 RSTPD_N → ESP32 GPIO 5
#define LED_PIN          2   // built-in LED on most ESP32 dev boards

// ── globals ───────────────────────────────────────────────────────────────────
Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RESET_PIN);
static int g_uid = -1;   // Odoo XML-RPC user id after authenticate()

// ── helpers ───────────────────────────────────────────────────────────────────

static String uidToHex(uint8_t* uid, uint8_t len)
{
    String s;
    s.reserve(len * 2);
    for (uint8_t i = 0; i < len; i++) {
        if (uid[i] < 0x10) s += '0';
        s += String(uid[i], HEX);
    }
    s.toUpperCase();
    return s;
}

static String nowUtc()
{
    time_t now;
    time(&now);
    struct tm* t = gmtime(&now);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    return String(buf);
}

static String xmlrpcPost(const String& endpoint, const String& body)
{
    HTTPClient http;
    http.begin(ODOO_HOST + endpoint);
    http.addHeader("Content-Type", "text/xml");
    http.setTimeout(8000);
    int code = http.POST(body);
    if (code != 200) {
        Serial.printf("[HTTP] POST %s failed, code=%d\n", endpoint.c_str(), code);
        http.end();
        return "";
    }
    String resp = http.getString();
    http.end();
    return resp;
}

static int extractInt(const String& xml, int afterPos = 0)
{
    for (const char* tag : {"<int>", "<i4>"}) {
        int pos = xml.indexOf(tag, afterPos);
        if (pos != -1) {
            int end = xml.indexOf('<', pos + strlen(tag));
            if (end != -1)
                return xml.substring(pos + strlen(tag), end).toInt();
        }
    }
    return -1;
}

static String extractString(const String& xml, int afterPos = 0)
{
    int pos = xml.indexOf("<string>", afterPos);
    if (pos == -1) return "";
    int end = xml.indexOf("</string>", pos);
    if (end == -1) return "";
    return xml.substring(pos + 8, end);
}

static void blink(int times, int onMs = 120, int offMs = 100)
{
    for (int i = 0; i < times; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(onMs);
        digitalWrite(LED_PIN, LOW);
        if (i < times - 1) delay(offMs);
    }
}

// ── Odoo XML-RPC ──────────────────────────────────────────────────────────────

static bool odooAuth()
{
    String body =
        "<?xml version='1.0'?>"
        "<methodCall><methodName>authenticate</methodName><params>"
        "<param><value><string>" ODOO_DB "</string></value></param>"
        "<param><value><string>" ODOO_USER "</string></value></param>"
        "<param><value><string>" ODOO_PASSWORD "</string></value></param>"
        "<param><value><struct/></value></param>"
        "</params></methodCall>";

    String resp = xmlrpcPost("/xmlrpc/2/common", body);
    if (resp.isEmpty()) return false;

    g_uid = extractInt(resp);
    if (g_uid <= 0) {
        Serial.println("[Odoo] Auth failed – check credentials");
        return false;
    }
    Serial.printf("[Odoo] Authenticated (uid=%d)\n", g_uid);
    return true;
}

static int findEmployeeByBadge(const String& cardUid, String& outName)
{
    String body = String(
        "<?xml version='1.0'?>"
        "<methodCall><methodName>execute_kw</methodName><params>"
        "<param><value><string>" ODOO_DB "</string></value></param>"
        "<param><value><int>") + g_uid + "</int></value></param>"
        "<param><value><string>" ODOO_PASSWORD "</string></value></param>"
        "<param><value><string>hr.employee</string></value></param>"
        "<param><value><string>search_read</string></value></param>"
        "<param><value><array><data><value><array><data>"
          "<value><array><data>"
            "<value><string>barcode</string></value>"
            "<value><string>=</string></value>"
            "<value><string>" + cardUid + "</string></value>"
          "</data></array></value>"
        "</data></array></value></data></array></value></param>"
        "<param><value><struct>"
          "<member><name>fields</name><value><array><data>"
            "<value><string>id</string></value>"
            "<value><string>name</string></value>"
          "</data></array></value></member>"
          "<member><name>limit</name><value><int>1</int></value></member>"
        "</struct></value></param>"
        "</params></methodCall>";

    String resp = xmlrpcPost("/xmlrpc/2/object", body);
    if (resp.isEmpty() || resp.indexOf("<name>id</name>") == -1) return -1;

    int idPos = resp.indexOf("<name>id</name>");
    int employeeId = extractInt(resp, idPos);

    int namePos = resp.indexOf("<name>name</name>");
    if (namePos != -1) outName = extractString(resp, namePos);

    return employeeId;
}

static int findOpenAttendance(int employeeId)
{
    String body = String(
        "<?xml version='1.0'?>"
        "<methodCall><methodName>execute_kw</methodName><params>"
        "<param><value><string>" ODOO_DB "</string></value></param>"
        "<param><value><int>") + g_uid + "</int></value></param>"
        "<param><value><string>" ODOO_PASSWORD "</string></value></param>"
        "<param><value><string>hr.attendance</string></value></param>"
        "<param><value><string>search_read</string></value></param>"
        "<param><value><array><data><value><array><data>"
          "<value><array><data>"
            "<value><string>employee_id</string></value>"
            "<value><string>=</string></value>"
            "<value><int>" + String(employeeId) + "</int></value>"
          "</data></array></value>"
          "<value><array><data>"
            "<value><string>check_out</string></value>"
            "<value><string>=</string></value>"
            "<value><boolean>0</boolean></value>"
          "</data></array></value>"
        "</data></array></value></data></array></value></param>"
        "<param><value><struct>"
          "<member><name>fields</name><value><array><data>"
            "<value><string>id</string></value>"
          "</data></array></value></member>"
          "<member><name>limit</name><value><int>1</int></value></member>"
        "</struct></value></param>"
        "</params></methodCall>";

    String resp = xmlrpcPost("/xmlrpc/2/object", body);
    if (resp.isEmpty() || resp.indexOf("<name>id</name>") == -1) return -1;
    return extractInt(resp, resp.indexOf("<name>id</name>"));
}

static int odooCheckIn(int employeeId)
{
    String body = String(
        "<?xml version='1.0'?>"
        "<methodCall><methodName>execute_kw</methodName><params>"
        "<param><value><string>" ODOO_DB "</string></value></param>"
        "<param><value><int>") + g_uid + "</int></value></param>"
        "<param><value><string>" ODOO_PASSWORD "</string></value></param>"
        "<param><value><string>hr.attendance</string></value></param>"
        "<param><value><string>create</string></value></param>"
        "<param><value><array><data><value><struct>"
          "<member><name>employee_id</name><value><int>" + String(employeeId) + "</int></value></member>"
          "<member><name>check_in</name><value><string>" + nowUtc() + "</string></value></member>"
        "</struct></value></data></array></value></param>"
        "<param><value><struct/></value></param>"
        "</params></methodCall>";

    return extractInt(xmlrpcPost("/xmlrpc/2/object", body));
}

static bool odooCheckOut(int attendanceId)
{
    String body = String(
        "<?xml version='1.0'?>"
        "<methodCall><methodName>execute_kw</methodName><params>"
        "<param><value><string>" ODOO_DB "</string></value></param>"
        "<param><value><int>") + g_uid + "</int></value></param>"
        "<param><value><string>" ODOO_PASSWORD "</string></value></param>"
        "<param><value><string>hr.attendance</string></value></param>"
        "<param><value><string>write</string></value></param>"
        "<param><value><array><data>"
          "<value><array><data><value><int>" + String(attendanceId) + "</int></value></data></array></value>"
          "<value><struct>"
            "<member><name>check_out</name><value><string>" + nowUtc() + "</string></value></member>"
          "</struct></value>"
        "</data></array></value></param>"
        "<param><value><struct/></value></param>"
        "</params></methodCall>";

    return xmlrpcPost("/xmlrpc/2/object", body).indexOf("<boolean>1</boolean>") != -1;
}

// ── card logic ────────────────────────────────────────────────────────────────

static void processCard(const String& cardUid)
{
    Serial.printf("[Card] UID: %s\n", cardUid.c_str());

    if (g_uid <= 0 && !odooAuth()) {
        Serial.println("[Card] Cannot reach Odoo");
        blink(5, 50, 50);
        return;
    }

    String employeeName;
    int employeeId = findEmployeeByBadge(cardUid, employeeName);
    if (employeeId <= 0) {
        Serial.printf("[Card] No employee with Badge ID '%s'\n"
                      "       → Odoo > Employees > [name] > Private Info > Badge ID = %s\n",
                      cardUid.c_str(), cardUid.c_str());
        blink(3, 300, 100);
        return;
    }
    Serial.printf("[Card] Employee: %s (id=%d)\n", employeeName.c_str(), employeeId);

    int openId = findOpenAttendance(employeeId);
    if (openId == -1) {
        int newId = odooCheckIn(employeeId);
        if (newId > 0) {
            Serial.printf("[✓] %s checked IN  (record %d)  %s UTC\n",
                          employeeName.c_str(), newId, nowUtc().c_str());
            blink(1, 400);
        } else {
            Serial.println("[✗] Check-in failed");
            blink(5, 50, 50);
        }
    } else {
        if (odooCheckOut(openId)) {
            Serial.printf("[✓] %s checked OUT  %s UTC\n",
                          employeeName.c_str(), nowUtc().c_str());
            blink(2, 200, 100);
        } else {
            Serial.println("[✗] Check-out failed");
            blink(5, 50, 50);
        }
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    delay(500);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // WiFi
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
    Serial.printf("\nWiFi: %s\n", WiFi.localIP().toString().c_str());

    // NTP
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("NTP sync");
    time_t now = 0;
    while (now < 1000000000UL) { delay(500); Serial.print('.'); time(&now); }
    Serial.println("\nTime: " + nowUtc() + " UTC");

    // Odoo
    if (!odooAuth())
        Serial.println("⚠  Odoo auth failed – check credentials above");

    // PN532
    Wire.begin();
    nfc.begin();
    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println("⚠  PN532 not found – check wiring & I2C DIP switches");
        while (1) { blink(3, 100, 100); delay(1000); }
    }
    Serial.printf("[PN532] PN5%02X firmware v%d.%d\n",
                  (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
    nfc.SAMConfig();

    Serial.println("\nReady – hold card to reader");
}

void loop()
{
    uint8_t uid[7];
    uint8_t uidLen = 0;

    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 100)) {
        processCard(uidToHex(uid, uidLen));
        delay(2000);  // debounce: ignore same card held on reader
    }
}
