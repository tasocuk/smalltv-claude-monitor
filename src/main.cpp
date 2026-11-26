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
struct ClaudeStats { int h5=-1,d7=-1; long h5_reset=0,d7_reset=0; uint32_t lastUpdate=0; char err[32]=""; } cl;

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
// alt satir: hata varsa (ornek: "Claude girisi gerekli") onu, yoksa donen kelimeyi yazar
void drawFooter(uint16_t col){
  tft.fillRect(LUX,L_footCY-9,LUW,18,COL_BG);
  tft.setTextDatum(MC_DATUM); tft.setFreeFont(&trFontS);
  if(cl.err[0]){ tft.setTextColor(COL_CORAL,COL_BG); tft.drawString(asciiTr(String("! ")+cl.err),LUX+LUW/2,L_footCY); return; }
  const String words[4]={cfg.f0,cfg.f1,cfg.f2,cfg.f3}; footIdx=(footIdx+1)&3;
  tft.setTextColor(col,COL_BG);
  tft.drawString(String("* ")+asciiTr(words[footIdx]),LUX+LUW/2,L_footCY);
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
  drawFooter(COL_ACCENT);
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
  drawFooter(COL_GRAY);
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
const char CFG_HTML[] PROGMEM = R"HTML(<!doctype html><html lang=tr><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'><meta name=theme-color content='#0b0b10'><title>SmallTV</title>
<style>
:root{--bg:#0b0b10;--card:#15151c;--card2:#1c1c25;--line:#262631;--tx:#f2f2f5;--mu:#8b8b99;--co:#ff7a59;--te:#2fd3c0;--ok:#3ddc84;--bad:#ff5f6d;--r:16px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color:var(--tx);background:radial-gradient(900px 500px at 10% -10%,#ff7a5922,transparent 60%),radial-gradient(800px 500px at 110% 10%,#2fd3c01c,transparent 60%),var(--bg);min-height:100vh}
.w{max-width:560px;margin:auto;padding:18px 16px 110px}
header{display:flex;align-items:center;gap:12px;margin:4px 0 18px}
.logo{width:40px;height:40px;border-radius:12px;background:linear-gradient(135deg,var(--co),#ffb36b);display:grid;place-items:center;font-size:20px;box-shadow:0 6px 20px #ff7a5944}
header h1{font-size:19px;margin:0;font-weight:650;letter-spacing:-.2px}
header p{margin:0;color:var(--mu);font-size:13px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--mu);margin-right:6px;vertical-align:1px}
.dot.on{background:var(--ok);box-shadow:0 0 0 4px #3ddc8422}.dot.off{background:var(--bad)}
.card{background:linear-gradient(180deg,var(--card),#121218);border:1px solid var(--line);border-radius:var(--r);padding:16px;margin:12px 0}
.card h2{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.9px;color:var(--mu);margin:0 0 12px;display:flex;justify-content:space-between;align-items:center}
.alert{display:none;background:#ff5f6d18;border:1px solid #ff5f6d55;color:#ffb3b9;border-radius:12px;padding:10px 12px;font-size:13.5px;margin-bottom:12px}
.lim{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.m{background:var(--card2);border-radius:14px;padding:14px;position:relative;overflow:hidden}
.m .k{font-size:12.5px;color:var(--mu)}
.m .n{font-size:32px;font-weight:700;letter-spacing:-1px;margin:2px 0 8px;font-variant-numeric:tabular-nums}
.m .n small{font-size:15px;color:var(--mu);font-weight:500}
.bar{height:7px;border-radius:9px;background:#ffffff12;overflow:hidden}
.bar i{display:block;height:100%;width:0;border-radius:9px;transition:width .8s cubic-bezier(.2,.8,.2,1)}
.c5 i{background:linear-gradient(90deg,#ff5f4a,var(--co),#ffb36b)}.c7 i{background:linear-gradient(90deg,#16a99a,var(--te),#8ff5e6)}
.m .rs{font-size:12px;color:var(--mu);margin-top:8px}
.stats{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
.chip{background:var(--card2);border:1px solid var(--line);border-radius:99px;padding:5px 11px;font-size:12.5px;color:var(--mu)}
.chip b{color:var(--tx);font-weight:600}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0;padding-top:2px}
.row label{font-size:14.5px}.row .sub{display:block;font-size:12px;color:var(--mu)}
.sw{position:relative;width:46px;height:28px;flex:none}.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;background:#33333f;border-radius:99px;transition:.2s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:22px;height:22px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s;box-shadow:0 2px 6px #0006}
.sw input:checked+span{background:var(--ok)}.sw input:checked+span:before{transform:translateX(18px)}
.seg{display:inline-flex;background:var(--card2);border:1px solid var(--line);border-radius:11px;padding:3px}
.seg button{border:0;background:none;color:var(--mu);padding:7px 12px;border-radius:8px;font:inherit;font-size:13px;cursor:pointer}
.seg button.a{background:#ffffff14;color:var(--tx)}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:6px;border-radius:9px;background:#ffffff14;outline:0}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#fff;border:4px solid var(--co);cursor:pointer}
input[type=range]::-moz-range-thumb{width:14px;height:14px;border-radius:50%;background:#fff;border:4px solid var(--co)}
.rg{display:grid;grid-template-columns:52px 1fr 32px;align-items:center;gap:10px;padding:7px 0;font-size:14px}
.rg .v{text-align:right;color:var(--co);font-variant-numeric:tabular-nums;font-size:13px}
input[type=text],input[type=number]{width:100%;padding:10px 12px;border-radius:11px;border:1px solid var(--line);background:var(--card2);color:var(--tx);font:inherit;outline:0;transition:border .15s}
input:focus{border-color:var(--co)}
input[type=number]{width:84px;text-align:center}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.f label{display:block;font-size:12.5px;color:var(--mu);margin:0 0 5px}
.pv{display:flex;justify-content:center;margin:4px 0 10px}
canvas{border-radius:14px;background:#000;box-shadow:0 0 0 1px var(--line),0 12px 30px #0008;width:200px;height:200px}
.btn{border:0;border-radius:12px;padding:11px 16px;font:inherit;font-weight:600;cursor:pointer;background:var(--card2);color:var(--tx);border:1px solid var(--line)}
.bar2{position:fixed;left:0;right:0;bottom:0;padding:12px 16px calc(12px + env(safe-area-inset-bottom));background:linear-gradient(180deg,transparent,#0b0b10 35%)}
.bar2 .in{max-width:560px;margin:auto;display:flex;gap:10px}
.save{flex:1;background:linear-gradient(135deg,var(--co),#ff9e6b);color:#1a0d08;border:0;box-shadow:0 8px 24px #ff7a5944}
.save:disabled{opacity:.5}
.toast{position:fixed;left:50%;bottom:86px;transform:translate(-50%,20px);background:#22222c;border:1px solid var(--line);padding:10px 16px;border-radius:12px;font-size:14px;opacity:0;transition:.25s;pointer-events:none}
.toast.s{opacity:1;transform:translate(-50%,0)}
.links{text-align:center;font-size:13px;color:var(--mu)}.links a{color:var(--mu);margin:0 8px}
</style></head><body><div class=w>
<header><div class=logo>📺</div><div><h1>SmallTV</h1><p id=sub><span class=dot></span>bağlanıyor…</p></div></header>

<div class=card><h2>Claude limiti <span id=age></span></h2>
<div class=alert id=err></div>
<div class=lim>
<div class=m><div class=k id=k5>5 saatlik</div><div class=n id=n5>–</div><div class="bar c5"><i id=b5></i></div><div class=rs id=r5>&nbsp;</div></div>
<div class=m><div class=k id=k7>Haftalık</div><div class=n id=n7>–</div><div class="bar c7"><i id=b7></i></div><div class=rs id=r7>&nbsp;</div></div>
</div>
<div class=stats><span class=chip>PC <b id=pc>–</b></span><span class=chip>Wi-Fi <b id=rssi>–</b></span><span class=chip>Açık <b id=up>–</b></span></div>
</div>

<div class=card><h2>Ekranlar</h2>
<div class=row><label>Claude limiti<span class=sub>5 saat + haftalık kullanım</span></label><label class=sw><input type=checkbox id=clOn><span></span></label></div>
<div class=row><label>PC durumu<span class=sub>CPU / RAM</span></label><label class=sw><input type=checkbox id=pcOn><span></span></label></div>
<div class=row><label>Geçiş süresi<span class=sub>saniye · 0 = kapalı</span></label><input type=number id=rotSec min=0 max=600></div>
<div class=row><label>Ekranı değiştir</label><button class=btn onclick=sw()>Şimdi geç</button></div>
</div>

<div class=card><h2>Görünüm</h2>
<div class=row><label>Gösterim</label><div class=seg id=cUsed><button data-v=1>Kullanılan</button><button data-v=0>Kalan</button></div></div>
<div class=row><label>Tema</label><div class=seg id=theme><button data-v=1>Parlak</button><button data-v=0>Klasik</button></div></div>
<div class=rg style=margin-top:6px><span>Işık</span><input type=range id=bright min=5 max=255><span class=v id=brightv></span></div>
</div>

<div class=card><h2>Güvenli alan</h2>
<div class=pv><canvas id=pv width=240 height=240></canvas></div>
<div class=rg><span>Üst</span><input type=range id=sTop min=0 max=30><span class=v id=sTopv></span></div>
<div class=rg><span>Alt</span><input type=range id=sBot min=0 max=30><span class=v id=sBotv></span></div>
<div class=rg><span>Sol</span><input type=range id=sLeft min=0 max=30><span class=v id=sLeftv></span></div>
<div class=rg><span>Sağ</span><input type=range id=sRight min=0 max=30><span class=v id=sRightv></span></div>
</div>

<div class=card><h2>Metinler</h2>
<div class=f><label>Başlık</label><input type=text id=title maxlength=24></div>
<div class=grid2 style=margin-top:10px><div class=f><label>5 saat rozeti</label><input type=text id=pill5 maxlength=12></div><div class=f><label>Haftalık rozeti</label><input type=text id=pill7 maxlength=12></div></div>
<div class=f style=margin-top:10px><label>Sıfırlanma eki</label><input type=text id=rsuf maxlength=24></div>
<div class=f style=margin-top:10px><label>Alt satır kelimeleri</label><div class=grid2><input type=text id=f0 maxlength=16><input type=text id=f1 maxlength=16><input type=text id=f2 maxlength=16><input type=text id=f3 maxlength=16></div></div>
</div>
<p class=links><a href=/update>Firmware</a>·<a href=/wifi>Wi-Fi</a>·<a href=/health>health</a></p>
</div>
<div class=bar2><div class=in><button class="btn save" id=sv onclick=save()>Kaydet ve uygula</button></div></div>
<div class=toast id=t></div>
<script>
var $=function(i){return document.getElementById(i)},C={};
function toast(m){var t=$('t');t.textContent=m;t.classList.add('s');clearTimeout(t._h);t._h=setTimeout(function(){t.classList.remove('s')},2200)}
function seg(id,v){[].forEach.call($(id).children,function(b){b.classList.toggle('a',b.dataset.v==String(v))});$(id).dataset.v=v}
['cUsed','theme'].forEach(function(id){$(id).onclick=function(e){if(e.target.dataset.v!=null){seg(id,e.target.dataset.v);draw();poll()}}});
function rr(x,X,Y,W,H,R){x.beginPath();x.moveTo(X+R,Y);x.arcTo(X+W,Y,X+W,Y+H,R);x.arcTo(X+W,Y+H,X,Y+H,R);x.arcTo(X,Y+H,X,Y,R);x.arcTo(X,Y,X+W,Y,R);x.fill()}
function draw(){var x=$('pv').getContext('2d');x.fillStyle='#000';x.fillRect(0,0,240,240);
 var t=+$('sTop').value,b=+$('sBot').value,l=+$('sLeft').value,r=+$('sRight').value,ux=l,uy=t,uw=240-l-r,uh=240-t-b;
 var hH=26,fH=14,gap=6,cH=(uh-hH-fH-gap)/2,gl=$('theme').dataset.v=='1';
 x.fillStyle='#fff';x.font='600 13px system-ui';x.textAlign='center';x.fillText($('title').value||'Kullanım',ux+uw/2,uy+17);
 x.fillStyle='#ff7a59';x.fillRect(ux+3,uy+7,14,12);
 function card(cy,c1,c2){x.fillStyle=gl?c2:'#202024';rr(x,ux,cy,uw,cH,12);if(gl){x.strokeStyle=c1;x.lineWidth=1.5;x.beginPath();x.roundRect?x.roundRect(ux+.5,cy+.5,uw-1,cH-1,12):0;x.stroke()}
  x.fillStyle='#fff';x.font='bold 26px system-ui';x.textAlign='left';x.fillText('18%',ux+12,cy+cH*.55);x.fillStyle='#ffffff18';rr(x,ux+12,cy+cH-17,uw-24,7,3);x.fillStyle=c1;rr(x,ux+12,cy+cH-17,(uw-24)*.18,7,3)}
 card(uy+hH,'#ff7a59','#2a1612');card(uy+hH+cH+gap,'#2fd3c0','#0f2523');
 x.fillStyle='#999';x.font='11px system-ui';x.textAlign='center';x.fillText('* '+($('f0').value||'…'),ux+uw/2,uy+uh-3);
 x.strokeStyle='#ff7a59';x.lineWidth=1;x.setLineDash([4,3]);x.strokeRect(ux+.5,uy+.5,uw-1,uh-1);x.setLineDash([])}
['sTop','sBot','sLeft','sRight','bright'].forEach(function(i){$(i).oninput=function(){$(i+'v').textContent=$(i).value;draw()}});
['title','f0'].forEach(function(i){$(i).oninput=draw});
function load(){fetch('/config.json').then(function(r){return r.json()}).then(function(c){C=c;
 for(var k in c){var e=$(k);if(!e||e.classList.contains('seg'))continue;if(e.type=='checkbox')e.checked=!!c[k];else e.value=c[k];if($(k+'v'))$(k+'v').textContent=c[k]}
 seg('cUsed',c.cUsed?1:0);seg('theme',c.theme);$('k5').textContent=c.pill5||'5 saatlik';$('k7').textContent=c.pill7||'Haftalık';draw()})}
function save(){var o={},b=$('sv');['sTop','sBot','sLeft','sRight','rotSec','bright'].forEach(function(k){o[k]=+$(k).value});
 o.pcOn=$('pcOn').checked;o.clOn=$('clOn').checked;o.cUsed=+$('cUsed').dataset.v;o.theme=+$('theme').dataset.v;
 ['title','pill5','pill7','rsuf','f0','f1','f2','f3'].forEach(function(k){o[k]=$(k).value});
 b.disabled=true;fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(function(r){return r.text()}).then(function(t){toast('✓ '+t)}).catch(function(){toast('Kaydedilemedi')}).then(function(){b.disabled=false})}
function sw(){fetch('/switch',{method:'POST'}).then(function(){toast('Ekran değişti')})}
function dur(s){if(s<=0)return'';var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'g '+h+'s':h?h+'s '+m+'dk':m+'dk'}
function lim(n,v,rs,used){var p=v<0?-1:(used?v:100-v);$('n'+n).innerHTML=p<0?'–':p+'<small>%</small>';$('b'+n).style.width=(p<0?0:p)+'%';
 var now=Date.now()/1000;$('r'+n).textContent=rs>now?dur(rs-now)+' sonra sıfırlanır':' '}
function poll(){fetch('/health').then(function(r){return r.json()}).then(function(h){
 var on=h.cl_online&&!h.cl_err;$('sub').innerHTML='<span class="dot '+(on?'on':'off')+'"></span>'+h.ip+' · '+(on?'veri geliyor':h.cl_err?'Claude bağlantı hatası':'ajan veri göndermiyor');
 var e=$('err');if(h.cl_err){e.style.display='block';e.textContent='⚠ '+h.cl_err+' — bilgisayarda terminalde: claude → /login'}else if(!h.cl_online){e.style.display='block';e.textContent='⚠ Bilgisayardaki SmallTV ajanından veri gelmiyor.'}else e.style.display='none';
 var u=$('cUsed').dataset.v=='1';lim(5,h.h5,h.h5_reset,u);lim(7,h.d7,h.d7_reset,u);
 $('age').textContent=h.cl_age>=0?(h.cl_age<60?h.cl_age+' sn önce':Math.round(h.cl_age/60)+' dk önce'):'';
 $('pc').textContent=h.pc_online?Math.round(h.cpu)+'% CPU · '+Math.round(h.ram)+'% RAM':'kapalı';
 $('rssi').textContent=h.rssi+' dBm';$('up').textContent=dur(h.up)||'<1dk'}).catch(function(){$('sub').innerHTML='<span class="dot off"></span>cihaza ulaşılamıyor'})}
load();poll();setInterval(poll,5000);
</script></body></html>)HTML";

void handleCfgPage(){ server.send_P(200,"text/html; charset=utf-8",CFG_HTML); }
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
  const char* e=doc["err"]|"";
  bool hadErr=cl.err[0]!=0;
  strncpy(cl.err,e,sizeof(cl.err)-1); cl.err[sizeof(cl.err)-1]=0;
  if(!cl.err[0]){ cl.h5=doc["h5"]|cl.h5; cl.d7=doc["d7"]|cl.d7; cl.h5_reset=doc["h5_reset"]|cl.h5_reset; cl.d7_reset=doc["d7_reset"]|cl.d7_reset; }
  cl.lastUpdate=millis(); server.send(200,"text/plain","OK");
  if(hadErr!=(cl.err[0]!=0) && screen==1) needStatic=true;
}
void handleHealth(){
  JsonDocument d; d["pc_online"]=pcOnline();d["cpu"]=st.cpu;d["ram"]=st.ram;d["host"]=st.host;
  d["cl_online"]=clOnline();d["h5"]=cl.h5;d["d7"]=cl.d7;d["h5_reset"]=cl.h5_reset;d["d7_reset"]=cl.d7_reset;d["cl_err"]=cl.err;
  d["cl_age"]=cl.lastUpdate?(long)((millis()-cl.lastUpdate)/1000):-1;d["screen"]=screen;
  d["ip"]=WiFi.localIP().toString();d["rssi"]=WiFi.RSSI();d["up"]=(long)(millis()/1000);d["heap"]=ESP.getFreeHeap();
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
