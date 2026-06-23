// ─────────────────────────────────────────────────────────────────────────────
//  beacon_share.cpp — captive-portal "scan to download" mode
//
//  No external libraries needed: the QR encoder is the ESP-IDF "qrcode"
//  component bundled with arduino-esp32 (<qrcode.h> → esp_qrcode_*). WiFi /
//  WebServer / DNSServer / LittleFS ship with the core too.
//
//  Note: do NOT add Richard Moore's "QRCode" Library-Manager library — its
//  header is also named qrcode.h and collides with this bundled one.
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <qrcode.h>   // ESP-IDF bundled QR encoder (esp_qrcode_*)

#include "beacon_share.h"
#include "wifi_rings.h"   // IMG_W, IMG_H

static const uint16_t S_BLACK = 0x0000;
static const uint16_t S_WHITE = 0xFFFF;

static WebServer       s_http(80);
static DNSServer       s_dns;
static const uint16_t  DNS_PORT = 53;
static const IPAddress s_ap_ip(192, 168, 4, 1);

// ─── filename safety — only "beacon_*.png", no path traversal ──────────────────
static bool safe_name(const String& f) {
    if (!f.startsWith("beacon_") || !f.endsWith(".png")) return false;
    if (f.indexOf('/') >= 0 || f.indexOf('\\') >= 0 || f.indexOf("..") >= 0) return false;
    return true;
}

// strip any leading directory from a LittleFS entry name (core-version dependent)
static String basename_of(const String& name) {
    int sl = name.lastIndexOf('/');
    return (sl >= 0) ? name.substring(sl + 1) : name;
}

// ─── HTTP: gallery (chunked so a long file list never needs a big String) ──────
static void handle_root() {
    s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_http.send(200, "text/html", "");
    s_http.sendContent(
        "<!doctype html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>beacon</title>"
        "<style>body{font-family:sans-serif;margin:1em;background:#111;color:#eee}"
        "a{color:#6cf;display:block;padding:.7em;border:1px solid #333;margin:.35em 0;"
        "border-radius:8px;text-decoration:none}h1{font-size:1.2em}</style></head>"
        "<body><h1>beacon &mdash; saved images</h1>");

    int n = 0;
    File root = LittleFS.open("/");
    if (root) {
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            String base = basename_of(String(f.name()));
            if (base.startsWith("beacon_") && base.endsWith(".png")) {
                s_http.sendContent("<a href='/dl?f=" + base + "'>" + base +
                                   " &middot; " + String((unsigned)(f.size() / 1024)) +
                                   " KB</a>");
                n++;
            }
        }
    }
    if (n == 0) s_http.sendContent("<p>no images yet</p>");
    s_http.sendContent("</body></html>");
    s_http.sendContent("");   // end chunked response
}

// ─── HTTP: download one image ──────────────────────────────────────────────────
static void handle_dl() {
    String f = s_http.arg("f");
    if (!safe_name(f)) { s_http.send(404, "text/plain", "not found"); return; }
    File file = LittleFS.open("/" + f, "r");
    if (!file) { s_http.send(404, "text/plain", "not found"); return; }
    s_http.sendHeader("Content-Disposition", "attachment; filename=" + f);
    s_http.streamFile(file, "application/octet-stream");
    file.close();
}

// ─── HTTP: captive-portal catch-all → bounce to the gallery ────────────────────
static void handle_portal() {
    s_http.sendHeader("Location", String("http://") + s_ap_ip.toString() + "/", true);
    s_http.send(302, "text/plain", "");
}

// ─── draw the WiFi-join QR + instructions ──────────────────────────────────────
// The ESP-IDF encoder is callback-based: esp_qrcode_generate() encodes the text
// then invokes display_func with a handle to read modules from. Its signature
// carries no user pointer, so the target display and the rendered size are
// passed through file-scope statics.
static Arduino_GFX* s_qr_gfx = nullptr;
static int          s_qr_qpx = 0;       // rendered QR side in px, for text layout
static const int    QR_SCALE = 5;
static const int    QR_OY    = 14;      // top margin of the QR block

static void qr_render_cb(esp_qrcode_handle_t qr) {
    const int modules = esp_qrcode_get_size(qr);
    const int qpx     = modules * QR_SCALE;
    const int qz      = QR_SCALE * 2;            // quiet zone
    const int ox      = (IMG_W - qpx) / 2;
    s_qr_qpx = qpx;

    s_qr_gfx->fillScreen(S_BLACK);
    s_qr_gfx->fillRect(ox - qz, QR_OY - qz, qpx + qz * 2, qpx + qz * 2, S_WHITE);
    for (int y = 0; y < modules; y++)
        for (int x = 0; x < modules; x++)
            if (esp_qrcode_get_module(qr, x, y))
                s_qr_gfx->fillRect(ox + x * QR_SCALE, QR_OY + y * QR_SCALE,
                                   QR_SCALE, QR_SCALE, S_BLACK);
}

static void draw_qr(Arduino_GFX* gfx, const char* text, const char* ssid) {
    s_qr_gfx = gfx;
    s_qr_qpx = 0;

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func       = qr_render_cb;
    cfg.max_qrcode_version = 4;                  // WiFi-join string fits well under v4
    cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
    if (esp_qrcode_generate(&cfg, text) != ESP_OK || s_qr_qpx == 0) {
        gfx->fillScreen(S_BLACK);
        gfx->setTextColor(S_WHITE, S_BLACK);
        gfx->setCursor(6, 20);
        gfx->print("QR encode failed");
        return;
    }

    const int qz = QR_SCALE * 2;
    const int ty = QR_OY + s_qr_qpx + qz + 6;
    gfx->setTextColor(S_WHITE, S_BLACK);
    gfx->setTextSize(1);
    gfx->setCursor(6, ty);      gfx->print("Scan to join WiFi:");
    gfx->setCursor(6, ty + 12); gfx->print(ssid);
    gfx->setCursor(6, ty + 28); gfx->print("then open http://192.168.4.1");
}

// ─── public entry point ─────────────────────────────────────────────────────────
void run_share_mode(Arduino_GFX* gfx, int button_pin) {
    Serial.println("share mode");

    // Per-device SSID from the low 16 bits of the chip MAC.
    char ssid[20];
    uint64_t cid = ESP.getEfuseMac();
    snprintf(ssid, sizeof(ssid), "beacon-%04X", (unsigned)(uint16_t)cid);

    // Open SoftAP at a fixed IP.
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(s_ap_ip, s_ap_ip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid);
    delay(100);

    // Captive portal: resolve every DNS query to us.
    s_dns.start(DNS_PORT, "*", s_ap_ip);

    s_http.on("/", handle_root);
    s_http.on("/dl", handle_dl);
    s_http.onNotFound(handle_portal);       // OS portal probes land here → gallery
    s_http.begin();

    // WiFi-join QR (open network).
    char qrtext[48];
    snprintf(qrtext, sizeof(qrtext), "WIFI:T:nopass;S:%s;;", ssid);
    draw_qr(gfx, qrtext, ssid);
    Serial.printf("AP '%s' up — http://%s/\n", ssid, s_ap_ip.toString().c_str());

    // Make sure the triggering long-press is released before we watch for exit.
    while (digitalRead(button_pin) == LOW) delay(5);
    delay(50);

    // Serve until the button is pressed again.
    while (true) {
        s_dns.processNextRequest();
        s_http.handleClient();
        if (digitalRead(button_pin) == LOW) {
            delay(30);
            if (digitalRead(button_pin) == LOW) {
                while (digitalRead(button_pin) == LOW) delay(5);
                break;
            }
        }
        delay(2);
    }

    s_http.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("share mode end");
}
