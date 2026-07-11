// ============================================================
//  WatchFlow v2.2 – ESP32 Drucker-Monitor für FilamentFlow
//  Bambu Lab (MQTT/TLS) + Moonraker/Klipper (HTTP)
//  MW Service 3D | filament-flow.com
//  Änderungen v2.2.1: stg_cur (Pro) + vt_tray (Free)
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <vector>

// ── CONSTANTS ──────────────────────────────────────────────
#define MAX_PRINTERS         3
#define BAMBU_PORT           8883
#define PRINTER_INTERVAL     20000
#define MQTT_WAIT_MS         15000
#define AP_SSID              "WatchFlow-Setup"
#define AP_PASS              "watchflow"
#define SETUP_PIN            4
#define AUTO_RESTART_MS      21600000UL   // Auto-Neustart nach 6 Stunden
#define MOONRAKER_PORT_DEF   7125

// ── STANDARD WERTE ─────────────────────────────────────────
#define DEFAULT_API_URL    "https://filament-flow.com/api/printer-status"
#define DEFAULT_API_KEY    ""
#define DEFAULT_USER_EMAIL ""
// ───────────────────────────────────────────────────────────

Preferences prefs;
WebServer server(80);

String cfg_wifi_ssid  = "";
String cfg_wifi_pass  = "";
String cfg_api_url    = "";
String cfg_api_key    = "";
String cfg_user_email = "";

// ── DRUCKER-TYP ─────────────────────────────────────────────
enum PrinterType { TYPE_BAMBU, TYPE_MOONRAKER };

struct PrinterConfig {
  String      name;
  String      ip;
  String      serial;      // Bambu: Seriennummer | Moonraker: leer
  String      lan_code;    // Bambu: LAN-Code     | Moonraker: leer
  int         mr_port = MOONRAKER_PORT_DEF;  // Moonraker Port
  PrinterType type    = TYPE_BAMBU;
  bool        active  = false;
};

PrinterConfig printerConfigs[MAX_PRINTERS];
int numPrinters = 0;

WiFiClientSecure wifiClients[MAX_PRINTERS];
PubSubClient* mqttClients[MAX_PRINTERS];

// ── DATENSTRUKTUREN ────────────────────────────────────────

struct AmsTray {
  int    ams_id;
  int    tray_id;
  String tray_type;
  String tray_color;
  int    remain;
  String tray_sub_brands;
  String tray_info_idx;
  float  tray_diameter;
  int    nozzle_temp_min;
  int    nozzle_temp_max;
  int    bed_temp;
  int    drying_temp;
  int    drying_time;
  float  humidity;
  float  temp;
};

struct HmsError {
  String attr;
  String code;
  int    severity;
};

struct PrinterData {
  String gcode_state       = "IDLE";
  int    mc_percent        = 0;
  int    mc_remaining_time = 0;
  String subtask_name      = "";
  String gcode_file        = "";
  String gcode_start_time  = "";
  String fail_reason       = "0";
  int    spd_lvl           = 2;
  int    layer_num         = 0;
  int    total_layer_num   = 0;
  float  nozzle_temper_1   = 0;
  float  nozzle_temper_2   = 0;
  float  bed_temper        = 0;
  float  chamber_temper    = 0;
  float  nozzle_target_1   = 0;
  float  nozzle_target_2   = 0;
  float  bed_target_temper = 0;
  int    fan_cooling       = 0;
  int    fan_aux           = 0;
  int    fan_chamber       = 0;
  int    fan_heatbreak     = 0;
  // ── NEU: stg_cur (Pro) + vt_tray (Free) ──
  int    stg_cur            = -1;
  String vt_tray_type        = "";
  String vt_tray_color       = "";
  int    vt_remain           = -1;
  String vt_tray_sub_brands  = "";
  String vt_tray_info_idx    = "";
  float  vt_tray_diameter    = 0;
  int    vt_nozzle_temp_min  = 0;
  int    vt_nozzle_temp_max  = 0;
  int    vt_bed_temp         = 0;
  int    vt_drying_temp      = 0;
  int    vt_drying_time      = 0;
  // ─────────────────────────────────────────
  std::vector<AmsTray>  trays;
  std::vector<HmsError> hms_errors;
};

PrinterData printerData[MAX_PRINTERS];
int activeCallbackIndex = 0;
int currentPrinter      = 0;
unsigned long lastPrinterSwitch = 0;
unsigned long currentInterval = PRINTER_INTERVAL; // dynamisch je nach Lizenz
bool configMode = false;
bool dataReceived[MAX_PRINTERS] = {false, false, false};

// ── HILFSFUNKTIONEN ────────────────────────────────────────

int fanToPercent(const char* val) {
  int raw = String(val).toInt();
  if (raw <= 0)  return 0;
  if (raw >= 15) return 100;
  return map(raw, 0, 15, 0, 100);
}

void resetPrinterData(int idx) {
  PrinterData& pd = printerData[idx];
  pd.gcode_state       = "IDLE";
  pd.mc_percent        = 0;
  pd.mc_remaining_time = 0;
  pd.nozzle_temper_1   = 0;  pd.nozzle_target_1   = 0;
  pd.nozzle_temper_2   = 0;  pd.nozzle_target_2   = 0;
  pd.bed_temper        = 0;  pd.bed_target_temper = 0;
  pd.chamber_temper    = 0;
  pd.subtask_name      = "";
  pd.gcode_file        = "";
  pd.gcode_start_time  = "";
  pd.fail_reason       = "0";
  pd.spd_lvl           = 2;
  pd.layer_num         = 0;
  pd.total_layer_num   = 0;
  pd.fan_cooling       = 0;
  pd.fan_aux           = 0;
  pd.fan_chamber       = 0;
  pd.fan_heatbreak     = 0;
  // ── NEU ──
  pd.stg_cur = -1;
  pd.vt_tray_type = ""; pd.vt_tray_color = ""; pd.vt_remain = -1;
  pd.vt_tray_sub_brands = ""; pd.vt_tray_info_idx = ""; pd.vt_tray_diameter = 0;
  pd.vt_nozzle_temp_min = 0; pd.vt_nozzle_temp_max = 0; pd.vt_bed_temp = 0;
  pd.vt_drying_temp = 0; pd.vt_drying_time = 0;
  // ─────────
  pd.trays.clear();
  pd.hms_errors.clear();
}

// ── HTML ───────────────────────────────────────────────────

const char* HTML_HEADER = R"(<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>WatchFlow Setup</title>
<style>
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px}
.card{background:#16213e;border-radius:12px;padding:24px;max-width:520px;margin:0 auto 20px}
h1{color:#00d4ff;text-align:center;margin-bottom:4px}
h2{color:#00d4ff;margin-top:0}
p.sub{text-align:center;color:#888;margin-top:0}
label{display:block;margin:12px 0 4px;color:#aaa;font-size:14px}
input,select{width:100%;padding:10px;border-radius:8px;border:1px solid #333;
background:#0f3460;color:#fff;box-sizing:border-box;font-size:14px}
input:focus,select:focus{outline:none;border-color:#00d4ff}
select option{background:#0f3460}
button{width:100%;padding:12px;background:#00d4ff;color:#000;border:none;
border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;margin-top:16px}
button:hover{background:#00b8d9}
.btn-reset{background:#ff4444;color:#fff;margin-top:8px}
.btn-reset:hover{background:#cc0000}
.badge{display:inline-block;background:#0f3460;padding:4px 10px;border-radius:20px;
font-size:12px;color:#00d4ff;margin-bottom:16px}
.printer-block{border:1px solid #333;border-radius:8px;padding:16px;margin-bottom:12px}
.printer-title{color:#00d4ff;font-weight:bold;margin-bottom:8px}
.success{color:#00ff88;text-align:center;padding:12px;background:#003322;
border-radius:8px;margin-top:12px}
.warning{color:#ffaa00;text-align:center;padding:12px;background:#332200;
border-radius:8px;margin-top:12px}
.info{color:#888;font-size:12px;margin-top:4px;margin-bottom:8px}
.divider{border:none;border-top:1px solid #333;margin:20px 0}
.hint{background:#0f3460;border-radius:8px;padding:10px;font-size:12px;color:#aaa;margin-bottom:12px}
.hint-mr{background:#0f2040;border-radius:8px;padding:10px;font-size:12px;color:#88aaff;margin-bottom:12px}
.type-bambu{border-color:#00d4ff55}
.type-moonraker{border-color:#8855ff55}
.badge-mr{color:#8855ff}
</style>
<script>
function updateType(i){
var t=document.getElementById('p'+i+'_type').value;
var bambu=document.getElementById('p'+i+'_bambu');
var mr=document.getElementById('p'+i+'_moonraker');
if(t==='bambu'){bambu.style.display='block';mr.style.display='none';}
else{bambu.style.display='none';mr.style.display='block';}
}
window.onload=function(){for(var i=0;i<3;i++)updateType(i);};
</script>
</head><body>)";

const char* HTML_FOOTER = R"(</body></html>)";

String getSetupPage() {
  String html = HTML_HEADER;
  html += "<div class='card'>";
  html += "<h1>&#9200; WatchFlow</h1>";
  html += "<p class='sub'>Bambu Lab &amp; Moonraker Monitor</p>";
  html += "<span class='badge'>ESP32 v2.2</span>";
  html += "<form action='/save' method='POST'>";

  // WLAN
  html += "<hr class='divider'>";
  html += "<h2>&#128246; WLAN</h2>";
  html += "<label>WLAN Name (SSID)</label>";
  html += "<input name='ssid' placeholder='Dein WLAN Name' value='" + cfg_wifi_ssid + "' required>";
  html += "<label>WLAN Passwort</label>";
  html += "<input name='wpass' type='password' placeholder='Leer lassen = unveraendert'>";

  // FilamentFlow
  html += "<hr class='divider'>";
  html += "<h2>&#9729; FilamentFlow</h2>";
  html += "<div class='hint'>&#128274; API Key findest du in der FilamentFlow App unter <strong>Einstellungen &rarr; WatchFlow</strong></div>";
  html += "<label>API URL</label>";
  html += "<input name='apiurl' value='" + cfg_api_url + "' required>";
  html += "<label>API Key</label>";
  html += "<input name='apikey' placeholder='Aus FilamentFlow App kopieren' value='" + cfg_api_key + "' required>";
  html += "<label>E-Mail Adresse</label>";
  html += "<input name='email' type='email' placeholder='Deine FilamentFlow E-Mail' value='" + cfg_user_email + "' required>";

  // Drucker
  html += "<hr class='divider'>";
  html += "<h2>&#128424; Drucker</h2>";
  html += "<p class='info'>Max. " + String(MAX_PRINTERS) + " Drucker &ndash; leer lassen = deaktiviert</p>";

  for (int i = 0; i < MAX_PRINTERS; i++) {
    String typeVal = (printerConfigs[i].type == TYPE_MOONRAKER) ? "moonraker" : "bambu";
    html += "<div class='printer-block'>";
    html += "<div class='printer-title'>Drucker " + String(i + 1) + "</div>";

    html += "<label>Name</label>";
    html += "<input name='p" + String(i) + "_name' placeholder='z.B. H2C oder Klipper-1' value='" + printerConfigs[i].name + "'>";
    html += "<label>IP Adresse</label>";
    html += "<input name='p" + String(i) + "_ip' placeholder='192.168.178.XXX' value='" + printerConfigs[i].ip + "'>";

    html += "<label>Typ</label>";
    html += "<select name='p" + String(i) + "_type' id='p" + String(i) + "_type' onchange='updateType(" + String(i) + ")'>";
    html += "<option value='bambu'" + String(typeVal == "bambu" ? " selected" : "") + ">&#127981; Bambu Lab (LAN)</option>";
    html += "<option value='moonraker'" + String(typeVal == "moonraker" ? " selected" : "") + ">&#128313; Moonraker / Klipper</option>";
    html += "</select>";

    // Bambu-Felder
    html += "<div id='p" + String(i) + "_bambu'>";
    html += "<div class='hint' style='margin-top:10px'>&#9888; Developer Mode: Einstellungen &rarr; LAN Only Mode &rarr; Developer Mode</div>";
    html += "<label>Seriennummer</label>";
    html += "<input name='p" + String(i) + "_serial' placeholder='31B8BP...' value='" + printerConfigs[i].serial + "'>";
    html += "<label>LAN Code (leer = unveraendert)</label>";
    html += "<input name='p" + String(i) + "_code' type='password' placeholder='LAN Code vom Display'>";
    html += "</div>";

    // Moonraker-Felder
    html += "<div id='p" + String(i) + "_moonraker' style='display:none'>";
    html += "<div class='hint-mr' style='margin-top:10px'>&#128313; Moonraker API auf Port 7125 (Standard). Kein Auth noetig im lokalen Netz.</div>";
    html += "<label>Moonraker Port (Standard: 7125)</label>";
    html += "<input name='p" + String(i) + "_mrport' placeholder='7125' value='" + String(printerConfigs[i].mr_port) + "'>";
    html += "</div>";

    html += "</div>";
  }

  html += "<button type='submit'>&#128190; Speichern &amp; Starten</button>";
  html += "</form>";
  html += "<hr class='divider'>";
  html += "<form action='/reset' method='POST' onsubmit=\"return confirm('Alle Einstellungen loeschen?')\">";
  html += "<button type='submit' class='btn-reset'>&#9888; Werkseinstellungen</button>";
  html += "</form>";
  html += "</div>";
  html += HTML_FOOTER;
  return html;
}

String getSavedPage() {
  String html = HTML_HEADER;
  html += "<div class='card'><h1>&#9200; WatchFlow</h1>";
  html += "<div class='success'>&#10003; Konfiguration gespeichert!<br><br>";
  html += "ESP32 startet in 3 Sekunden neu...<br><br>";
  html += "Verbinde dich wieder mit deinem WLAN.</div></div>";
  html += HTML_FOOTER;
  return html;
}

String getResetPage() {
  String html = HTML_HEADER;
  html += "<div class='card'><h1>&#9200; WatchFlow</h1>";
  html += "<div class='warning'>&#9888; Werkseinstellungen wiederhergestellt!<br><br>";
  html += "ESP32 startet in 3 Sekunden neu...<br><br>";
  html += "Verbinde mit <strong>WatchFlow-Setup</strong><br>";
  html += "und oeffne http://192.168.4.1</div></div>";
  html += HTML_FOOTER;
  return html;
}

// ── KONFIGURATION ──────────────────────────────────────────

void saveConfig() {
  prefs.begin("watchflow", false);
  prefs.putString("ssid",    cfg_wifi_ssid);
  prefs.putString("wpass",   cfg_wifi_pass);
  prefs.putString("apiurl",  cfg_api_url);
  prefs.putString("apikey",  cfg_api_key);
  prefs.putString("email",   cfg_user_email);
  prefs.putInt("numPrinters", numPrinters);
  for (int i = 0; i < MAX_PRINTERS; i++) {
    String pfx = "p" + String(i) + "_";
    prefs.putString((pfx + "name").c_str(),   printerConfigs[i].name);
    prefs.putString((pfx + "ip").c_str(),     printerConfigs[i].ip);
    prefs.putString((pfx + "serial").c_str(), printerConfigs[i].serial);
    prefs.putInt(   (pfx + "type").c_str(),   (int)printerConfigs[i].type);
    prefs.putInt(   (pfx + "mrport").c_str(), printerConfigs[i].mr_port);
    if (printerConfigs[i].lan_code.length() > 0)
      prefs.putString((pfx + "code").c_str(), printerConfigs[i].lan_code);
  }
  prefs.end();
}

bool loadConfig() {
  prefs.begin("watchflow", true);
  cfg_wifi_ssid  = prefs.getString("ssid",   "");
  cfg_wifi_pass  = prefs.getString("wpass",  "");
  cfg_api_url    = prefs.getString("apiurl",  DEFAULT_API_URL);
  cfg_api_key    = prefs.getString("apikey",  DEFAULT_API_KEY);
  cfg_user_email = prefs.getString("email",   DEFAULT_USER_EMAIL);
  numPrinters    = prefs.getInt("numPrinters", 0);
  for (int i = 0; i < MAX_PRINTERS; i++) {
    String pfx = "p" + String(i) + "_";
    printerConfigs[i].name     = prefs.getString((pfx + "name").c_str(),   "");
    printerConfigs[i].ip       = prefs.getString((pfx + "ip").c_str(),     "");
    printerConfigs[i].serial   = prefs.getString((pfx + "serial").c_str(), "");
    printerConfigs[i].lan_code = prefs.getString((pfx + "code").c_str(),   "");
    printerConfigs[i].type     = (PrinterType)prefs.getInt((pfx + "type").c_str(), (int)TYPE_BAMBU);
    printerConfigs[i].mr_port  = prefs.getInt((pfx + "mrport").c_str(), MOONRAKER_PORT_DEF);
    if (printerConfigs[i].type == TYPE_BAMBU) {
      printerConfigs[i].active = printerConfigs[i].name.length()     > 0 &&
                                  printerConfigs[i].ip.length()       > 0 &&
                                  printerConfigs[i].serial.length()   > 0 &&
                                  printerConfigs[i].lan_code.length() > 0;
    } else {
      printerConfigs[i].active = printerConfigs[i].name.length() > 0 &&
                                  printerConfigs[i].ip.length()   > 0;
    }
  }
  prefs.end();
  return cfg_wifi_ssid.length() > 0;
}

void resetConfig() {
  prefs.begin("watchflow", false);
  prefs.clear();
  prefs.end();
  Serial.println("Werkseinstellungen wiederhergestellt");
}

// ── WEBSERVER ──────────────────────────────────────────────

void handleRoot()  { server.send(200, "text/html", getSetupPage()); }

void handleReset() {
  resetConfig();
  server.send(200, "text/html", getResetPage());
  delay(3000);
  ESP.restart();
}

void handleSave() {
  cfg_wifi_ssid  = server.arg("ssid");
  String newPass = server.arg("wpass");
  if (newPass.length() > 0) cfg_wifi_pass = newPass;
  cfg_api_url    = server.arg("apiurl");
  cfg_api_key    = server.arg("apikey");
  cfg_user_email = server.arg("email");

  numPrinters = 0;
  for (int i = 0; i < MAX_PRINTERS; i++) {
    printerConfigs[i].name   = server.arg("p" + String(i) + "_name");
    printerConfigs[i].ip     = server.arg("p" + String(i) + "_ip");
    printerConfigs[i].serial = server.arg("p" + String(i) + "_serial");

    String typeStr = server.arg("p" + String(i) + "_type");
    printerConfigs[i].type = (typeStr == "moonraker") ? TYPE_MOONRAKER : TYPE_BAMBU;

    String portStr = server.arg("p" + String(i) + "_mrport");
    printerConfigs[i].mr_port = (portStr.length() > 0) ? portStr.toInt() : MOONRAKER_PORT_DEF;

    String code = server.arg("p" + String(i) + "_code");
    if (code.length() > 0) printerConfigs[i].lan_code = code;

    if (printerConfigs[i].type == TYPE_BAMBU) {
      printerConfigs[i].active = printerConfigs[i].name.length()     > 0 &&
                                  printerConfigs[i].ip.length()       > 0 &&
                                  printerConfigs[i].serial.length()   > 0 &&
                                  printerConfigs[i].lan_code.length() > 0;
    } else {
      printerConfigs[i].active = printerConfigs[i].name.length() > 0 &&
                                  printerConfigs[i].ip.length()   > 0;
    }
    if (printerConfigs[i].active) numPrinters = i + 1;
  }

  saveConfig();
  server.send(200, "text/html", getSavedPage());
  delay(3000);
  ESP.restart();
}

void startConfigMode() {
  configMode = true;
  Serial.println("Setup-Modus: " + String(AP_SSID));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  server.on("/",      handleRoot);
  server.on("/save",  HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  Serial.println("Webserver -> http://192.168.4.1");
}

// ── MQTT CALLBACK (Bambu) ──────────────────────────────────

void mqttCallbackGeneric(char* topic, byte* payload, unsigned int length) {
  Serial.println("RAW von Drucker " + String(activeCallbackIndex + 1) + " Laenge: " + String(length));

  String preview = "";
  for (unsigned int i = 0; i < min(length, (unsigned int)100); i++)
    preview += (char)payload[i];

  if (preview.indexOf("print") == -1) {
    Serial.println("Nicht-print Nachricht -> pushall erneut senden...");
    printerData[activeCallbackIndex].gcode_state = "IDLE";
    printerData[activeCallbackIndex].mc_percent  = 0;
    String cmdTopic = "device/" + printerConfigs[activeCallbackIndex].serial + "/request";
    mqttClients[activeCallbackIndex]->publish(cmdTopic.c_str(),
      "{\"pushing\":{\"sequence_id\":\"3\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
    return;
  }

  // ── Filter: 1536 Bytes (erweitert für stg_cur + vt_tray) ──
  StaticJsonDocument<1536> filter;
  filter["print"]["gcode_state"]       = true;
  filter["print"]["mc_percent"]        = true;
  filter["print"]["mc_remaining_time"] = true;
  filter["print"]["bed_temper"]        = true;
  filter["print"]["bed_target_temper"] = true;
  filter["print"]["subtask_name"]      = true;
  filter["print"]["gcode_file"]        = true;
  filter["print"]["gcode_start_time"]  = true;
  filter["print"]["fail_reason"]       = true;
  filter["print"]["spd_lvl"]           = true;
  filter["print"]["big_fan1_speed"]    = true;
  filter["print"]["big_fan2_speed"]    = true;
  filter["print"]["cooling_fan_speed"] = true;
  filter["print"]["heatbreak_fan_speed"] = true;
  filter["print"]["3D"]["layer_num"]       = true;
  filter["print"]["3D"]["total_layer_num"] = true;
  filter["print"]["device"]["ctc"]["info"]["temp"]           = true;
  filter["print"]["device"]["extruder"]["info"][0]["temp"]   = true;
  filter["print"]["device"]["extruder"]["info"][0]["target"] = true;
  filter["print"]["device"]["extruder"]["info"][1]["temp"]   = true;
  filter["print"]["device"]["extruder"]["info"][1]["target"] = true;
  filter["print"]["ams"]["ams"][0]["id"]           = true;
  filter["print"]["ams"]["ams"][0]["humidity"]     = true;
  filter["print"]["ams"]["ams"][0]["humidity_raw"] = true;
  filter["print"]["ams"]["ams"][0]["temp"]         = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["id"]             = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["state"]          = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_type"]      = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_color"]     = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["remain"]         = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_sub_brands"] = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_info_idx"]  = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_diameter"]  = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["nozzle_temp_min"] = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["nozzle_temp_max"] = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["bed_temp"]       = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["drying_temp"]    = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["drying_time"]    = true;
  // ── NEU: stg_cur + vt_tray ──
  filter["print"]["stg_cur"] = true;
  filter["print"]["vt_tray"]["tray_type"]       = true;
  filter["print"]["vt_tray"]["tray_color"]      = true;
  filter["print"]["vt_tray"]["remain"]          = true;
  filter["print"]["vt_tray"]["tray_sub_brands"] = true;
  filter["print"]["vt_tray"]["tray_info_idx"]   = true;
  filter["print"]["vt_tray"]["tray_diameter"]   = true;
  filter["print"]["vt_tray"]["nozzle_temp_min"] = true;
  filter["print"]["vt_tray"]["nozzle_temp_max"] = true;
  filter["print"]["vt_tray"]["bed_temp"]        = true;
  filter["print"]["vt_tray"]["drying_temp"]     = true;
  filter["print"]["vt_tray"]["drying_time"]     = true;
  // ────────────────────────────
  filter["print"]["hms"][0]["attr"]     = true;
  filter["print"]["hms"][0]["code"]     = true;
  filter["print"]["hms"][0]["severity"] = true;

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload, length,
                              DeserializationOption::Filter(filter));
  if (err) {
    Serial.println("JSON Fehler: " + String(err.c_str()));
    return;
  }
  if (!doc.containsKey("print")) return;

  JsonObject print = doc["print"];
  PrinterData& pd = printerData[activeCallbackIndex];

  if (print.containsKey("gcode_state"))       pd.gcode_state       = print["gcode_state"].as<String>();
  if (print.containsKey("mc_percent"))        pd.mc_percent        = print["mc_percent"];
  if (print.containsKey("mc_remaining_time")) pd.mc_remaining_time = print["mc_remaining_time"];
  if (print.containsKey("subtask_name"))      pd.subtask_name      = print["subtask_name"].as<String>();
  if (print.containsKey("gcode_file"))        pd.gcode_file        = print["gcode_file"].as<String>();
  if (print.containsKey("gcode_start_time"))  pd.gcode_start_time  = print["gcode_start_time"].as<String>();
  if (print.containsKey("fail_reason"))       pd.fail_reason       = print["fail_reason"].as<String>();
  if (print.containsKey("spd_lvl"))           pd.spd_lvl           = print["spd_lvl"];
  if (print.containsKey("bed_temper"))        pd.bed_temper        = print["bed_temper"];
  if (print.containsKey("bed_target_temper")) pd.bed_target_temper = print["bed_target_temper"];
  // ── NEU: stg_cur ──
  if (print.containsKey("stg_cur")) {
    pd.stg_cur = print["stg_cur"];
    Serial.println("stg_cur=" + String(pd.stg_cur));
  }
  // ──────────────────

  if (doc["print"]["3D"].containsKey("layer_num"))
    pd.layer_num = doc["print"]["3D"]["layer_num"];
  if (doc["print"]["3D"].containsKey("total_layer_num"))
    pd.total_layer_num = doc["print"]["3D"]["total_layer_num"];

  if (doc["print"]["device"]["ctc"]["info"].containsKey("temp"))
    pd.chamber_temper = doc["print"]["device"]["ctc"]["info"]["temp"];
  if (doc["print"]["device"]["extruder"]["info"][0].containsKey("temp"))
    pd.nozzle_temper_1 = doc["print"]["device"]["extruder"]["info"][0]["temp"];
  if (doc["print"]["device"]["extruder"]["info"][0].containsKey("target"))
    pd.nozzle_target_1 = doc["print"]["device"]["extruder"]["info"][0]["target"];
  if (doc["print"]["device"]["extruder"]["info"][1].containsKey("temp"))
    pd.nozzle_temper_2 = doc["print"]["device"]["extruder"]["info"][1]["temp"];
  if (doc["print"]["device"]["extruder"]["info"][1].containsKey("target"))
    pd.nozzle_target_2 = doc["print"]["device"]["extruder"]["info"][1]["target"];

  if (print.containsKey("cooling_fan_speed"))   pd.fan_cooling  = fanToPercent(print["cooling_fan_speed"].as<const char*>());
  if (print.containsKey("big_fan1_speed"))      pd.fan_aux      = fanToPercent(print["big_fan1_speed"].as<const char*>());
  if (print.containsKey("big_fan2_speed"))      pd.fan_chamber  = fanToPercent(print["big_fan2_speed"].as<const char*>());
  if (print.containsKey("heatbreak_fan_speed")) pd.fan_heatbreak= fanToPercent(print["heatbreak_fan_speed"].as<const char*>());

  pd.trays.clear();
  if (doc["print"].containsKey("ams")) {
    JsonArray amsList = doc["print"]["ams"]["ams"].as<JsonArray>();
    for (JsonObject ams : amsList) {
      int   amsId       = ams["id"].as<String>().toInt();
      float humidityVal = ams["humidity"].as<String>().toFloat();
      float amsHumidity = (humidityVal == 0 && ams.containsKey("humidity_raw"))
                          ? ams["humidity_raw"].as<String>().toFloat()
                          : humidityVal;
      float amsTemp = ams["temp"].as<String>().toFloat();
      JsonArray trays = ams["tray"].as<JsonArray>();
      for (JsonObject tray : trays) {
        if (tray["state"] != 11) continue;
        AmsTray t;
        t.ams_id          = amsId;
        t.tray_id         = tray["id"].as<String>().toInt();
        t.tray_type       = tray["tray_type"].as<String>();
        t.tray_color      = tray["tray_color"].as<String>();
        t.remain          = tray["remain"];
        t.tray_sub_brands = tray["tray_sub_brands"].as<String>();
        t.tray_info_idx   = tray["tray_info_idx"].as<String>();
        t.tray_diameter   = tray["tray_diameter"].as<String>().toFloat();
        t.nozzle_temp_min = tray["nozzle_temp_min"].as<String>().toInt();
        t.nozzle_temp_max = tray["nozzle_temp_max"].as<String>().toInt();
        t.bed_temp        = tray["bed_temp"].as<String>().toInt();
        t.drying_temp     = tray["drying_temp"].as<String>().toInt();
        t.drying_time     = tray["drying_time"].as<String>().toInt();
        t.humidity        = amsHumidity;
        t.temp            = amsTemp;
        pd.trays.push_back(t);
      }
    }
  }

  // ── NEU: vt_tray (externe Spule ohne AMS) ──
  if (doc["print"].containsKey("vt_tray")) {
    JsonObject vt = doc["print"]["vt_tray"];
    pd.vt_tray_type       = vt["tray_type"].as<String>();
    pd.vt_tray_color      = vt["tray_color"].as<String>();
    pd.vt_remain          = vt["remain"] | -1;
    pd.vt_tray_sub_brands = vt["tray_sub_brands"].as<String>();
    pd.vt_tray_info_idx   = vt["tray_info_idx"].as<String>();
    pd.vt_tray_diameter   = vt["tray_diameter"].as<String>().toFloat();
    pd.vt_nozzle_temp_min = vt["nozzle_temp_min"].as<String>().toInt();
    pd.vt_nozzle_temp_max = vt["nozzle_temp_max"].as<String>().toInt();
    pd.vt_bed_temp        = vt["bed_temp"].as<String>().toInt();
    pd.vt_drying_temp     = vt["drying_temp"].as<String>().toInt();
    pd.vt_drying_time     = vt["drying_time"].as<String>().toInt();
    Serial.println("vt_tray: type=" + pd.vt_tray_type + " remain=" + String(pd.vt_remain));
  } else {
    pd.vt_remain = -1;
  }
  // ────────────────────────────────────────────

  pd.hms_errors.clear();
  if (doc["print"].containsKey("hms")) {
    JsonArray hmsList = doc["print"]["hms"].as<JsonArray>();
    for (JsonObject hms : hmsList) {
      HmsError e;
      e.attr     = hms["attr"].as<String>();
      e.code     = hms["code"].as<String>();
      e.severity = hms["severity"] | 3;
      pd.hms_errors.push_back(e);
      Serial.println("HMS: attr=" + e.attr + " code=" + e.code + " sev=" + String(e.severity));
    }
  }

  dataReceived[activeCallbackIndex] = true;
  Serial.println(
    printerConfigs[activeCallbackIndex].name + " [Bambu]: " + pd.gcode_state +
    " | " + pd.mc_percent + "%" +
    " | D1:" + pd.nozzle_temper_1 + "->" + pd.nozzle_target_1 +
    " | Bett:" + pd.bed_temper + "->" + pd.bed_target_temper +
    " | stg:" + pd.stg_cur +
    " | vt_remain:" + pd.vt_remain +
    " | Heap: " + String(ESP.getFreeHeap()) + " Bytes"
  );
}

void mqttCallback0(char* t, byte* p, unsigned int l) { activeCallbackIndex = 0; mqttCallbackGeneric(t, p, l); }
void mqttCallback1(char* t, byte* p, unsigned int l) { activeCallbackIndex = 1; mqttCallbackGeneric(t, p, l); }
void mqttCallback2(char* t, byte* p, unsigned int l) { activeCallbackIndex = 2; mqttCallbackGeneric(t, p, l); }

void (*mqttCallbacks[MAX_PRINTERS])(char*, uint8_t*, unsigned int) = {
  mqttCallback0, mqttCallback1, mqttCallback2
};

// ── BAMBU MQTT CONNECT ─────────────────────────────────────

void connectBambuAndWait(int idx) {
  wifiClients[idx].setInsecure();
  mqttClients[idx]->setServer(printerConfigs[idx].ip.c_str(), BAMBU_PORT);
  mqttClients[idx]->setCallback(mqttCallbacks[idx]);
  mqttClients[idx]->setBufferSize(32768);

  String clientId = "WatchFlow_" + String(random(0xffff), HEX);
  String subTopic = "device/" + printerConfigs[idx].serial + "/report";
  String cmdTopic = "device/" + printerConfigs[idx].serial + "/request";

  Serial.print("Bambu MQTT verbinde mit " + printerConfigs[idx].name + "...");
  if (!mqttClients[idx]->connect(clientId.c_str(), "bblp",
      printerConfigs[idx].lan_code.c_str())) {
    Serial.println("Fehler rc=" + String(mqttClients[idx]->state()));
    resetPrinterData(idx);
    return;
  }
  Serial.println("OK");

  mqttClients[idx]->subscribe(subTopic.c_str());
  mqttClients[idx]->publish(cmdTopic.c_str(),
    "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
  delay(500);
  mqttClients[idx]->publish(cmdTopic.c_str(),
    "{\"pushing\":{\"sequence_id\":\"2\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");

  dataReceived[idx] = false;
  unsigned long waitStart = millis();
  while (millis() - waitStart < MQTT_WAIT_MS) {
    mqttClients[idx]->loop();
    if (dataReceived[idx]) {
      Serial.println("Daten empfangen von " + printerConfigs[idx].name);
      break;
    }
    delay(10);
  }

  mqttClients[idx]->disconnect();
  wifiClients[idx].stop();

  if (!dataReceived[idx]) {
    Serial.println("Timeout -> " + printerConfigs[idx].name + " auf IDLE");
    resetPrinterData(idx);
  }
}

// ── MOONRAKER HTTP POLL ─────────────────────────────────────

void pollMoonraker(int idx) {
  PrinterData& pd = printerData[idx];
  String baseUrl = "http://" + printerConfigs[idx].ip + ":" + String(printerConfigs[idx].mr_port);

  String url = baseUrl + "/printer/objects/query?print_stats&display_status&extruder&heater_bed&fan&toolhead";
  WiFiClient plainClient;
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(plainClient, url);
  int code = http.GET();

  if (code != 200) {
    Serial.println(printerConfigs[idx].name + " [Moonraker] HTTP " + String(code) + " -> IDLE");
    resetPrinterData(idx);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.println(printerConfigs[idx].name + " [Moonraker] JSON Fehler: " + String(err.c_str()));
    resetPrinterData(idx);
    return;
  }

  JsonObject result = doc["result"]["status"];

  if (result.containsKey("print_stats")) {
    JsonObject ps = result["print_stats"];
    String state = ps["state"] | "standby";
    if      (state == "printing") pd.gcode_state = "RUNNING";
    else if (state == "paused")   pd.gcode_state = "PAUSE";
    else if (state == "error")    pd.gcode_state = "FAILED";
    else if (state == "complete") pd.gcode_state = "FINISH";
    else                          pd.gcode_state = "IDLE";

    pd.gcode_file      = ps["filename"] | "";
    pd.layer_num       = ps["current_layer"] | 0;
    pd.total_layer_num = ps["total_layer"]   | 0;

    float printDuration = ps["print_duration"] | 0.0f;
    float totalDuration = ps["total_duration"] | 0.0f;
    if (totalDuration > 0 && printDuration > 0) {
      float progress = printDuration / totalDuration;
      if (progress > 0 && progress < 1.0f) {
        float remaining = (totalDuration - printDuration) / 60.0f;
        pd.mc_remaining_time = (int)remaining;
      }
    }
  }

  if (result.containsKey("display_status")) {
    float prog = result["display_status"]["progress"] | 0.0f;
    pd.mc_percent = (int)(prog * 100.0f);
  }

  if (result.containsKey("extruder")) {
    pd.nozzle_temper_1 = result["extruder"]["temperature"] | 0.0f;
    pd.nozzle_target_1 = result["extruder"]["target"]      | 0.0f;
  }

  if (result.containsKey("heater_bed")) {
    pd.bed_temper        = result["heater_bed"]["temperature"] | 0.0f;
    pd.bed_target_temper = result["heater_bed"]["target"]      | 0.0f;
  }

  if (result.containsKey("fan")) {
    float fanSpeed = result["fan"]["speed"] | 0.0f;
    pd.fan_cooling = (int)(fanSpeed * 100.0f);
  }

  // Moonraker hat kein AMS und kein vt_tray → zurücksetzen
  pd.trays.clear();
  pd.hms_errors.clear();
  pd.stg_cur   = -1;
  pd.vt_remain = -1;

  Serial.println(
    printerConfigs[idx].name + " [Moonraker]: " + pd.gcode_state +
    " | " + pd.mc_percent + "%" +
    " | D:" + pd.nozzle_temper_1 + "->" + pd.nozzle_target_1 +
    " | Bett:" + pd.bed_temper + "->" + pd.bed_target_temper +
    " | Heap: " + String(ESP.getFreeHeap()) + " Bytes"
  );
}

// ── DISPATCHER: Bambu oder Moonraker ───────────────────────

void connectAndWait(int idx) {
  if (!printerConfigs[idx].active) return;
  if (printerConfigs[idx].type == TYPE_MOONRAKER) {
    pollMoonraker(idx);
  } else {
    connectBambuAndWait(idx);
  }
}

// ── API SEND (FilamentFlow VPS) ────────────────────────────

void sendToAPI(int i) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!printerConfigs[i].active) return;

  delay(500);
  PrinterData& pd = printerData[i];

  DynamicJsonDocument doc(8192);
  doc["user_email"]    = cfg_user_email;
  doc["name"]          = printerConfigs[i].name;
  doc["status"]        = pd.gcode_state;
  doc["progress"]      = pd.mc_percent;
  doc["remaining_min"] = pd.mc_remaining_time;
  doc["job_name"]      = pd.subtask_name;
  doc["gcode_file"]    = pd.gcode_file;
  doc["start_time"]    = pd.gcode_start_time;
  doc["fail_reason"]   = pd.fail_reason;
  doc["speed_level"]   = pd.spd_lvl;
  doc["layer"]         = pd.layer_num;
  doc["total_layers"]  = pd.total_layer_num;
  doc["nozzle_temp"]   = pd.nozzle_temper_1;
  doc["nozzle_temp_2"] = pd.nozzle_temper_2;
  doc["bed_temp"]      = pd.bed_temper;
  doc["chamber_temp"]  = pd.chamber_temper;
  doc["nozzle_target"]   = pd.nozzle_target_1;
  doc["nozzle_target_2"] = pd.nozzle_target_2;
  doc["bed_target"]      = pd.bed_target_temper;
  doc["printer_type"]    = (printerConfigs[i].type == TYPE_MOONRAKER) ? "moonraker" : "bambu";

  // ── NEU: stage + vt_tray ──
  doc["stage"] = pd.stg_cur;

  JsonObject vtTray = doc.createNestedObject("vt_tray");
  vtTray["tray_type"]       = pd.vt_tray_type;
  vtTray["tray_color"]      = "#" + (pd.vt_tray_color.length() >= 6
                                ? pd.vt_tray_color.substring(0, 6)
                                : pd.vt_tray_color);
  vtTray["remain"]          = pd.vt_remain;
  vtTray["tray_sub_brands"] = pd.vt_tray_sub_brands;
  vtTray["tray_info_idx"]   = pd.vt_tray_info_idx;
  vtTray["tray_diameter"]   = pd.vt_tray_diameter;
  vtTray["nozzle_temp_min"] = pd.vt_nozzle_temp_min;
  vtTray["nozzle_temp_max"] = pd.vt_nozzle_temp_max;
  vtTray["bed_temp"]        = pd.vt_bed_temp;
  vtTray["drying_temp"]     = pd.vt_drying_temp;
  vtTray["drying_time"]     = pd.vt_drying_time;
  // ─────────────────────────

  JsonObject fans = doc.createNestedObject("fans");
  fans["cooling"]   = pd.fan_cooling;
  fans["aux"]       = pd.fan_aux;
  fans["chamber"]   = pd.fan_chamber;
  fans["heatbreak"] = pd.fan_heatbreak;

  JsonArray amsArray = doc.createNestedArray("ams");
  int currentAmsId = -1;
  JsonObject currentAms;
  JsonArray  currentTrays;
  for (AmsTray& t : pd.trays) {
    if (t.ams_id != currentAmsId) {
      currentAms = amsArray.createNestedObject();
      currentAms["ams_id"]   = t.ams_id;
      currentAms["humidity"] = t.humidity;
      currentAms["temp"]     = t.temp;
      currentTrays = currentAms.createNestedArray("trays");
      currentAmsId = t.ams_id;
    }
    JsonObject tray = currentTrays.createNestedObject();
    tray["tray_id"]         = t.tray_id;
    tray["tray_type"]       = t.tray_type;
    tray["tray_color"]      = "#" + t.tray_color.substring(0, 6);
    tray["remain"]          = t.remain;
    tray["tray_sub_brands"] = t.tray_sub_brands;
    tray["tray_info_idx"]   = t.tray_info_idx;
    tray["tray_diameter"]   = t.tray_diameter;
    tray["nozzle_temp_min"] = t.nozzle_temp_min;
    tray["nozzle_temp_max"] = t.nozzle_temp_max;
    tray["bed_temp"]        = t.bed_temp;
    tray["drying_temp"]     = t.drying_temp;
    tray["drying_time"]     = t.drying_time;
  }

  JsonArray hmsArray = doc.createNestedArray("hms");
  for (HmsError& e : pd.hms_errors) {
    JsonObject hErr = hmsArray.createNestedObject();
    hErr["attr"]     = e.attr;
    hErr["code"]     = e.code;
    hErr["severity"] = e.severity;
  }

  String body;
  serializeJson(doc, body);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(secureClient, cfg_api_url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", cfg_api_key);
  int code = http.POST(body);
  if (code == 200) {
    String resp = http.getString();
    DynamicJsonDocument rd(2048);
    if (!deserializeJson(rd, resp)) {
      int newIntervalRaw = rd["poll_interval_ms"] | 0;
      if (newIntervalRaw >= 10000 && newIntervalRaw <= 300000) {
        currentInterval = (unsigned long)newIntervalRaw;
        Serial.println("Poll-Intervall: " + String(currentInterval/1000) + "s");
      }
    }
  }
  Serial.println(printerConfigs[i].name + " POST -> HTTP " + String(code) +
                 " | Intervall: " + String(currentInterval/1000) + "s" +
                 " | Heap: " + String(ESP.getFreeHeap()) + " Bytes");
  http.end();
  secureClient.stop();
  delay(200);
}

// ── SETUP & LOOP ───────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n⌚ WatchFlow v2.2 starting...");

  pinMode(SETUP_PIN, INPUT_PULLUP);
  Serial.print("Setup-Button pruefen");
  bool setupPressed = true;
  for (int i = 0; i < 6; i++) {
    delay(500); Serial.print(".");
    if (digitalRead(SETUP_PIN) == HIGH) {
      setupPressed = false;
      break;
    }
  }
  Serial.println();

  if (setupPressed) {
    Serial.println("Setup-Button gedrueckt -> Setup-Modus");
    loadConfig();
    startConfigMode();
    return;
  }

  for (int i = 0; i < MAX_PRINTERS; i++)
    mqttClients[i] = new PubSubClient(wifiClients[i]);

  bool hasConfig = loadConfig();
  if (!hasConfig) {
    Serial.println("Keine Konfiguration -> Setup-Modus");
    startConfigMode();
    return;
  }

  Serial.print("Verbinde mit WLAN: " + cfg_wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWLAN Fehler -> Setup-Modus");
    loadConfig();
    startConfigMode();
    return;
  }

  Serial.println("\nWLAN: " + WiFi.localIP().toString());
  Serial.println("Heap beim Start: " + String(ESP.getFreeHeap()) + " Bytes");

  int activeCount = 0;
  for (int i = 0; i < MAX_PRINTERS; i++) {
    if (printerConfigs[i].active) {
      activeCount++;
      Serial.println("  Drucker " + String(i+1) + ": " + printerConfigs[i].name +
                     " [" + (printerConfigs[i].type == TYPE_MOONRAKER ? "Moonraker" : "Bambu") + "]");
    }
  }
  Serial.println("Aktive Drucker: " + String(activeCount));

  for (int i = 0; i < MAX_PRINTERS; i++) {
    if (printerConfigs[i].active) {
      currentPrinter = i;
      connectAndWait(i);
      sendToAPI(i);
      break;
    }
  }

  lastPrinterSwitch = millis();
}

void loop() {
  if (configMode) {
    server.handleClient();
    return;
  }

  // Auto-Neustart nach 6 Stunden (Heap-Schutz)
  if (millis() > AUTO_RESTART_MS) {
    Serial.println("Planmaessiger Neustart nach 6h...");
    delay(500);
    ESP.restart();
  }

  if (millis() - lastPrinterSwitch > currentInterval) {
    int next = currentPrinter;
    for (int i = 1; i <= MAX_PRINTERS; i++) {
      next = (currentPrinter + i) % MAX_PRINTERS;
      if (printerConfigs[next].active) break;
    }
    currentPrinter = next;
    connectAndWait(currentPrinter);
    sendToAPI(currentPrinter);
    lastPrinterSwitch = millis();
    Serial.println("=== Wechsle zu: " + printerConfigs[currentPrinter].name +
                   " [" + (printerConfigs[currentPrinter].type == TYPE_MOONRAKER ? "Moonraker" : "Bambu") + "] ===");
  }
}
