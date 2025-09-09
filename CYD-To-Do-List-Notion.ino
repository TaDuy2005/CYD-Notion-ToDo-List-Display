#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <algorithm>
#include <math.h>

TFT_eSPI tft;

// -------- Wi-Fi / Notion --------
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PSK  = "YOUR_PASS";

const char* NOTION_SECRET   = "secret_xxx";     // Internal Integration Token (store in NVS for production)
const char* DATABASE_ID     = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; // 32-char (or dashed) id
const char* NOTION_VERSION = "2022-06-28";

const bool USE_INSECURE_TLS = true; // demo

// -------- Touch pins (CYD) --------
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

SPIClass tsSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// -------- Screen / UI --------
#define SCREEN_W 240
#define SCREEN_H 320

const uint16_t COLOR_BG      = TFT_BLACK;
const uint16_t COLOR_TITLE   = TFT_CYAN;
const uint16_t COLOR_TEXT    = TFT_WHITE;
const uint16_t COLOR_CHECKED = TFT_GREEN;
const uint16_t COLOR_UNCHECK = TFT_DARKGREY;
const uint16_t COLOR_DIVIDER = TFT_DARKGREY;
const uint16_t COLOR_HL      = TFT_NAVY;
const uint16_t COLOR_BTN     = TFT_DARKGREY;
const uint16_t COLOR_BTN_TXT = TFT_WHITE;

const uint8_t  FONT_TITLE    = 4;
const uint8_t  FONT_ITEM     = 2;

const int16_t  LIST_X     = 10;
const int16_t  LIST_Y     = 56;
const int16_t  ROW_H      = 30;
const int16_t  CHECK_SIZE = 22;
const int16_t  CHECK_PAD  = 8;

// Bottom buttons: UP | REFRESH | DOWN
const int16_t  BTN_BAR_H  = 40;
const int16_t  BTN_Y      = SCREEN_H - BTN_BAR_H;
const int16_t  BTN_W      = SCREEN_W/3;
const int16_t  BTN_H      = BTN_BAR_H;
const int16_t  BTN_UP_X   = 0;
const int16_t  BTN_RF_X   = BTN_W;
const int16_t  BTN_DN_X   = BTN_W*2;

// -------- Data model --------
struct TaskItem {
  String text;
  String status;
  bool   done;
};
const int MAX_TASKS = 50;
TaskItem tasks[MAX_TASKS];
int taskCount = 0;

// Pagination
uint8_t firstIdx = 0;
uint8_t visibleRows = 0;

// -------- Calibration (5-point LS affine) --------
struct Cal { double a,b,c,d,e,f; bool valid; } cal;
Preferences prefs;

bool readRawAvg(uint16_t &rx, uint16_t &ry, uint16_t samples=28, uint16_t zMin=10, uint32_t pressTimeout=8000) {
  uint32_t t0=millis();
  while (!(ts.tirqTouched() && ts.touched())) { if (millis()-t0>pressTimeout) return false; delay(3); }
  uint32_t sx=0, sy=0; uint16_t n=0;
  while (n<samples) {
    if (ts.tirqTouched() && ts.touched()) { TS_Point p=ts.getPoint(); if (p.z>=zMin){ sx+=p.x; sy+=p.y; n++; } }
    delay(2);
  }
  rx = sx/n; ry = sy/n;
  while (ts.tirqTouched() && ts.touched()) delay(2);
  delay(60);
  return true;
}
bool solve3(double M[3][3], double V[3], double X[3]) {
  for (int i=0;i<3;i++){
    int p=i; for(int r=i+1;r<3;r++) if (fabs(M[r][i])>fabs(M[p][i])) p=r;
    if (fabs(M[p][i])<1e-12) return false;
    if (p!=i){ for(int c=i;c<3;c++) std::swap(M[i][c],M[p][c]); std::swap(V[i],V[p]); }
    double piv=M[i][i]; for(int c=i;c<3;c++) M[i][c]/=piv; V[i]/=piv;
    for(int r=0;r<3;r++) if(r!=i){ double f=M[r][i]; for(int c=i;c<3;c++) M[r][c]-=f*M[i][c]; V[r]-=f*V[i]; }
  }
  X[0]=V[0]; X[1]=V[1]; X[2]=V[2]; return true;
}
bool fitAffineLS(const uint16_t *xr,const uint16_t *yr,const int16_t *xs,const int16_t *ys,int N, Cal &out){
  double S_xx=0,S_xy=0,S_x1=0,S_yy=0,S_y1=0,S_11=N;
  double Tx=0, Uy=0, V1=0, Tx2=0, Uy2=0, V12=0;
  for (int i=0;i<N;i++){ double X=xr[i],Y=yr[i],XS=xs[i],YS=ys[i];
    S_xx+=X*X; S_xy+=X*Y; S_x1+=X; S_yy+=Y*Y; S_y1+=Y;
    Tx+=X*XS; Uy+=Y*XS; V1+=XS;  Tx2+=X*YS; Uy2+=Y*YS; V12+=YS; }
  double M1[3][3]={{S_xx,S_xy,S_x1},{S_xy,S_yy,S_y1},{S_x1,S_y1,S_11}}, Vx[3]={Tx,Uy,V1}, X1[3];
  double M2[3][3]={{S_xx,S_xy,S_x1},{S_xy,S_yy,S_y1},{S_x1,S_y1,S_11}}, Vy[3]={Tx2,Uy2,V12}, X2[3];
  if(!solve3(M1,Vx,X1) || !solve3(M2,Vy,X2)) return false;
  out.a=X1[0]; out.b=X1[1]; out.c=X1[2]; out.d=X2[0]; out.e=X2[1]; out.f=X2[2]; out.valid=true; return true;
}
bool applyAffine(int16_t &sx,int16_t &sy){
  if(!cal.valid || !(ts.tirqTouched()&&ts.touched())) return false;
  TS_Point p=ts.getPoint(); double xr=p.x, yr=p.y;
  double xs=cal.a*xr+cal.b*yr+cal.c, ys=cal.d*xr+cal.e*yr+cal.f;
  if(xs<0) xs=0; if(xs>SCREEN_W-1) xs=SCREEN_W-1; if(ys<0) ys=0; if(ys>SCREEN_H-1) ys=SCREEN_H-1;
  sx=(int16_t)xs; sy=(int16_t)ys; return true;
}
void crosshair(int16_t x,int16_t y,uint16_t col){ const int L=9; tft.drawFastHLine(x-L,y,2*L+1,col); tft.drawFastVLine(x,y-L,2*L+1,col); tft.drawCircle(x,y,L+3,col); }
bool runCalibrationLS(){
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE,TFT_BLACK);
  tft.setTextDatum(TC_DATUM); tft.drawString("Touch Calibration",SCREEN_W/2,12,2);
  tft.setTextDatum(TL_DATUM); tft.drawString("Tap the crosshair",8,32,2);
  const int M=26; struct Pt{int16_t x,y;} T[5]={{M,M},{SCREEN_W-M,M},{SCREEN_W/2,SCREEN_H/2},{SCREEN_W-M,SCREEN_H-M},{M,SCREEN_H-M}};
  uint16_t rx[5],ry[5]; int16_t xs[5],ys[5]; for(int i=0;i<5;i++){ xs[i]=T[i].x; ys[i]=T[i].y; }
  for(int i=0;i<5;i++){
    tft.fillRect(0,60,SCREEN_W,SCREEN_H-60,TFT_BLACK); crosshair(T[i].x,T[i].y,TFT_YELLOW);
    tft.setTextDatum(TC_DATUM); tft.drawString(String("Target ")+String(i+1)+"/5",SCREEN_W/2,60,2);
    uint16_t ax,ay; uint32_t t0=millis(); while(!readRawAvg(ax,ay,28,10,15000)){ if(millis()-t0>16000){ tft.setTextDatum(TC_DATUM); tft.drawString("Timeout. Try again.",SCREEN_W/2,90,2); delay(800); t0=millis(); } }
    rx[i]=ax; ry[i]=ay; crosshair(T[i].x,T[i].y,TFT_GREEN); delay(120);
  }
  if(!fitAffineLS(rx,ry,xs,ys,5,cal)){ tft.fillRect(0,60,SCREEN_W,SCREEN_H-60,TFT_BLACK); tft.setTextDatum(TC_DATUM); tft.drawString("Calibration failed",SCREEN_W/2,90,2); delay(900); return false; }
  prefs.begin("touch",false); prefs.putBytes("cal",&cal,sizeof(Cal)); prefs.end();
  tft.fillRect(0,60,SCREEN_W,SCREEN_H-60,TFT_BLACK); tft.setTextDatum(TL_DATUM); tft.drawString("Cal OK",8,70,2); delay(400);
  return true;
}
bool loadCalibration(){ prefs.begin("touch",true); size_t n=prefs.getBytesLength("cal"); bool ok=false; if(n==sizeof(Cal)){ prefs.getBytes("cal",&cal,sizeof(Cal)); ok=cal.valid; } prefs.end(); return ok; }

// -------- UI helpers --------
void drawHeader(const char* subMsg) {
  tft.fillRect(0, 0, SCREEN_W, LIST_Y, COLOR_BG); // FIX: 5-arg fillRect
  tft.setTextColor(COLOR_TITLE, COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("To-Do List", 8, 8, FONT_TITLE);
  tft.setTextColor(TFT_DARKGREY, COLOR_BG);
  if (subMsg) tft.drawString(subMsg, 8, 36, 1);
}
void drawButtons() {
  tft.fillRect(0, BTN_Y, SCREEN_W, BTN_H, COLOR_BG);
  // UP
  tft.fillRect(BTN_UP_X+6, BTN_Y+6, BTN_W-12, BTN_H-12, COLOR_BTN);
  tft.setTextColor(COLOR_BTN_TXT, COLOR_BTN); tft.setTextDatum(MC_DATUM);
  tft.drawString("UP", BTN_UP_X + BTN_W/2, BTN_Y + BTN_H/2, 2);
  // REFRESH
  tft.fillRect(BTN_RF_X+6, BTN_Y+6, BTN_W-12, BTN_H-12, COLOR_BTN);
  tft.drawString("REFRESH", BTN_RF_X + BTN_W/2, BTN_Y + BTN_H/2, 2);
  // DOWN
  tft.fillRect(BTN_DN_X+6, BTN_Y+6, BTN_W-12, BTN_H-12, COLOR_BTN);
  tft.drawString("DOWN", BTN_DN_X + BTN_W/2, BTN_Y + BTN_H/2, 2);
}
void drawCheckbox(int16_t x,int16_t y,bool checked){
  tft.drawRect(x,y,CHECK_SIZE,CHECK_SIZE, checked?COLOR_CHECKED:COLOR_UNCHECK);
  if(checked){
    int16_t x1=x+3,y1=y+CHECK_SIZE/2+1, x2=x+CHECK_SIZE/2-1,y2=y+CHECK_SIZE-4, x3=x+CHECK_SIZE-3,y3=y+4;
    tft.drawLine(x1,y1,x2,y2,COLOR_CHECKED); tft.drawLine(x2,y2,x3,y3,COLOR_CHECKED);
  }
}
void drawRow(int gi, bool highlight=false){
  int16_t y = LIST_Y + (gi-firstIdx)*ROW_H;
  tft.fillRect(0,y,SCREEN_W,ROW_H, highlight?COLOR_HL:COLOR_BG);
  tft.drawLine(0,y+ROW_H-1,SCREEN_W,y+ROW_H-1,COLOR_DIVIDER);
  int16_t cbx=LIST_X, cby=y+(ROW_H-CHECK_SIZE)/2;
  drawCheckbox(cbx,cby,tasks[gi].done);
  String line = tasks[gi].text;
  if (tasks[gi].status.length()) line += " [" + tasks[gi].status + "]";
  if (line.length() > 34) line = line.substring(0, 34) + "...";
  tft.setTextColor(COLOR_TEXT, highlight?COLOR_HL:COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(line, LIST_X + CHECK_SIZE + CHECK_PAD, y+6, FONT_ITEM);
}
void drawList(){
  int16_t listH = SCREEN_H - LIST_Y - BTN_BAR_H;
  tft.fillRect(0, LIST_Y, SCREEN_W, listH, COLOR_BG);
  // FIX: avoid std::min type issues
  uint8_t endIdx = firstIdx + visibleRows;
  if (endIdx > taskCount) endIdx = taskCount;
  for(int i=firstIdx; i<endIdx; i++) drawRow(i,false);
  if (taskCount == 0) {
    tft.setTextColor(TFT_RED, COLOR_BG);
    tft.drawString("No tasks found.", 8, LIST_Y+6, 2);
  }
}
void drawAll(const char* subMsg){
  drawHeader(subMsg);
  drawList();
  drawButtons();
}

// -------- Hit testing --------
bool inBtnUp(int16_t x,int16_t y){ return (x>=BTN_UP_X && x<BTN_UP_X+BTN_W && y>=BTN_Y && y<BTN_Y+BTN_H); }
bool inBtnRf(int16_t x,int16_t y){ return (x>=BTN_RF_X && x<BTN_RF_X+BTN_W && y>=BTN_Y && y<BTN_Y+BTN_H); }
bool inBtnDn(int16_t x,int16_t y){ return (x>=BTN_DN_X && x<BTN_DN_X+BTN_W && y>=BTN_Y && y<BTN_Y+BTN_H); }
bool inListArea(int16_t x,int16_t y){ return (y>=LIST_Y && y<BTN_Y); }
uint8_t idxFromPoint(int16_t x,int16_t y){
  if(!inListArea(x,y)) return 255;
  int16_t rel = (y - LIST_Y) / ROW_H;
  uint8_t gi = firstIdx + rel;
  if (gi >= taskCount || rel<0 || rel>=visibleRows) return 255;
  return gi;
}
bool inCheckbox(int gi,int16_t x,int16_t y){
  int16_t rowY = LIST_Y + (gi-firstIdx)*ROW_H;
  int16_t cbx=LIST_X, cby=rowY+(ROW_H-CHECK_SIZE)/2;
  return (x>=cbx && x<=cbx+CHECK_SIZE && y>=cby && y<=cby+CHECK_SIZE);
}

// -------- Paging --------
void pageUp(){
  if(firstIdx >= visibleRows) firstIdx -= visibleRows;
  else firstIdx = 0;
  drawList();
}
void pageDown(){
  if (firstIdx + visibleRows < taskCount){
    uint8_t remaining = taskCount - (firstIdx + visibleRows);
    uint8_t step = visibleRows;
    if (step > remaining) step = remaining; // FIX: replace min()
    firstIdx += step;
  }
  drawList();
}

// -------- Notion fetch --------
bool notionFetch() {
  taskCount = 0;

  WiFiClientSecure client;
  if (USE_INSECURE_TLS) client.setInsecure();

  HTTPClient http;
  String url = String("https://api.notion.com/v1/databases/") + DATABASE_ID + "/query";

  String nextCursor = "";
  int  safety = 0;

  while (taskCount < MAX_TASKS && safety++ < 5) {
    if (!http.begin(client, url)) { drawHeader("HTTP begin() failed"); return false; }
    http.addHeader("Authorization", String("Bearer ") + NOTION_SECRET);
    http.addHeader("Notion-Version", NOTION_VERSION);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<384> body;
    body["page_size"] = 25;
    if (nextCursor.length()) body["start_cursor"] = nextCursor;
    String payload; serializeJson(body, payload);

    int code = http.POST(payload);
    if (code <= 0) { drawHeader(http.errorToString(code).c_str()); http.end(); return false; }

    String resp = http.getString();
    http.end();

    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, resp);
    if (err) { drawHeader("JSON parse error"); return false; }
        if (doc["object"] == "error") {
      const char* msg = doc["message"].as<const char*>();
      drawHeader(msg ? msg : "Notion error");
      return false;
    }

    JsonArray results = doc["results"].as<JsonArray>();
    for (JsonObject page : results) {
      if (taskCount >= MAX_TASKS) break;

      String title = "(untitled)";
      JsonArray titleArr = page["properties"]["Task"]["title"];
      if (!titleArr.isNull() && titleArr.size() > 0) {
        String t = titleArr[0]["plain_text"].as<String>();
        if (t.length()) title = t;
      }

      String status = "";
      JsonObject st = page["properties"]["Status"]["status"];
      if (!st.isNull() && !st["name"].isNull()) status = st["name"].as<String>();

      String s = status; s.toLowerCase();
      bool done = (s.indexOf("done") >= 0) || (s.indexOf("complete") >= 0) || (s.indexOf("finished") >= 0);

      tasks[taskCount++] = { title, status, done };
    }

    bool hasMore = doc["has_more"] | false;
    if (!hasMore) break;
    nextCursor = String((const char*)(doc["next_cursor"] | ""));
    if (nextCursor.length() == 0) break;
  }

  for (int i = 0, j = taskCount - 1; i < j; ++i, --j) {
    TaskItem tmp = tasks[i];
    tasks[i] = tasks[j];
    tasks[j] = tmp;
  }
  return true;
}

// -------- Touch → UI --------
void handleTouchUI(){
  static bool wasDown=false; static uint32_t lastTap=0;
  int16_t sx,sy; bool down=applyAffine(sx,sy);
  if(down && !wasDown){
    uint32_t now=millis();
    if(now-lastTap>140){
      if(inBtnUp(sx,sy))      { pageUp(); }
      else if(inBtnRf(sx,sy)) { drawHeader("Refreshing..."); if (WiFi.status()==WL_CONNECTED && notionFetch()) { firstIdx=0; drawAll("Notion OK"); } else { drawHeader("Refresh failed"); drawButtons(); } }
      else if(inBtnDn(sx,sy)) { pageDown(); }
      else {
        uint8_t gi = idxFromPoint(sx,sy);
        if (gi!=255){
          drawRow(gi,true);
          if(inCheckbox(gi,sx,sy)){ tasks[gi].done = !tasks[gi].done; drawRow(gi,false); }
          // else { tasks[gi].done = !tasks[gi].done; drawRow(gi,false); } // row-tap toggle
        }
      }
      lastTap=now;
    }
  }
  wasDown=down;
}

// -------- Setup / Loop --------
void setup(){
  Serial.begin(115200);

  tsSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(tsSPI); ts.setRotation(0);

  tft.init(); tft.setRotation(0);
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE,TFT_BLACK);

  visibleRows = (SCREEN_H - LIST_Y - BTN_BAR_H) / ROW_H;
  if (visibleRows < 1) visibleRows = 1;

  bool forceCal=false; uint32_t t0=millis();
  while (millis()-t0<700){ if(ts.tirqTouched() && ts.touched()){ forceCal=true; break; } delay(10); }
  cal.valid=false;
  if (!forceCal && loadCalibration()) { /* ok */ }
  else { if (runCalibrationLS()) { cal.valid=true; prefs.begin("touch",false); prefs.putBytes("cal",&cal,sizeof(Cal)); prefs.end(); } }

  drawHeader("Connecting Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PSK);
  uint32_t w0=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-w0<15000) { delay(250); tft.drawString(".", 180, 36, 1); }
  if (WiFi.status()!=WL_CONNECTED) { drawAll("Wi-Fi failed"); return; }

  drawHeader("Querying Notion...");
  if (notionFetch()) { firstIdx = 0; drawAll("Notion OK"); }
  else { drawAll("Notion error"); }
}

void loop(){ handleTouchUI(); delay(6); }


