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
static const uint16_t S_GREEN = 0x3FE2;   // OSINT green (#39ff14) in RGB565

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
// OSINT terminal aesthetic. All markup/CSS is a flash-resident const string
// streamed in chunks — costs flash, negligible RAM. Two directory passes keep
// memory flat: one to count, one to emit rows.
static void handle_root() {
    s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_http.send(200, "text/html", "");

    s_http.sendContent(
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>beacon // archive</title><style>"
        ":root{--g:#39ff14;--d:#0a0f0a;--dim:#162416}"
        "*{box-sizing:border-box}"
        "body{font-family:ui-monospace,Menlo,Consolas,monospace;margin:0;"
        "background:var(--d);color:var(--g);padding:16px;line-height:1.4;"
        "text-shadow:0 0 4px rgba(57,255,20,.45)}"
        "h1{font-size:1em;letter-spacing:2px;margin:0 0 2px;"
        "border-bottom:1px solid var(--dim);padding-bottom:8px}"
        ".sub{color:#7aff6a;opacity:.65;font-size:.72em;margin:6px 0 14px}"
        ".bar{height:10px;background:var(--dim);border:1px solid var(--g);"
        "border-radius:2px;overflow:hidden}"
        ".fill{height:100%;background:var(--g);box-shadow:0 0 8px var(--g)}"
        ".meta{font-size:.72em;opacity:.85;margin:6px 0 16px;letter-spacing:1px}"
        ".row{display:flex;align-items:center;gap:8px;border:1px solid var(--dim);"
        "padding:8px 10px;margin:6px 0;border-radius:4px;background:rgba(57,255,20,.03)}"
        ".nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:.8em}"
        "a.nm{color:var(--g);text-decoration:none;border-bottom:1px dotted var(--dim)}"
        "a.nm:hover{border-bottom-color:var(--g)}"
        ".sz{opacity:.55;font-size:.68em}"
        ".btn{font-family:inherit;font-size:.7em;text-decoration:none;padding:5px 9px;"
        "border:1px solid var(--g);border-radius:3px;color:var(--g);background:transparent;"
        "cursor:pointer;letter-spacing:1px}"
        ".btn:hover{background:var(--g);color:#000}"
        ".del{border-color:#ff4444;color:#ff6a6a}"
        ".del:hover{background:#ff4444;color:#000}"
        ".actions{display:flex;align-items:center;gap:10px;margin:0 0 12px}"
        ".grow{flex:1}"
        ".selall{font-size:.7em;opacity:.85;display:flex;align-items:center;gap:6px;letter-spacing:1px}"
        "input[type=checkbox]{accent-color:#39ff14;width:17px;height:17px;flex:none}"
        ".empty{opacity:.5;font-style:italic;margin-top:24px}"
        "</style></head><body>"
        "<h1>&#9586; BEACON // RECON ARCHIVE</h1>"
        "<div class=sub>captive node 192.168.4.1 &middot; pixel-sort wifi beacons</div>");

    // ── storage gauge ──
    const size_t total = LittleFS.totalBytes();
    const size_t used  = LittleFS.usedBytes();
    const int    pct   = total ? (int)((used * 100) / total) : 0;

    // pass 1 — count images
    int n = 0;
    File root = LittleFS.open("/");
    if (root) {
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            String base = basename_of(String(f.name()));
            if (base.startsWith("beacon_") && base.endsWith(".png")) n++;
        }
        root.close();
    }

    s_http.sendContent("<div class=bar><div class=fill style='width:" + String(pct) + "%'></div></div>");
    s_http.sendContent("<div class=meta>STORAGE " + String(pct) + "% &middot; " +
                       String((unsigned)(used / 1024)) + "/" + String((unsigned)(total / 1024)) +
                       " KB &middot; " + String(n) + " IMG</div>");

    // pass 2 — rows inside a POST form so multi-select delete needs no JS
    // (captive-portal mini-browsers block JS navigation; plain forms work).
    if (n > 0) {
        // GET form, two submit buttons via formaction → bulk DOWNLOAD or DELETE of
        // the checked rows. No JS gate (iOS captive browser blocks confirm()/JS),
        // so the actions always fire on mobile. Select-all is a JS nicety only.
        s_http.sendContent(
            "<form method=GET>"
            "<div class=actions>"
            "<label class=selall><input type=checkbox "
            "onclick=\"for(var c of document.querySelectorAll('.cb'))c.checked=this.checked\">ALL</label>"
            "<span class=grow></span>"
            "<button class=btn type=submit formaction='/dl'>DOWNLOAD</button>"
            "<button class='btn del' type=submit formaction='/del'>DELETE</button>"
            "</div>");

        root = LittleFS.open("/");
        if (root) {
            for (File f = root.openNextFile(); f; f = root.openNextFile()) {
                String base = basename_of(String(f.name()));
                if (!base.startsWith("beacon_") || !base.endsWith(".png")) continue;
                s_http.sendContent(
                    "<div class=row>"
                    "<input type=checkbox class=cb name=sel value='" + base + "'>"
                    "<a class=nm href='/view?f=" + base + "'>" + base + "</a>"
                    "<span class=sz>" + String((unsigned)(f.size() / 1024)) + "K</span>"
                    "<a class=btn href='/view?f=" + base + "'>PREVIEW</a>"
                    "</div>");
            }
            root.close();
        }
        s_http.sendContent("</form>");
    } else {
        s_http.sendContent("<div class=empty>// no captures on node</div>");
    }

    s_http.sendContent("</body></html>");
    s_http.sendContent("");   // end chunked response
}

// ─── HTTP: delete every checked image (form POST: one or more `sel=NAME`) ───────
static void handle_del() {
    int removed = 0;
    for (int i = 0; i < s_http.args(); i++) {
        if (s_http.argName(i) != "sel") continue;
        String f = s_http.arg(i);
        if (!safe_name(f)) continue;
        if (LittleFS.remove("/" + f)) { removed++; Serial.printf("deleted %s\n", f.c_str()); }
    }
    Serial.printf("delete: removed %d file(s)\n", removed);
    // Render the refreshed gallery in-place rather than 303-redirecting: the iOS
    // captive browser doesn't reliably follow redirects, so this guarantees the
    // user immediately sees the updated list as confirmation.
    handle_root();
}

// ─── HTTP: inline PNG (for the in-browser preview) ─────────────────────────────
static void handle_img() {
    String f = s_http.arg("f");
    if (!safe_name(f)) { s_http.send(404, "text/plain", "not found"); return; }
    File file = LittleFS.open("/" + f, "r");
    if (!file) { s_http.send(404, "text/plain", "not found"); return; }
    s_http.streamFile(file, "image/png");    // inline content-type → browser renders it
    file.close();
}

// ─── preview page — full image inline + iOS "Save to Photos" hint (shared) ──────
// The image is served inline (image/png), so a phone long-press gives the native
// "Save to Photos" sheet — the reliable way to save off a captive-portal AP,
// where forced downloads just render as scrambled bytes.
static void render_view(const String& f) {
    s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_http.send(200, "text/html", "");
    s_http.sendContent(
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>" + f + "</title><style>"
        "body{font-family:ui-monospace,Menlo,Consolas,monospace;background:#0a0f0a;"
        "color:#39ff14;margin:0;padding:16px;text-align:center;"
        "text-shadow:0 0 4px rgba(57,255,20,.45)}"
        ".nm{font-size:.72em;opacity:.8;word-break:break-all;letter-spacing:1px}"
        "img{max-width:100%;image-rendering:pixelated;border:1px solid #39ff14;"
        "box-shadow:0 0 14px rgba(57,255,20,.4);margin:14px 0}"
        ".hint{font-size:.72em;opacity:.65;margin:4px 0 16px;letter-spacing:1px}"
        ".btn{display:inline-block;text-decoration:none;border:1px solid #39ff14;"
        "color:#39ff14;padding:7px 14px;border-radius:3px;margin:4px;font-size:.75em;"
        "letter-spacing:1px}.btn:hover{background:#39ff14;color:#000}"
        "</style></head><body>"
        "<div class=nm>" + f + "</div>"
        "<img src='/img?f=" + f + "'>"
        "<div class=hint>press and hold to save</div>"
        "<div><a class=btn href='/'>&#8592; BACK TO ARCHIVE</a></div>"
        "</body></html>");
    s_http.sendContent("");
}

static void handle_view() {
    String f = s_http.arg("f");
    if (!safe_name(f)) { s_http.send(404, "text/plain", "not found"); return; }
    render_view(f);
}

// ─── selection helpers ──────────────────────────────────────────────────────────
#define SEL_MAX 64

// Gather requested filenames: a single ?f=NAME (preview/direct links) plus any
// number of sel=NAME (the gallery form checkboxes). Only safe names are kept.
static int collect_sel(String* out) {
    int c = 0;
    String f = s_http.arg("f");
    if (f.length() && safe_name(f) && c < SEL_MAX) out[c++] = f;
    for (int i = 0; i < s_http.args() && c < SEL_MAX; i++) {
        if (s_http.argName(i) != "sel") continue;
        String s = s_http.arg(i);
        if (safe_name(s)) out[c++] = s;
    }
    return c;
}

// CRC-32 (zip/PNG poly), bitwise — no table, no deps.
static uint32_t zip_crc32(uint32_t crc, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc;
}
static inline void put_le(uint8_t* p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Stream the named files as ONE uncompressed ("stored") .zip — lets a phone pull
// a whole selection in a single download, and iOS Files can expand a stored zip.
// Content-Length is precomputed so the body is a plain (non-chunked) byte stream.
static void stream_zip(String* names, int n) {
    uint32_t fsize[SEL_MAX], fcrc[SEL_MAX], foff[SEL_MAX];
    uint8_t  buf[1024];

    uint32_t total = 0, cdsize = 0;
    for (int i = 0; i < n; i++) {
        File f = LittleFS.open("/" + names[i], "r");
        fsize[i] = f ? (uint32_t)f.size() : 0;
        if (f) f.close();
        const uint32_t nl = names[i].length();
        total  += 30 + nl + fsize[i];      // local header + data
        cdsize += 46 + nl;                 // central directory record
    }
    s_http.setContentLength(total + cdsize + 22);
    s_http.sendHeader("Content-Disposition", "attachment; filename=beacon_bundle.zip");
    s_http.send(200, "application/zip", "");
    WiFiClient& client = s_http.client();

    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        uint32_t crc = 0xFFFFFFFFu;
        File f = LittleFS.open("/" + names[i], "r");
        if (f) { int r; while ((r = f.read(buf, sizeof(buf))) > 0) crc = zip_crc32(crc, buf, r); f.close(); }
        crc ^= 0xFFFFFFFFu;
        fcrc[i] = crc; foff[i] = off;

        const uint32_t nl = names[i].length();
        uint8_t h[30];
        put_le(h + 0, 0x04034b50, 4); put_le(h + 4, 20, 2); put_le(h + 6, 0, 2);
        put_le(h + 8, 0, 2);              // method 0 = stored
        put_le(h + 10, 0, 2); put_le(h + 12, 0x21, 2);   // time / date (1980-01-01)
        put_le(h + 14, crc, 4); put_le(h + 18, fsize[i], 4); put_le(h + 22, fsize[i], 4);
        put_le(h + 26, nl, 2); put_le(h + 28, 0, 2);
        client.write(h, 30);
        client.write((const uint8_t*)names[i].c_str(), nl);
        off += 30 + nl;

        f = LittleFS.open("/" + names[i], "r");
        if (f) { int r; while ((r = f.read(buf, sizeof(buf))) > 0) client.write(buf, r); f.close(); }
        off += fsize[i];
    }

    const uint32_t cdoff = off;
    for (int i = 0; i < n; i++) {
        const uint32_t nl = names[i].length();
        uint8_t h[46];
        put_le(h + 0, 0x02014b50, 4); put_le(h + 4, 20, 2); put_le(h + 6, 20, 2);
        put_le(h + 8, 0, 2); put_le(h + 10, 0, 2); put_le(h + 12, 0, 2); put_le(h + 14, 0x21, 2);
        put_le(h + 16, fcrc[i], 4); put_le(h + 20, fsize[i], 4); put_le(h + 24, fsize[i], 4);
        put_le(h + 28, nl, 2); put_le(h + 30, 0, 2); put_le(h + 32, 0, 2); put_le(h + 34, 0, 2);
        put_le(h + 36, 0, 2); put_le(h + 38, 0, 4); put_le(h + 42, foff[i], 4);
        client.write(h, 46);
        client.write((const uint8_t*)names[i].c_str(), nl);
    }

    uint8_t e[22];
    put_le(e + 0, 0x06054b50, 4); put_le(e + 4, 0, 2); put_le(e + 6, 0, 2);
    put_le(e + 8, n, 2); put_le(e + 10, n, 2);
    put_le(e + 12, cdsize, 4); put_le(e + 16, cdoff, 4); put_le(e + 20, 0, 2);
    client.write(e, 22);
}

// ─── HTTP: download — one file → inline preview (long-press to save on iOS),
//     several → a single stored .zip (best opened in Safari / on desktop) ────────
static void handle_dl() {
    String names[SEL_MAX];
    int n = collect_sel(names);
    if (n == 0) { handle_root(); return; }          // nothing selected → back to gallery

    if (n == 1) {
        // Inline preview, not a forced download: a captive-portal browser would
        // render an attachment as scrambled bytes. Long-press the image to save.
        render_view(names[0]);
        return;
    }
    stream_zip(names, n);
}

// ─── OS connectivity probes → report "online" so no captive popup appears ───────
// iOS/macOS fetch http://captive.apple.com/hotspot-detect.html and expect a page
// containing "Success". Give them exactly that and iOS marks the network online,
// so it does NOT trap the user in the Captive Network Assistant mini-browser
// (which blocks long-press / Save to Photos / downloads). The user instead opens
// real Safari at 192.168.4.1, where saving images works natively.
static void handle_captive() {
    s_http.send(200, "text/html",
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

// Android probes /generate_204 and expects an empty HTTP 204.
static void handle_204() {
    s_http.send(204, "text/plain", "");
}

// ─── draw two QRs side-by-side: [1] join wifi  [2] open Safari ──────────────────
// The ESP-IDF encoder is callback-based: esp_qrcode_generate() encodes the text
// then invokes display_func with a handle to read modules from. Its signature
// carries no user pointer, so the target display, draw origin and scale pass
// through file-scope statics. The callback draws ONE code at (s_qr_ox,s_qr_oy)
// without clearing the screen, so two codes can share the display.
static Arduino_GFX* s_qr_gfx = nullptr;
static int          s_qr_qpx = 0;       // rendered QR side in px (for layout)
static int          s_qr_ox = 0, s_qr_oy = 0, s_qr_scale = 4;
static bool         s_qr_measure = false;   // true = size only, don't draw

static void qr_render_cb(esp_qrcode_handle_t qr) {
    const int modules = esp_qrcode_get_size(qr);
    s_qr_qpx = modules * s_qr_scale;
    if (s_qr_measure) return;                    // measuring pass — just report size
    const int qz = s_qr_scale * 2;               // quiet zone (kept white, scannable)
    s_qr_gfx->fillRect(s_qr_ox - qz, s_qr_oy - qz,
                       s_qr_qpx + qz * 2, s_qr_qpx + qz * 2, S_WHITE);
    for (int y = 0; y < modules; y++)
        for (int x = 0; x < modules; x++)
            if (esp_qrcode_get_module(qr, x, y))
                s_qr_gfx->fillRect(s_qr_ox + x * s_qr_scale, s_qr_oy + y * s_qr_scale,
                                   s_qr_scale, s_qr_scale, S_BLACK);
}

static int qr_encode(Arduino_GFX* gfx, const char* text, int ox, int oy, int scale, bool measure) {
    s_qr_gfx = gfx; s_qr_ox = ox; s_qr_oy = oy; s_qr_scale = scale;
    s_qr_qpx = 0; s_qr_measure = measure;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func       = qr_render_cb;
    cfg.max_qrcode_version = 4;
    cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
    const int r = (esp_qrcode_generate(&cfg, text) == ESP_OK) ? s_qr_qpx : 0;
    s_qr_measure = false;
    return r;
}

// Print text horizontally centered on cx.
static void print_center(Arduino_GFX* gfx, int cx, int y, const char* s) {
    gfx->setCursor(cx - (int)strlen(s) * 3, y);   // 6px/char → half = 3
    gfx->print(s);
}

// Two QRs, centered as a group on an OSINT green-on-black screen:
//   [1] join the open AP   [2] open the gallery in Safari (scan with Camera)
static void draw_share_screen(Arduino_GFX* gfx, const char* ssid) {
    char wifitext[48];
    snprintf(wifitext, sizeof(wifitext), "WIFI:T:nopass;S:%s;;", ssid);
    static const char* urltext = "http://192.168.4.1";
    const int scale = 4;

    gfx->fillScreen(S_BLACK);
    gfx->setTextColor(S_GREEN, S_BLACK);
    gfx->setTextSize(1);
    print_center(gfx, IMG_W / 2, 8, "// BEACON SHARE");

    // measure first so the pair can be centered as a block
    const int wpx = qr_encode(gfx, wifitext, 0, 0, scale, true);
    const int upx = qr_encode(gfx, urltext,  0, 0, scale, true);
    if (wpx == 0 || upx == 0) {
        print_center(gfx, IMG_W / 2, 70, "QR failed");
        print_center(gfx, IMG_W / 2, 86, "join wifi, open 192.168.4.1");
        return;
    }

    const int gap = 24;
    const int oy  = 46;
    const int gx  = (IMG_W - (wpx + gap + upx)) / 2;     // centered group origin
    const int wox = gx;
    const int uox = gx + wpx + gap;
    const int qz  = scale * 2;

    qr_encode(gfx, wifitext, wox, oy, scale, false);
    qr_encode(gfx, urltext,  uox, oy, scale, false);

    // green frames tie the white codes into the osint look
    gfx->drawRect(wox - qz, oy - qz, wpx + qz * 2, wpx + qz * 2, S_GREEN);
    gfx->drawRect(uox - qz, oy - qz, upx + qz * 2, upx + qz * 2, S_GREEN);

    const int ly = oy + (wpx > upx ? wpx : upx) + qz + 12;
    gfx->setTextColor(S_GREEN, S_BLACK);
    print_center(gfx, wox + wpx / 2, ly, "1 JOIN");
    print_center(gfx, uox + upx / 2, ly, "2 OPEN");
}

// ─── public entry point ─────────────────────────────────────────────────────────
void run_share_mode(Arduino_GFX* gfx, beacon_press_fn poll) {
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
    s_http.on("/del", handle_del);
    s_http.on("/img", handle_img);
    s_http.on("/view", handle_view);
    s_http.on("/generate_204", handle_204); // Android probe
    s_http.on("/gen_204", handle_204);
    s_http.onNotFound(handle_captive);      // iOS/Win probes → "online", no popup
    s_http.begin();

    // Two QRs: [1] join the open AP, [2] open the gallery in Safari.
    draw_share_screen(gfx, ssid);
    Serial.printf("AP '%s' up — http://%s/\n", ssid, s_ap_ip.toString().c_str());

    // The triggering long-press was already released by poll_press() before we
    // got here (it waits for release before reporting LONG), so we can watch for
    // the exit gesture right away.

    // Serve until a LONG press (touch or GPIO0). A short tap is ignored, so the
    // QR can't be dismissed by a stray touch while someone's scanning it.
    while (true) {
        s_dns.processNextRequest();
        s_http.handleClient();
        if (poll && poll() == 2) break;   // 2 = long press → exit
        delay(2);
    }

    s_http.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("share mode end");
}
