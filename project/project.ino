#include <Arduino.h>
#include <esp_task_wdt.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc_cal.h"
#include <SPI.h>
#include "pin_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <Preferences.h>
#include <math.h>   // for sin(), cos(), PI



// ——— Wi-Fi credentials —————————————————————————————————
const char* ssid     = "";
const char* password = "";

// ——— TFT setup ——————————————————————————————————————
TFT_eSPI tft = TFT_eSPI();

// ——— Buttons ——————————————————————————————————————
#define BTN1 0
#define BTN2 14

// ——— App state —————————————————————————————————————
enum ScreenState { BOOT, FORECAST, MENU, SETTINGS, HISTORIK };
ScreenState currentScreen = BOOT;

const char* programVersion = "v1.0.0";
const char* teamNumber     = "Team 3";


// — menu —————————————————————————————————————————
int  menuIdx = 0;
const int MENU_OPTS = 4;
const char* menuItems[MENU_OPTS] = { "Forecast", "Historical", "Settings", "Reset" };


// — cities ———————————————————————————————————————
enum City { STOCKHOLM=0, MALMO=1, GOTHENBURG=2 };
City selectedCity = STOCKHOLM;
const int NUM_CITIES = 3;
const char* cityNames[NUM_CITIES] = { "Stockholm", "Malmo", "Gothenburg" };

// — parameters —————————————————————————————————————
enum Param { TEMP=0, WIND=1, HUM=2 };
Param selectedParam = TEMP;
const int NUM_PARAMS = 3;
const char* paramNames[NUM_PARAMS] = { "Temperature", "Windspeed", "Humidity" };
const char* paramCodes[NUM_PARAMS] = { "t", "ws", "r" };
const char* paramUnits[NUM_PARAMS] = { "°C", " m/s", "%" };

static const int histParamID[NUM_PARAMS]   = { 1, 4, 6 };    // 1=Temp, 4=Wind speed, 7=Rel. humidity//
// Indexed as histStationID[city][param]
static const int histStationID[NUM_CITIES][NUM_PARAMS] = {
  { 98230, 98210, 98230 },  // Stockholm: temp, wind, humidity //
  { 52350, 52350, 52350 },  
  { 71420, 71420, 71420 }   
};


// — settings submenu —————————————————————————————
int settingsMode = 0;  // 0=pick city, 1=pick param


// ──────────────────────────────────────────────────────────────
// Button-state & timing globals 
// ──────────────────────────────────────────────────────────────

// raw button debounces
bool prevB1 = false;
bool prevB2 = false;

// both-button “hold to menu” "detector"
bool   bothPressed       = false;
bool   bothMenuTriggered = false;
unsigned long bothPressStart = 0;
const unsigned long BOTH_PRESS_THRESHOLD = 500;  // ms hold to open menu

// HISTORIK scroll timing for BTN2
unsigned long b2PressTime = 0;
bool          b2LongHandled = false;
const unsigned long LONG_PRESS_MS = 800;  // ms for BTN2 (long-press)


Preferences prefs;

// — SMHI URLs ——————————————————————————————————————
const char* fcUrls[NUM_CITIES] = {
  "https://opendata-download-metfcst.smhi.se/api/category/pmp3g/version/2/geotype/point/lon/18.0686/lat/59.3293/data.json",
  "https://opendata-download-metfcst.smhi.se/api/category/pmp3g/version/2/geotype/point/lon/13.0708/lat/55.5715/data.json",
  "https://opendata-download-metfcst.smhi.se/api/category/pmp3g/version/2/geotype/point/lon/11.9924/lat/57.7156/data.json"
};
// latest months
const char* histUrls[NUM_CITIES] = {
  "https://opendata-download-metobs.smhi.se/api/version/latest/parameter/1/station/98230/period/latest-months/data.json",
  "https://opendata-download-metobs.smhi.se/api/version/latest/parameter/1/station/52350/period/latest-months/data.json",
  "https://opendata-download-metobs.smhi.se/api/version/latest/parameter/1/station/71420/period/latest-months/data.json"
};

// — history-view state —————————————————————————
bool  hist7Days      = true;    // true=7d window; false=1d window //
int   histOffsetDays = 0;       // how many days back from “most recent” //



// ──────────────── ICONS ──────────────────────────────────

// SUN //
void drawSun(int x,int y) {
  tft.fillCircle(x,y,3,TFT_WHITE);
  for(int i=0;i<8;i++){
    float a=i*(PI/4);
    tft.drawLine(x,y, x+cos(a)*5, y+sin(a)*5, TFT_WHITE);
  }
}

//Cloud//
void drawCloud(int x,int y) {
  tft.fillRect(x-6,y,12,4,TFT_WHITE);
  tft.fillCircle(x-4,y,3,TFT_WHITE);
  tft.fillCircle(x,y-2,3,TFT_WHITE);
  tft.fillCircle(x+4,y,3,TFT_WHITE);
}

//Cloud with rain//
void drawRain(int x,int y) {
  drawCloud(x,y-1);
  for(int dx=-3;dx<=3;dx+=3)
    tft.drawLine(x+dx,y+4, x+dx,y+8, TFT_WHITE);
}

//Thunder//
void drawThunder(int x,int y) {
  drawCloud(x,y-1);
  int px1[] = {x, x-2, x+1}, py1[] = {y+2,y+8,y+8};
  tft.fillTriangle(px1[0],py1[0], px1[1],py1[1], px1[2],py1[2], TFT_WHITE);
  int px2[] = {x+1,x-1,x+3}, py2[] = {y+8,y+14,y+5};
  tft.fillTriangle(px2[0],py2[0], px2[1],py2[1], px2[2],py2[2], TFT_WHITE);
}

//Snow//
void drawSnow(int x,int y) {
  for(int i=0;i<6;i++){
    float a=i*(PI/3);
    tft.drawLine(x,y, x+cos(a)*6, y+sin(a)*6, TFT_WHITE);
  }
}

// ──────────────── UTILITIES ──────────────────────────────
void connectWiFi() {
  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Connecting to WI-FI...",10,10);
  }
}

char* decodeChunkedBuffer(const char* in) {
  int len=strlen(in),pos=0,outPos=0;
  char* out=(char*)malloc(len+1);
  if(!out) return nullptr;
  while(pos<len){
    const char* e=strstr(in+pos,"\r\n");
    if(!e) break;
    int h=e-(in+pos);
    char hdr[32]; strncpy(hdr,in+pos,min(h,31)); hdr[min(h,31)]='\0';
    int sz=strtol(hdr,nullptr,16);
    if(sz<=0) break;
    pos+=h+2;
    if(pos+sz>len) break;
    memcpy(out+outPos, in+pos, sz);
    outPos+=sz; pos+=sz+2;
  }
  out[outPos]=0;
  return out;
}


char* fetchSMHIData(bool historical=false) {
  String url;
  if (!historical) {
    url = fcUrls[selectedCity];
  } else {
    url = String("https://opendata-download-metobs.smhi.se/api/version/latest")
    + "/parameter/" + String(histParamID[selectedParam])
    + "/station/"  + String(histStationID[selectedCity][selectedParam])
    + "/period/latest-months/data.json";

  }

  tft.fillScreen(TFT_BLACK);
  tft.drawString("Loading data...",10,10);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return nullptr;
  }
  WiFiClient* stream = http.getStreamPtr();
  const int BUF = 200000;
  char* buf = (char*)malloc(BUF+1);
  int idx=0; unsigned long t0=millis();
  while(http.connected() && millis()-t0<10000 && idx<BUF){
    while(stream->available() && idx<BUF){
      buf[idx++] = stream->read();
      t0 = millis();
    }
    delay(5);
  }
  http.end();
  buf[idx] = '\0';
  return (buf[0]=='{'||buf[0]=='[') ? buf : decodeChunkedBuffer(buf);
}


// ──────────────── FORECAST ───────────────────────────────
void showForecast(char* json) {
  if(!json){
    tft.fillScreen(TFT_BLACK);
    tft.drawString("HTTP FEL",10,10);
    return;
  }
  DynamicJsonDocument doc(200000);
  if(deserializeJson(doc,json)){
    free(json);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("JSON FEL",10,10);
    return;
  }
  free(json);

  auto arr = doc["timeSeries"].as<JsonArray>();
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("Prognos "+String(cityNames[selectedCity]),10,10);

  int n=min(24,(int)arr.size());
  const int LX=10,RX=170,Y0=30,LH=12;
  for(int i=0;i<n;i++){
    auto e = arr[i].as<JsonObject>();
    String tm = String(e["validTime"].as<const char*>()).substring(11,16);
    float val=NAN; int sym=0;
    for(auto v:e["parameters"].as<JsonArray>()){
      auto p=v.as<JsonObject>();
      if(!strcmp(p["name"].as<const char*>(),paramCodes[selectedParam]))
        val=p["values"][0].as<float>();
      if(!strcmp(p["name"].as<const char*>(),"Wsymb2"))
        sym=p["values"][0].as<int>();
    }
    int col=i<12?0:1,row=i<12?i:i-12;
    int x=col?RX:LX, y=Y0+row*LH;
    String txt = tm + ": " + (isnan(val)?"--":String(val,1)+paramUnits[selectedParam]);
    tft.drawString(txt,x,y);
    int ix=x+tft.textWidth(txt)+8,
        iy=y+(tft.fontHeight()/2)-1;
    switch(sym){
      case 1: case 2: case 3: case 4: drawSun(ix,iy);    break;
      case 5: case 6: case 7:           drawCloud(ix,iy);  break;
      case 8: case 9: case 10:          drawRain(ix,iy);   break;
      case 11:                          drawThunder(ix,iy);break;
      case 12: case 13: case 14:        drawSnow(ix,iy);   break;
    }
  }
  tft.setTextSize(2);
}

// ──────────────── HISTORY GRAPH ──────────────────────────
void showHistorik(char* json) {
  if (!json) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("HTTP FEL", 10, 10);
    return;
  }
  DynamicJsonDocument doc(200000);
  auto err = deserializeJson(doc, json);
  free(json);
  if (err) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("JSON FEL", 10, 10);
    return;
  }

  JsonArray vals;
  if (doc.is<JsonArray>()) {
    vals = doc.as<JsonArray>();
  } else if (doc.containsKey("value")) {
    vals = doc["value"].as<JsonArray>();
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("INGA DATA", 10, 10);
    return;
  }

  int total = vals.size();
  if (total == 0) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("INGA TIMMAR", 10, 10);
    return;
  }

  int window = hist7Days ? 168 : 24;
  window = min(window, total);
  int maxOffset = (total - window) / 24;
  histOffsetDays = constrain(histOffsetDays, 0, maxOffset);

  int startIdx = total - window - histOffsetDays * 24;
  startIdx = max(0, startIdx);

  // Text in the corner in history //
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  String mode = hist7Days ? "Mode: 7d" : "Mode: 1d";
  tft.drawString(mode, tft.width() - tft.textWidth(mode) - 5, 10);
  String legend = "B2 short=<-  long=->";
  tft.drawString(legend, tft.width() - tft.textWidth(legend) - 5, 22);

  // plot
  drawHistorikGraph(vals, startIdx, window);
}

void drawHistorikGraph(JsonArray arr, int startIdx, int count) {
  const int GX = 20, GY = 40;
  const int GW = tft.width()  - 40;
  const int GH = tft.height() - 60;

  // 1 find min/max
  float minv =  1e6, maxv = -1e6;
  for (int i = 0; i < count; i++) {
    float v = arr[startIdx + i]["value"].as<float>();  // <- correct here
    minv = min(minv, v);
    maxv = max(maxv, v);
  }
  if (minv == maxv) {
    minv -= 1;
    maxv += 1;
  }

  // 2 clear only the graph box
  tft.fillRect(GX+1, GY+1, GW-1, GH-1, TFT_BLACK);
  tft.drawRect(GX, GY, GW, GH, TFT_WHITE);

  // 3 draw Y-axis labels at x=2 
  tft.setTextSize(1);
  String topLbl = String(maxv, 1) + paramUnits[selectedParam];
  String botLbl = String(minv, 1) + paramUnits[selectedParam];
  tft.drawString(topLbl, 2, GY - 6);
  tft.drawString(botLbl, 2, GY + GH - 6);

  // 4 plot the line
  int px = -1, py = -1;
  for (int i = 0; i < count; i++) {
    float v = arr[startIdx + i]["value"].as<float>();  // <- and here
    int x = GX + (GW * i) / (count - 1);
    int y = GY + GH - int((v - minv) / (maxv - minv) * GH);
    if (i > 0) {
      tft.drawLine(px, py, x, y, TFT_CYAN);
    }
    px = x;
    py = y;
  }
}

// ──────────────── MENU & SETTINGS ───────────────────────
void drawMenu(){
  tft.fillScreen(TFT_BLACK);
  tft.drawString("MENY",10,10);
  for(int i=0;i<MENU_OPTS;i++){
    tft.setTextColor(i==menuIdx?TFT_YELLOW:TFT_WHITE);
    tft.drawString(menuItems[i],10,40+i*30);
  }
  tft.setTextColor(TFT_WHITE);
}

void drawSettings(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("Settings",10,10);
  if(settingsMode==0){
    tft.drawString("Choose city:",10,30);
    for(int i=0;i<NUM_CITIES;i++){
      tft.setTextColor(i==selectedCity?TFT_YELLOW:TFT_WHITE);
      tft.drawString(cityNames[i],10,50+i*20);
    }
  } else {
    tft.drawString("Choose parameter:",10,30);
    for(int i=0;i<NUM_PARAMS;i++){
      tft.setTextColor(i==selectedParam?TFT_YELLOW:TFT_WHITE);
      tft.drawString(paramNames[i],10,50+i*20);
    }
  }
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
}

// ──────────────── SCREEN ROUTER ─────────────────────────
void updateScreen(){
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  switch(currentScreen){
    case BOOT:
      tft.fillScreen(TFT_BLACK);
      tft.drawString("Starting...",10,10);
      tft.drawString("Version: "+String(programVersion),10,40);
      tft.drawString("Team: "+String(teamNumber),10,70);
      delay(2000);
      currentScreen=FORECAST;
      updateScreen();
      break;
    case FORECAST:
      showForecast(fetchSMHIData(false));
      break;
    case MENU:
      drawMenu();
      break;
    case SETTINGS:
      drawSettings();
      break;
    case HISTORIK:
      showHistorik(fetchSMHIData(true));
      break;
  }
}

// ──────────────── BUTTON HANDLING ───────────────────────
void handleButtons() {
  bool b1 = (digitalRead(BTN1) == LOW);
  bool b2 = (digitalRead(BTN2) == LOW);

  // 1) BOTH-BUTTON “hold and release” to get MENU
  static bool  bothPressed = false;
  static unsigned long bothStart = 0;
  
  if (b1 && b2) {
    if (!bothPressed) {
      bothPressed = true;
      bothStart   = millis();
    }
    // while both buttons are down, do nothing else
    return;
  }
  // on release after hold
  if (bothPressed && !b1 && !b2) {
    bothPressed = false;
    if (millis() - bothStart >= BOTH_PRESS_THRESHOLD) {
      currentScreen = MENU;
      updateScreen();
      
      return;
    }
    // else, fall through to single-button logic
  }

  // 2) BTN1 short-press on release
  static bool prev1 = HIGH;
  if (!b1 && prev1) {
    if (currentScreen == HISTORIK) {
      hist7Days      = !hist7Days;
      histOffsetDays = 0;
      updateScreen();
    }
    else if (currentScreen == MENU) {
  if      (menuIdx == 0) {
    currentScreen = FORECAST;
    updateScreen();
  }
  else if (menuIdx == 1) {
    currentScreen = HISTORIK;
    updateScreen();
  }
  else if (menuIdx == 2) {
    currentScreen = SETTINGS;
    settingsMode = 0;
    updateScreen();
  }
  else {
    // RESET option selected
    selectedCity = STOCKHOLM;
    selectedParam = TEMP;
    prefs.begin("cfg", false);
    prefs.putUInt("city", (uint32_t)selectedCity);
    prefs.putUInt("param", (uint32_t)selectedParam);
    prefs.end();

    // Show confirmation
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Settings", 10, 10);
    tft.drawString("Reset", 10, 40);
    delay(2000);

    currentScreen = MENU;
    updateScreen();
  }
}


    else if (currentScreen == SETTINGS) {
      if (settingsMode == 0) {
        settingsMode = 1;
        drawSettings();
      } else {
        prefs.begin("cfg", false);
        prefs.putUInt("city",  (uint32_t)selectedCity);
        prefs.putUInt("param", (uint32_t)selectedParam);
        prefs.end();
        currentScreen = MENU;
        updateScreen();
      }
    }
    else {
      currentScreen = MENU;
      updateScreen();
    }
  }
  prev1 = b1;

  // 3) BTN2 long vs short on release for HISTORIK to scroll throught the days
  static bool          prev2 = HIGH;
  static unsigned long down2 = 0;

  if (b2 && !prev2) {
    down2 = millis();
  }
  if (!b2 && prev2) {
    unsigned long held = millis() - down2;
    if (currentScreen == HISTORIK) {
      if (held >= LONG_PRESS_MS) {
        // forward one day
        histOffsetDays = max(0, histOffsetDays - 1);
      } else {
        // backward one day
        histOffsetDays = histOffsetDays + 1;
      }
      updateScreen();
    }
    else if (currentScreen == MENU) {
      menuIdx = (menuIdx + 1) % MENU_OPTS;
      drawMenu();
    }
    else if (currentScreen == SETTINGS) {
      if (settingsMode == 0)
        selectedCity = City((selectedCity + 1) % NUM_CITIES);
      else
        selectedParam = Param((selectedParam + 1) % NUM_PARAMS);
      drawSettings();
    }
  }
  prev2 = b2;
}

void setup(){
  Serial.begin(115200);
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);

  prefs.begin("cfg", true);
  selectedCity  = City(prefs.getUShort("city", STOCKHOLM));
  selectedParam = Param(prefs.getUShort("param", TEMP));
  prefs.end();

  connectWiFi();
  updateScreen();
}

void loop(){
  handleButtons();
  delay(50);
}


// TFT Pin check
  //////////////////
 // DO NOT TOUCH //
//////////////////
#if PIN_LCD_WR  != TFT_WR || \
    PIN_LCD_RD  != TFT_RD || \
    PIN_LCD_CS    != TFT_CS   || \
    PIN_LCD_DC    != TFT_DC   || \
    PIN_LCD_RES   != TFT_RST  || \
    PIN_LCD_D0   != TFT_D0  || \
    PIN_LCD_D1   != TFT_D1  || \
    PIN_LCD_D2   != TFT_D2  || \
    PIN_LCD_D3   != TFT_D3  || \
    PIN_LCD_D4   != TFT_D4  || \
    PIN_LCD_D5   != TFT_D5  || \
    PIN_LCD_D6   != TFT_D6  || \
    PIN_LCD_D7   != TFT_D7  || \
    PIN_LCD_BL   != TFT_BL  || \
    TFT_BACKLIGHT_ON   != HIGH  || \
    170   != TFT_WIDTH  || \
    320   != TFT_HEIGHT
#error  "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error  "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error  "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error  "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
#error  "The current version is not supported for the time being, please use a version below Arduino ESP32 3.0"
#endif
