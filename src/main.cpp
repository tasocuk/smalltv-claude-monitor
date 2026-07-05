// ============================================================================
//  GeekMagic SmallTV-Ultra  —  Ozel Firmware (v4)  +  Web Kontrol Paneli
//  - Duyarli (responsive) yerlesim + ayarlanabilir DEADZONE (guvenli alan)
//  - Ekran 0: PC (CPU/RAM halka) | Ekran 1: Claude ("Kullanim" kart)
//  - Cihazustu config paneli:  http://<ip>/   (deadzone, ekranlar, metinler, parlaklik)
//  - Config LittleFS /config.json'da saklanir, canli uygulanir.
//  - Turkce girisler suan ASCII'ye cevrilir (asciiTr) — gercek TR font Asama 2.
// ============================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "secrets.h"
#include "trfont.h"    // Turkce GFX fontlar: trFontS (15px), trFontL (22px)

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
#ifndef BL_ACTIVE_LOW
#define BL_ACTIVE_LOW 1
#endif
#define PIN_BL 5

static const char*    AP_SSID  = "SmallTV-Custom";
static const char*    HOSTNAME = "smalltv";
static const char*    NTP_TZ   = "GMT-3";
static const uint32_t PC_TIMEOUT_MS     = 8000;
static const uint32_t CLAUDE_TIMEOUT_MS = 300000;

TFT_eSPI tft = TFT_eSPI();
#define COL_BG      TFT_BLACK
#define COL_HEADER  0x2124
#define COL_TEXT    TFT_WHITE
#define COL_DIM     0x7BEF
#define COL_RINGBG  0x2965
#define COL_CARD    0x2124
#define COL_TRACK   0x49EB
#define COL_CORAL   0xFB49
#define COL_LIME    0xC729
#define COL_PILL    0x39C9
#define COL_ACCENT  0xFC4B
#define COL_GRAY    0xC618

ESP8266WebServer        server(80);
ESP8266HTTPUpdateServer httpUpdater;

struct PcStats { float cpu=0,ram=0,cpu_t=-1,cpu_ghz=-1,ram_used=-1,ram_total=-1; char host[24]=""; uint32_t lastUpdate=0; } st;
struct ClaudeStats { int h5=-1,d7=-1; long h5_reset=0,d7_reset=0; uint32_t lastUpdate=0; } cl;

// -------- Config (LittleFS /config.json) --------
struct Config {
  int  sTop=4, sBot=16, sLeft=6, sRight=6;     // deadzone kenar bosluklari (px)
  bool pcOn=true, clOn=true; int rotSec=10;    // ekranlar + rotasyon (0=manuel)
  int  brightness=255;
  bool claudeUsed=true;                        // true=kullanilan, false=kalan
  String title="Kullanim", pill5="Anlik", pill7="Haftalik", resetSuffix="sonra sifirlanir";
  String f0="Dusunuyor", f1="Demleniyor", f2="Mirildaniyor", f3="Kivaminda";
  int claudeTheme=1;                           // 0=Klasik, 1=Parlak (animasyonlu)
} cfg;

bool   apMode=false;
String wifiSsid, wifiPass;
int    screen=0; uint32_t lastSwitch=0; bool needStatic=true;
int    prevA=-1, prevB=-1; bool prevOnline=false; String prevHost="";
int    footIdx=0;

// -------- hesaplanan yerlesim (deadzone'a gore) --------
int LUX,LUY,LUW,LUH, L_headCY, L_cardH,L_CY1,L_CY2, L_footCY;
int L_ringR,L_ringCY,L_cpuX,L_ramX,L_labelY,L_infoY;
void computeLayout(){
  LUX=cfg.sLeft; LUY=cfg.sTop; LUW=240-cfg.sLeft-cfg.sRight; LUH=240-cfg.sTop-cfg.sBot;
  int headerH=26, footH=14, gap=6;
  L_headCY=LUY+headerH/2;
  int cardsH=LUH-headerH-footH; L_cardH=(cardsH-gap)/2;
  L_CY1=LUY+headerH; L_CY2=L_CY1+L_cardH+gap; L_footCY=LUY+LUH-footH/2;
  // PC halka yerlesimi
  L_cpuX=LUX+LUW/4; L_ramX=LUX+LUW*3/4;
  int availH=LUH-headerH-34;
  L_ringR=min(LUW/4-8, availH/2); if(L_ringR>54)L_ringR=54; if(L_ringR<20)L_ringR=20;
  L_ringCY=LUY+headerH+L_ringR+4;
  L_labelY=L_ringCY+L_ringR+11; L_infoY=L_labelY+18;
}

// -------- Turkce -> ASCII (font gelene kadar) --------
String asciiTr(const String& s){
  String o; o.reserve(s.length());
  for(size_t i=0;i<s.length();){
    uint8_t c=s[i];
    if(c<0x80){ o+=(char)c; i++; continue; }
    uint8_t c2=(i+1<s.length())?(uint8_t)s[i+1]:0; uint16_t cp=0;
    if((c&0xE0)==0xC0){ cp=((c&0x1F)<<6)|(c2&0x3F); i+=2; } else { i++; continue; }
    char b='?';                       // UTF-8 -> Latin-5 (ISO-8859-9) tek bayt
    switch(cp){
      case 0x131:b=(char)0xFD;break; case 0x130:b=(char)0xDD;break; // i I (noktasiz/noktali)
      case 0x15F:b=(char)0xFE;break; case 0x15E:b=(char)0xDE;break; // s S
      case 0x11F:b=(char)0xF0;break; case 0x11E:b=(char)0xD0;break; // g G
      default: if(cp>=0xA0&&cp<=0xFF)b=(char)cp; else b='?'; break; // u/o/c ve digerleri Latin-1 ile ayni
    }
    o+=b;
  } return o;
}

// -------- yardimcilar --------
uint16_t loadColor(float p){ if(p<60)return TFT_GREEN; if(p<85)return TFT_ORANGE; return TFT_RED; }
bool pcOnline(){ return st.lastUpdate && (millis()-st.lastUpdate)<PC_TIMEOUT_MS; }
bool clOnline(){ return cl.lastUpdate && (millis()-cl.lastUpdate)<CLAUDE_TIMEOUT_MS; }
void setBrightness(int b){ if(b<0)b=0; if(b>255)b=255; analogWrite(PIN_BL, BL_ACTIVE_LOW? 255-b : b); }
void fmtReset(long ep,char*buf,size_t n){
  time_t now=time(nullptr);
  if(ep<=0||now<1600000000){ strncpy(buf,"-",n); return; }
  long s=ep-(long)now; if(s<0)s=0; int d=s/86400,h=(s%86400)/3600,m=(s%3600)/60;
  if(d>0)snprintf(buf,n,"%dg %dsa",d,h); else if(h>0)snprintf(buf,n,"%dsa %ddk",h,m); else snprintf(buf,n,"%ddk",m);
}

// -------- Config yukle/kaydet --------
void saveConfig(){
  JsonDocument d;
  d["sTop"]=cfg.sTop; d["sBot"]=cfg.sBot; d["sLeft"]=cfg.sLeft; d["sRight"]=cfg.sRight;
  d["pcOn"]=cfg.pcOn; d["clOn"]=cfg.clOn; d["rotSec"]=cfg.rotSec; d["bright"]=cfg.brightness; d["cUsed"]=cfg.claudeUsed;
  d["title"]=cfg.title; d["pill5"]=cfg.pill5; d["pill7"]=cfg.pill7; d["rsuf"]=cfg.resetSuffix;
  d["f0"]=cfg.f0; d["f1"]=cfg.f1; d["f2"]=cfg.f2; d["f3"]=cfg.f3; d["theme"]=cfg.claudeTheme;
  File f=LittleFS.open("/config.json","w"); if(!f)return; serializeJson(d,f); f.close();
}
void loadConfig(){
  if(!LittleFS.exists("/config.json"))return;
  File f=LittleFS.open("/config.json","r"); if(!f)return; JsonDocument d;
  if(deserializeJson(d,f)){ f.close(); return; }
  cfg.sTop=d["sTop"]|cfg.sTop; cfg.sBot=d["sBot"]|cfg.sBot; cfg.sLeft=d["sLeft"]|cfg.sLeft; cfg.sRight=d["sRight"]|cfg.sRight;
  cfg.pcOn=d["pcOn"]|cfg.pcOn; cfg.clOn=d["clOn"]|cfg.clOn; cfg.rotSec=d["rotSec"]|cfg.rotSec;
  cfg.brightness=d["bright"]|cfg.brightness; cfg.claudeUsed=d["cUsed"]|cfg.claudeUsed;
  cfg.title=(const char*)(d["title"]|cfg.title.c_str()); cfg.pill5=(const char*)(d["pill5"]|cfg.pill5.c_str());
  cfg.pill7=(const char*)(d["pill7"]|cfg.pill7.c_str()); cfg.resetSuffix=(const char*)(d["rsuf"]|cfg.resetSuffix.c_str());
  cfg.f0=(const char*)(d["f0"]|cfg.f0.c_str()); cfg.f1=(const char*)(d["f1"]|cfg.f1.c_str());
  cfg.f2=(const char*)(d["f2"]|cfg.f2.c_str()); cfg.f3=(const char*)(d["f3"]|cfg.f3.c_str());
  cfg.claudeTheme=d["theme"]|cfg.claudeTheme;
  f.close();
}

// -------- ciz: primitifler --------
void drawGauge(int cx,int cy,int r,int pct,uint16_t col){
  int ir=r*77/100; if(pct<0)pct=0; if(pct>100)pct=100;
  tft.drawSmoothArc(cx,cy,r,ir,0,360,COL_RINGBG,COL_BG,true);
  int end=(int)(360.0f*pct/100.0f); if(pct>0&&end<2)end=2;
  if(end>=2)tft.drawSmoothArc(cx,cy,r,ir,0,end,col,COL_BG,true);
  tft.fillCircle(cx,cy,ir-2,COL_BG);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_TEXT,COL_BG);
  if(pct>=100){ tft.setTextFont(4); tft.drawString("100",cx,cy); } else { tft.setTextFont(6); tft.drawString(String(pct),cx,cy); }
}
void drawMascot(int x,int y){
  static const uint8_t M[6]={0b00100100,0b01111110,0b11011011,0b11111111,0b01100110,0b01000010};
  for(int r=0;r<6;r++)for(int c=0;c<8;c++) if(M[r]&(0x80>>c)) tft.fillRect(x+c*3,y+r*3,3,3,COL_CORAL);
}
void drawWifiIcon(int cx,int cy,bool online){
  uint16_t col=online?COL_TEXT:0x528A; tft.fillRect(cx-18,cy-18,36,26,COL_BG);
  for(int r=6;r<=16;r+=5)tft.drawSmoothArc(cx,cy,r,r-2,135,225,col,COL_BG,true);
  tft.fillCircle(cx,cy,2,col);
}
void drawBar(int x,int y,int w,int h,int pct,uint16_t fill){
  if(pct<0)pct=0; if(pct>100)pct=100; tft.fillRoundRect(x,y,w,h,h/2,COL_TRACK);
  int fw=w*pct/100; if(fw>=h)tft.fillRoundRect(x,y,fw,h,h/2,fill); else if(fw>0)tft.fillRoundRect(x,y,h,h,h/2,fill);
}
void drawPillR(int rightX,int y,int h,const String& txt){
  tft.setFreeFont(&trFontS); int w=tft.textWidth(txt)+18; int x=rightX-w;
  tft.fillRoundRect(x,y,w,h,h/2,COL_PILL);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_TEXT,COL_PILL); tft.drawString(txt,x+w/2,y+h/2);
}

// -------- PC ekrani --------
void drawStaticPc(){
  tft.fillScreen(COL_BG);
  tft.setTextDatum(ML_DATUM); tft.setTextColor(COL_TEXT,COL_BG); tft.setTextFont(2);
  tft.drawString(strlen(st.host)?st.host:"SmallTV",LUX+18,L_headCY);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_DIM,COL_BG);
  tft.drawString("CPU",L_cpuX,L_labelY); tft.drawString("RAM",L_ramX,L_labelY);
  prevA=prevB=-1; prevHost=String(st.host);
}
void drawClockDot(bool online){
  tft.fillCircle(LUX+8,L_headCY,5,online?TFT_GREEN:TFT_RED);
  time_t now=time(nullptr); char hhmm[6];
  if(now>1600000000){ struct tm*t=localtime(&now); strftime(hhmm,sizeof hhmm,"%H:%M",t);} else strcpy(hhmm,"--:--");
  tft.fillRect(LUX+LUW-52,L_headCY-8,52,16,COL_BG);
  tft.setTextDatum(MR_DATUM); tft.setTextFont(2); tft.setTextColor(COL_TEXT,COL_BG); tft.drawString(hhmm,LUX+LUW,L_headCY);
}
void drawInfoPc(){
  char b[24]; tft.setTextDatum(MC_DATUM); tft.setTextFont(2); tft.setTextColor(COL_TEXT,COL_BG);
  tft.fillRect(LUX,L_infoY-9,LUW/2,18,COL_BG); tft.fillRect(LUX+LUW/2,L_infoY-9,LUW/2,18,COL_BG);
  if(st.cpu_t>=0){snprintf(b,sizeof b,"%d C",(int)(st.cpu_t+0.5f)); tft.drawString(b,L_cpuX,L_infoY);}
  else if(st.cpu_ghz>=0){snprintf(b,sizeof b,"%.1f GHz",st.cpu_ghz); tft.drawString(b,L_cpuX,L_infoY);}
  if(st.ram_total>0){snprintf(b,sizeof b,"%.1f/%.0fG",st.ram_used,st.ram_total); tft.drawString(b,L_ramX,L_infoY);}
}
void renderPc(bool online){
  int a=online?(int)(st.cpu+0.5f):0,b=online?(int)(st.ram+0.5f):0;
  uint16_t ca=online?loadColor(st.cpu):COL_RINGBG, cb=online?loadColor(st.ram):COL_RINGBG;
  if(prevHost!=String(st.host)){ tft.fillRect(LUX+18,L_headCY-8,LUW-90,16,COL_BG); tft.setTextDatum(ML_DATUM); tft.setTextColor(COL_TEXT,COL_BG); tft.setTextFont(2); tft.drawString(strlen(st.host)?st.host:"SmallTV",LUX+18,L_headCY); prevHost=String(st.host);}
  if(a!=prevA||online!=prevOnline){ drawGauge(L_cpuX,L_ringCY,L_ringR,a,ca); prevA=a; }
  if(b!=prevB||online!=prevOnline){ drawGauge(L_ramX,L_ringCY,L_ringR,b,cb); prevB=b; }
  drawInfoPc(); drawClockDot(online);
}

// -------- Claude ekrani --------
void drawClaudeValue(int CY,int val,uint16_t barcol){
  tft.fillRect(LUX+10,CY+6,LUW*55/100,52,COL_CARD);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(COL_TEXT,COL_CARD);
  tft.setTextFont(6); String s=String(val); tft.drawString(s,LUX+14,CY+8);
  int nw=tft.textWidth(s,6); tft.setTextFont(4); tft.drawString("%",LUX+14+nw+2,CY+30);
  drawBar(LUX+14,CY+L_cardH-30,LUW-28,11,val,barcol);
}
void drawClaudeReset(int CY,long ep){
  char t[16]; fmtReset(ep,t,sizeof t);
  tft.fillRect(LUX+12,CY+L_cardH-19,LUW-24,18,COL_CARD);
  tft.setTextDatum(ML_DATUM); tft.setFreeFont(&trFontS); tft.setTextColor(COL_GRAY,COL_CARD);
  tft.drawString(String(t)+" "+asciiTr(cfg.resetSuffix),LUX+14,CY+L_cardH-10);
}
void drawClaudeStaticClassic(){
  tft.fillScreen(COL_BG);
  drawMascot(LUX+2,L_headCY-9);
  tft.setTextDatum(MC_DATUM); tft.setFreeFont(&trFontL); tft.setTextColor(COL_TEXT,COL_BG);
  tft.drawString(asciiTr(cfg.title),LUX+LUW/2,L_headCY);
  drawWifiIcon(LUX+LUW-12,L_headCY+2,clOnline());
  tft.fillRoundRect(LUX,L_CY1,LUW,L_cardH,12,COL_CARD);
  tft.fillRoundRect(LUX,L_CY2,LUW,L_cardH,12,COL_CARD);
  drawPillR(LUX+LUW-8,L_CY1+12,24,asciiTr(cfg.pill5));
  drawPillR(LUX+LUW-8,L_CY2+12,24,asciiTr(cfg.pill7));
  const String words[4]={cfg.f0,cfg.f1,cfg.f2,cfg.f3}; footIdx=(footIdx+1)&3;
  tft.fillRect(LUX,L_footCY-9,LUW,18,COL_BG);
  tft.setTextDatum(MC_DATUM); tft.setFreeFont(&trFontS); tft.setTextColor(COL_ACCENT,COL_BG);
  tft.drawString(String("* ")+asciiTr(words[footIdx]),LUX+LUW/2,L_footCY);
  prevA=prevB=-1;
}
void renderClaudeClassic(bool online){
  int r5=(cl.h5>=0)?(cfg.claudeUsed?cl.h5:100-cl.h5):0;
  int r7=(cl.d7>=0)?(cfg.claudeUsed?cl.d7:100-cl.d7):0;
  int u5=online?r5:0, u7=online?r7:0;
  if(u5!=prevA){ drawClaudeValue(L_CY1,u5,COL_CORAL); prevA=u5; }
  if(u7!=prevB){ drawClaudeValue(L_CY2,u7,COL_LIME); prevB=u7; }
  drawClaudeReset(L_CY1,cl.h5_reset); drawClaudeReset(L_CY2,cl.d7_reset);
}

// ================= PARLAK (glow) TEMA =================
#define COL_CT1  0x28A2
#define COL_CT2  0x0924
#define COL_TEAL 0x2E73
#define COL_TEALL 0x6FDB
#define COL_CORALL 0xFDAD
#define COL_TRK2 0x2945
uint16_t dim565(uint16_t c,int f){ if(f>255)f=255; if(f<0)f=0; int r=(c>>11)&31,g=(c>>5)&63,b=c&31; return (uint16_t)(((r*f/255)<<11)|((g*f/255)<<5)|(b*f/255)); }
int animTri(){ int p=(millis()/6)%512; return p<256?p:512-p; }
void glowBorder(int y,uint16_t accent,int inten){
  tft.drawRoundRect(LUX,y,LUW,L_cardH,13,dim565(accent,40+inten/3));
  tft.drawRoundRect(LUX+1,y+1,LUW-2,L_cardH-2,12,dim565(accent,90+inten*2/3));
}
void drawPillOutline(int rightX,int y,int h,const String& txt,uint16_t accent,uint16_t bg){
  tft.setFreeFont(&trFontS); int w=tft.textWidth(txt)+20; int x=rightX-w;
  tft.fillRoundRect(x,y,w,h,h/2,bg); tft.drawRoundRect(x,y,w,h,h/2,accent);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(accent,bg); tft.drawString(txt,x+w/2,y+h/2);
}
void drawWifiFilled(int cx,int cy,bool online){
  uint16_t col=online?COL_TEXT:0x528A; tft.fillRect(cx-17,cy-16,36,24,COL_BG);
  for(int r=5;r<=15;r+=5) tft.drawSmoothArc(cx,cy,r+1,r-1,140,220,col,COL_BG,true);
  tft.fillCircle(cx,cy,3,col);
}
void drawGlowValue(int CY,int val,uint16_t tint,uint16_t barA,uint16_t barL){
  tft.fillRect(LUX+12,CY+8,LUW*55/100,54,tint);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(COL_TEXT,tint);
  tft.setTextFont(6); String s=String(val); tft.drawString(s,LUX+16,CY+10);
  int nw=tft.textWidth(s,6); tft.setTextFont(4); tft.drawString("%",LUX+16+nw+2,CY+34);
  int x=LUX+16,y=CY+L_cardH-28,w=LUW-32,h=12;
  tft.fillRoundRect(x,y,w,h,h/2,COL_TRK2);
  int fw=w*val/100; if(val>0&&fw<h)fw=h;
  if(fw>0){ tft.fillRoundRect(x,y,fw,h,h/2,barA); if(fw>h) tft.fillRoundRect(x,y,fw*2/5,h,h/2,barL); }
}
void drawGlowReset(int CY,long ep,uint16_t tint){
  char t[16]; fmtReset(ep,t,sizeof t);
  tft.fillRect(LUX+14,CY+L_cardH-19,LUW-28,18,tint);
  tft.setTextDatum(ML_DATUM); tft.setFreeFont(&trFontS); tft.setTextColor(0xB596,tint);
  tft.drawString(String(t)+" "+asciiTr(cfg.resetSuffix),LUX+16,CY+L_cardH-10);
}
void drawClaudeStaticGlow(){
  tft.fillScreen(COL_BG);
  tft.fillRoundRect(LUX,L_CY1,LUW,L_cardH,13,COL_CT1);
  tft.fillRoundRect(LUX,L_CY2,LUW,L_cardH,13,COL_CT2);
  glowBorder(L_CY1,COL_CORAL,animTri()); glowBorder(L_CY2,COL_TEAL,animTri());
  drawPillOutline(LUX+LUW-8,L_CY1+11,26,asciiTr(cfg.pill5),COL_CORALL,COL_CT1);
  drawPillOutline(LUX+LUW-8,L_CY2+11,26,asciiTr(cfg.pill7),COL_TEALL,COL_CT2);
  drawMascot(LUX+2,L_headCY-9);
  tft.setTextDatum(MC_DATUM); tft.setFreeFont(&trFontL); tft.setTextColor(COL_TEXT,COL_BG);
  tft.drawString(asciiTr(cfg.title),LUX+LUW/2,L_headCY);
  drawWifiFilled(LUX+LUW-12,L_headCY+2,clOnline());
  const String words[4]={cfg.f0,cfg.f1,cfg.f2,cfg.f3}; footIdx=(footIdx+1)&3;
  tft.fillRect(LUX,L_footCY-9,LUW,18,COL_BG);
  tft.setTextDatum(MC_DATUM); tft.setFreeFont(&trFontS); tft.setTextColor(COL_GRAY,COL_BG);
  tft.drawString(String("* ")+asciiTr(words[footIdx]),LUX+LUW/2,L_footCY);
  prevA=prevB=-1;
}
void renderClaudeGlow(bool online){
  int r5=(cl.h5>=0)?(cfg.claudeUsed?cl.h5:100-cl.h5):0;
  int r7=(cl.d7>=0)?(cfg.claudeUsed?cl.d7:100-cl.d7):0;
  int u5=online?r5:0,u7=online?r7:0;
  if(u5!=prevA){ drawGlowValue(L_CY1,u5,COL_CT1,COL_CORAL,COL_CORALL); prevA=u5; }
  if(u7!=prevB){ drawGlowValue(L_CY2,u7,COL_CT2,COL_TEAL,COL_TEALL); prevB=u7; }
  drawGlowReset(L_CY1,cl.h5_reset,COL_CT1); drawGlowReset(L_CY2,cl.d7_reset,COL_CT2);
}
void animGlow(){
  int t=animTri();
  glowBorder(L_CY1,COL_CORAL,t); glowBorder(L_CY2,COL_TEAL,t);
  static int lastBy=999; int by=(t-128)/48;
  if(by!=lastBy){ tft.fillRect(LUX+2,L_headCY-12,26,22,COL_BG); drawMascot(LUX+2,L_headCY-9+by); lastBy=by; }
}
// dagitici (tema secimine gore)
void drawClaudeStatic(){ if(cfg.claudeTheme==1) drawClaudeStaticGlow(); else drawClaudeStaticClassic(); }
void renderClaude(bool online){ if(cfg.claudeTheme==1) renderClaudeGlow(online); else renderClaudeClassic(online); }

// -------- ekran secimi / tick --------
int enabledCount(){ return (cfg.pcOn?1:0)+(cfg.clOn?1:0); }
void ensureValidScreen(){ if(screen==0&&!cfg.pcOn)screen=1; if(screen==1&&!cfg.clOn)screen=0; if(!cfg.pcOn&&!cfg.clOn)screen=0; }
void forceRedraw(){ needStatic=true; }
void tick(){
  ensureValidScreen();
  if(cfg.rotSec>0 && enabledCount()==2 && millis()-lastSwitch>=(uint32_t)cfg.rotSec*1000){ screen^=1; lastSwitch=millis(); needStatic=true; }
  if(needStatic){ if(screen==0)drawStaticPc(); else drawClaudeStatic(); needStatic=false; prevOnline=!((screen==0)?pcOnline():clOnline()); }
  bool online=(screen==0)?pcOnline():clOnline();
  if(screen==0)renderPc(online); else renderClaude(online);
  if(online!=prevOnline){ if(screen==0){} else drawWifiIcon(LUX+LUW-12,L_headCY+2,online); prevOnline=online; }
}

// ============================ WEB PANEL ============================
const char CFG_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'><title>SmallTV Panel</title>
<style>
body{font-family:system-ui,sans-serif;background:#0f0f12;color:#eee;margin:0;padding:14px;max-width:520px;margin:auto}
h1{font-size:19px;margin:6px 0 14px}h2{font-size:14px;color:#ff8a4b;margin:18px 0 8px;border-bottom:1px solid #2a2a30;padding-bottom:4px}
.row{display:flex;align-items:center;gap:10px;margin:7px 0}.row label{flex:1;font-size:14px}
input[type=text]{width:100%;padding:8px;border-radius:6px;border:1px solid #33333a;background:#1a1a1f;color:#fff;box-sizing:border-box}
input[type=range]{flex:2}input[type=number]{width:64px;padding:6px;border-radius:6px;border:1px solid #33333a;background:#1a1a1f;color:#fff}
.v{width:34px;text-align:right;color:#ff8a4b;font-variant-numeric:tabular-nums}
button{padding:11px 16px;border:0;border-radius:8px;background:#2b7a4b;color:#fff;font-size:15px;width:100%;margin-top:16px}
.sec{background:#16161a;border-radius:10px;padding:10px 12px;margin:10px 0}
canvas{background:#000;border-radius:8px;display:block;margin:6px auto;border:1px solid #333}
.mini{display:flex;gap:8px}.mini button{width:auto;margin:0;background:#33333a;padding:8px 12px;font-size:13px}
small{color:#888}
</style></head><body>
<h1>📺 SmallTV Kontrol Paneli</h1>
<div class=sec><h2 style=margin-top:0>Onizleme + Guvenli Alan (Deadzone)</h2>
<canvas id=pv width=240 height=240></canvas>
<div class=row><label>Ust</label><input type=range id=sTop min=0 max=30><span class=v id=sTopv></span></div>
<div class=row><label>Alt</label><input type=range id=sBot min=0 max=30><span class=v id=sBotv></span></div>
<div class=row><label>Sol</label><input type=range id=sLeft min=0 max=30><span class=v id=sLeftv></span></div>
<div class=row><label>Sag</label><input type=range id=sRight min=0 max=30><span class=v id=sRightv></span></div>
<small>Kenarda kesilen olursa ilgili boslugu artir.</small></div>

<div class=sec><h2 style=margin-top:0>Ekranlar</h2>
<div class=row><label>PC ekrani (CPU/RAM)</label><input type=checkbox id=pcOn></div>
<div class=row><label>Claude ekrani</label><input type=checkbox id=clOn></div>
<div class=row><label>Rotasyon (sn, 0=kapali)</label><input type=number id=rotSec min=0 max=120></div>
<div class=mini><button onclick=switchNow()>Simdi gecis yap</button></div></div>

<div class=sec><h2 style=margin-top:0>Gorunum</h2>
<div class=row><label>Parlaklik</label><input type=range id=bright min=5 max=255><span class=v id=brightv></span></div>
<div class=row><label>Claude modu</label><select id=cUsed style=padding:7px;border-radius:6px;background:#1a1a1f;color:#fff;border:1px solid #33333a><option value=1>Kullanilan %</option><option value=0>Kalan %</option></select></div>
<div class=row><label>Claude temasi</label><select id=theme style=padding:7px;border-radius:6px;background:#1a1a1f;color:#fff;border:1px solid #33333a><option value=1>Parlak (animasyonlu)</option><option value=0>Klasik</option></select></div></div>

<div class=sec><h2 style=margin-top:0>Metinler</h2>
<div class=row><label>Baslik</label></div><input type=text id=title>
<div class=row><label>5 saat rozeti</label></div><input type=text id=pill5>
<div class=row><label>7 gun rozeti</label></div><input type=text id=pill7>
<div class=row><label>Reset eki</label></div><input type=text id=rsuf>
<div class=row><label>Footer kelimeleri</label></div>
<input type=text id=f0><input type=text id=f1 style=margin-top:6px><input type=text id=f2 style=margin-top:6px><input type=text id=f3 style=margin-top:6px></div>

<button onclick=save()>KAYDET & UYGULA</button>
<p style=text-align:center><a href='/update' style=color:#7af>Firmware</a> &nbsp; <a href='/wifi' style=color:#7af>WiFi</a></p>
<script>
var F=['sTop','sBot','sLeft','sRight','pcOn','clOn','rotSec','bright','cUsed','title','pill5','pill7','rsuf','f0','f1','f2','f3'];
function g(i){return document.getElementById(i)}
function draw(){var c=g('pv'),x=c.getContext('2d');x.clearRect(0,0,240,240);
 var t=+g('sTop').value,b=+g('sBot').value,l=+g('sLeft').value,r=+g('sRight').value;
 var ux=l,uy=t,uw=240-l-r,uh=240-t-b;
 x.strokeStyle='#444';x.strokeRect(0.5,0.5,239,239);
 x.strokeStyle='#ff8a4b';x.setLineDash([4,3]);x.strokeRect(ux,uy,uw,uh);x.setLineDash([]);
 var hH=26,fH=14,gap=6,cH=(uh-hH-fH-gap)/2;
 x.fillStyle='#c0392b';x.fillRect(ux+2,uy+8,18,12);
 x.fillStyle='#fff';x.font='13px sans-serif';x.textAlign='center';x.fillText(g('title').value||'Kullanim',ux+uw/2,uy+17);
 function card(cy,col){x.fillStyle='#202024';x.fillRect(ux,cy,uw,cH);x.fillStyle='#fff';x.font='bold 22px sans-serif';x.textAlign='left';x.fillText('18%',ux+10,cy+cH*0.5);x.fillStyle=col;x.fillRect(ux+10,cy+cH-16,uw*0.2,7)}
 card(uy+hH,'#ff6b4a');card(uy+hH+cH+gap,'#c4e64a');
 x.fillStyle='#ff8a4b';x.font='11px sans-serif';x.textAlign='center';x.fillText('* '+(g('f0').value||'...'),ux+uw/2,uy+uh-3);
}
['sTop','sBot','sLeft','sRight'].forEach(i=>{g(i).oninput=()=>{g(i+'v').textContent=g(i).value;draw()}});
g('bright').oninput=()=>g('brightv').textContent=g('bright').value;
['title','f0'].forEach(i=>g(i).oninput=draw);
function load(){fetch('/config.json').then(r=>r.json()).then(c=>{
 for(var k in c){var e=g(k);if(!e)continue;if(e.type=='checkbox')e.checked=!!c[k];else e.value=c[k];}
 g('sTopv').textContent=c.sTop;g('sBotv').textContent=c.sBot;g('sLeftv').textContent=c.sLeft;g('sRightv').textContent=c.sRight;g('brightv').textContent=c.bright;
 g('cUsed').value=c.cUsed?1:0;g('theme').value=c.theme;draw();});}
function save(){var o={};
 o.sTop=+g('sTop').value;o.sBot=+g('sBot').value;o.sLeft=+g('sLeft').value;o.sRight=+g('sRight').value;
 o.pcOn=g('pcOn').checked;o.clOn=g('clOn').checked;o.rotSec=+g('rotSec').value;o.bright=+g('bright').value;o.cUsed=+g('cUsed').value;o.theme=+g('theme').value;
 o.title=g('title').value;o.pill5=g('pill5').value;o.pill7=g('pill7').value;o.rsuf=g('rsuf').value;
 o.f0=g('f0').value;o.f1=g('f1').value;o.f2=g('f2').value;o.f3=g('f3').value;
 fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.text()).then(t=>alert(t));}
function switchNow(){fetch('/switch',{method:'POST'}).then(r=>r.text()).then(t=>{});}
load();
</script></body></html>)HTML";

void handleCfgPage(){ server.send_P(200,"text/html",CFG_HTML); }
void handleCfgJson(){
  JsonDocument d;
  d["sTop"]=cfg.sTop;d["sBot"]=cfg.sBot;d["sLeft"]=cfg.sLeft;d["sRight"]=cfg.sRight;
  d["pcOn"]=cfg.pcOn;d["clOn"]=cfg.clOn;d["rotSec"]=cfg.rotSec;d["bright"]=cfg.brightness;d["cUsed"]=cfg.claudeUsed;
  d["title"]=cfg.title;d["pill5"]=cfg.pill5;d["pill7"]=cfg.pill7;d["rsuf"]=cfg.resetSuffix;
  d["f0"]=cfg.f0;d["f1"]=cfg.f1;d["f2"]=cfg.f2;d["f3"]=cfg.f3;d["theme"]=cfg.claudeTheme;
  String o; serializeJson(d,o); server.send(200,"application/json",o);
}
void handleCfgPost(){
  JsonDocument d; if(deserializeJson(d,server.arg("plain"))){ server.send(400,"text/plain","JSON hatasi"); return; }
  cfg.sTop=constrain((int)(d["sTop"]|cfg.sTop),0,40); cfg.sBot=constrain((int)(d["sBot"]|cfg.sBot),0,40);
  cfg.sLeft=constrain((int)(d["sLeft"]|cfg.sLeft),0,40); cfg.sRight=constrain((int)(d["sRight"]|cfg.sRight),0,40);
  cfg.pcOn=d["pcOn"]|cfg.pcOn; cfg.clOn=d["clOn"]|cfg.clOn; cfg.rotSec=constrain((int)(d["rotSec"]|cfg.rotSec),0,600);
  cfg.brightness=constrain((int)(d["bright"]|cfg.brightness),5,255); cfg.claudeUsed=((int)(d["cUsed"]|(cfg.claudeUsed?1:0)))!=0;
  if(d["title"].is<const char*>())cfg.title=(const char*)d["title"];
  if(d["pill5"].is<const char*>())cfg.pill5=(const char*)d["pill5"];
  if(d["pill7"].is<const char*>())cfg.pill7=(const char*)d["pill7"];
  if(d["rsuf"].is<const char*>())cfg.resetSuffix=(const char*)d["rsuf"];
  if(d["f0"].is<const char*>())cfg.f0=(const char*)d["f0"];
  if(d["f1"].is<const char*>())cfg.f1=(const char*)d["f1"];
  if(d["f2"].is<const char*>())cfg.f2=(const char*)d["f2"];
  if(d["f3"].is<const char*>())cfg.f3=(const char*)d["f3"];
  cfg.claudeTheme=constrain((int)(d["theme"]|cfg.claudeTheme),0,1);
  saveConfig(); computeLayout(); setBrightness(cfg.brightness); ensureValidScreen(); forceRedraw();
  server.send(200,"text/plain","Kaydedildi, uygulandi!");
}
void handleSwitch(){ if(enabledCount()==2){ screen^=1; lastSwitch=millis(); needStatic=true; } server.send(200,"text/plain","ok"); }

// -------- diger HTTP --------
void handleStats(){
  if(server.method()!=HTTP_POST){server.send(405,"text/plain","POST only");return;}
  JsonDocument doc; if(deserializeJson(doc,server.arg("plain"))){server.send(400,"text/plain","bad json");return;}
  st.cpu=doc["cpu"]|st.cpu; st.ram=doc["ram"]|st.ram; st.cpu_t=doc["cpu_t"]|-1.0f; st.cpu_ghz=doc["cpu_ghz"]|-1.0f;
  st.ram_used=doc["ram_used"]|-1.0f; st.ram_total=doc["ram_total"]|-1.0f;
  const char* h=doc["host"]|""; if(h&&*h){strncpy(st.host,h,sizeof(st.host)-1);st.host[sizeof(st.host)-1]=0;}
  st.lastUpdate=millis(); server.send(200,"text/plain","OK");
}
void handleClaude(){
  if(server.method()!=HTTP_POST){server.send(405,"text/plain","POST only");return;}
  JsonDocument doc; if(deserializeJson(doc,server.arg("plain"))){server.send(400,"text/plain","bad json");return;}
  cl.h5=doc["h5"]|cl.h5; cl.d7=doc["d7"]|cl.d7; cl.h5_reset=doc["h5_reset"]|cl.h5_reset; cl.d7_reset=doc["d7_reset"]|cl.d7_reset;
  cl.lastUpdate=millis(); server.send(200,"text/plain","OK");
}
void handleHealth(){
  JsonDocument d; d["pc_online"]=pcOnline();d["cpu"]=st.cpu;d["ram"]=st.ram;d["host"]=st.host;
  d["cl_online"]=clOnline();d["h5"]=cl.h5;d["d7"]=cl.d7;d["ip"]=WiFi.localIP().toString();
  String o; serializeJson(d,o); server.send(200,"application/json",o);
}
void handleWifiGet(){
  String h="<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
   "<style>body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:24px auto;padding:0 16px}input{width:100%;padding:9px;margin:6px 0;box-sizing:border-box;border-radius:5px;border:1px solid #444;background:#1c1c1c;color:#fff}button{padding:10px 16px;border:0;border-radius:6px;background:#2b7a4b;color:#fff}</style>"
   "<h2>WiFi</h2><form method=POST action=/wifi>SSID<input name=ssid value='"+wifiSsid+"'>Sifre<input name=pass type=password placeholder='(bos=degistirme)'><button>Kaydet</button></form>";
  server.send(200,"text/html; charset=utf-8",h);
}
void handleWifiPost(){ String s=server.arg("ssid"),p=server.arg("pass"); if(s.length())wifiSsid=s; if(p.length())wifiPass=p;
  File f=LittleFS.open("/wifi.json","w"); if(f){JsonDocument d;d["ssid"]=wifiSsid;d["pass"]=wifiPass;serializeJson(d,f);f.close();}
  server.send(200,"text/html","Kaydedildi, yeniden baslatiliyor..."); delay(700); ESP.restart(); }

// -------- WiFi --------
void loadWifi(){ wifiSsid=DEFAULT_WIFI_SSID; wifiPass=DEFAULT_WIFI_PASS;
  if(!LittleFS.exists("/wifi.json"))return; File f=LittleFS.open("/wifi.json","r"); if(!f)return; JsonDocument d;
  if(!deserializeJson(d,f)){String s=(const char*)(d["ssid"]|""); if(s.length()){wifiSsid=s;wifiPass=(const char*)(d["pass"]|"");}} f.close(); }
bool connectWiFi(){ if(!wifiSsid.length())return false; WiFi.mode(WIFI_STA); WiFi.hostname(HOSTNAME); WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());
  uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED&&millis()-t0<20000){delay(250);yield();} return WiFi.status()==WL_CONNECTED; }
void startAP(){ apMode=true; WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID); }

void setup(){
  Serial.begin(115200);
  pinMode(PIN_BL,OUTPUT); analogWriteRange(255);
  LittleFS.begin(); loadConfig(); computeLayout(); setBrightness(cfg.brightness);
  tft.init(); tft.setRotation(0); tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM); tft.setTextFont(4); tft.setTextColor(COL_TEXT,COL_BG); tft.drawString("Baglaniyor...",120,120);
  loadWifi(); if(!connectWiFi()) startAP();
  configTime(NTP_TZ,"pool.ntp.org","time.google.com");
  MDNS.begin(HOSTNAME); httpUpdater.setup(&server);
  server.on("/",handleCfgPage); server.on("/config.json",handleCfgJson);
  server.on("/config",HTTP_POST,handleCfgPost); server.on("/switch",HTTP_POST,handleSwitch);
  server.on("/stats",handleStats); server.on("/claude",handleClaude); server.on("/health",handleHealth);
  server.on("/wifi",HTTP_GET,handleWifiGet); server.on("/wifi",HTTP_POST,handleWifiPost);
  server.begin(); MDNS.addService("http","tcp",80);
  ensureValidScreen(); lastSwitch=millis(); needStatic=true; tft.fillScreen(COL_BG); tick();
}
void loop(){
  server.handleClient(); MDNS.update();
  uint32_t now=millis(); static uint32_t last=0,lastAnim=0;
  if(now-last>=1000){ last=now; tick(); }
  if(screen==1 && cfg.clOn && cfg.claudeTheme==1 && !needStatic && now-lastAnim>=55){ lastAnim=now; animGlow(); }
}
