// ESP32-2432S028R (Cheap Yellow Display) 卓上時計
//  上段: 西暦年月日 + 曜日
//  中段: 時刻 HH:MM(大) + 秒(小) / 右側に月齢
//  下段: 5日間の天気予報（アイコン + 最低/最高気温）

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

// ---------- CYD (ILI9341) 用ディスプレイ設定 ----------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 14;
      cfg.pin_mosi    = 13;
      cfg.pin_miso    = 12;
      cfg.pin_dc      = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs          = 15;
      cfg.pin_rst         = -1;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.memory_width    = 240;
      cfg.memory_height   = 320;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.readable        = true;
      cfg.invert          = false;  // 色が反転して見える場合は true
      cfg.rgb_order       = false;  // 赤と青が入れ替わる場合は true
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      // タッチパネル (XPT2046) は表示とは別の SPI に接続されている
      auto cfg = _touch.config();
      cfg.x_min           = 300;
      cfg.x_max           = 3900;
      cfg.y_min           = 200;
      cfg.y_max           = 3700;
      cfg.pin_int         = -1;
      cfg.bus_shared      = false;
      cfg.offset_rotation = 0;
      cfg.spi_host        = SPI3_HOST;
      cfg.freq            = 1000000;
      cfg.pin_sclk        = 25;
      cfg.pin_mosi        = 32;
      cfg.pin_miso        = 39;
      cfg.pin_cs          = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;

// ---------- 色 ----------
static const uint16_t COL_BG   = TFT_BLACK;
static const uint16_t COL_DATE = TFT_WHITE;
static const uint16_t COL_TIME = 0x07FF;                    // シアン
static const uint16_t COL_SEC  = 0xFD20;                    // オレンジ
static const uint16_t COL_LINE = 0x4208;                    // 暗いグレー
static uint16_t COL_SUN, COL_CLOUD, COL_DARK, COL_RAIN, COL_TMIN, COL_TMAX,
                COL_SUNDAY, COL_SATURDAY, COL_MOON_LIT, COL_MOON_DARK;

static void initColors() {
  COL_SUN       = lgfx::color565(255, 200, 0);
  COL_CLOUD     = lgfx::color565(205, 210, 220);
  COL_DARK      = lgfx::color565(120, 125, 140);
  COL_RAIN      = lgfx::color565(70, 150, 255);
  COL_TMIN      = lgfx::color565(100, 170, 255);
  COL_TMAX      = lgfx::color565(255, 100, 90);
  COL_SUNDAY    = lgfx::color565(255, 110, 110);
  COL_SATURDAY  = lgfx::color565(110, 170, 255);
  COL_MOON_LIT  = lgfx::color565(255, 240, 170);
  COL_MOON_DARK = lgfx::color565(35, 40, 60);
}

// ---------- レイアウト ----------
static const int Y_LINE1   = 50;    // 日付行の下線
static const int Y_LINE2   = 138;   // 時刻行の下線
static const int TIME_X    = 4;
static const int TIME_Y    = 60;
static const int MOON_CX   = 284;
static const int MOON_CY   = 88;
static const int MOON_R    = 24;
static const int W_Y0      = 140;   // 天気行の上端
static const int W_COL_W   = 64;    // 320 / 5

static const char *WEEKDAY_JA[] = {"日", "月", "火", "水", "木", "金", "土"};

static int timeW = 0, timeH = 0;

// ---------- バックライト（タッチで 通常 → 暗い → 消灯 を順に切替）----------
static const uint8_t BRIGHTNESS_LEVELS[] = {200, 30, 0};  // 通常 / 暗い / 消灯
static const int BRIGHTNESS_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
static int brightnessIdx = 0;

static void handleTouch() {
  static bool wasTouched = false;
  static uint32_t lastMs = 0;

  int32_t x, y;
  bool touched = lcd.getTouch(&x, &y) > 0;

  if (touched && !wasTouched && millis() - lastMs > 300) {
    brightnessIdx = (brightnessIdx + 1) % BRIGHTNESS_COUNT;
    lcd.setBrightness(BRIGHTNESS_LEVELS[brightnessIdx]);
    lastMs = millis();
  }
  wasTouched = touched;
}

static bool timeIsValid(const tm &t) { return (t.tm_year + 1900) >= 2024; }

// ============================================================
//  共通
// ============================================================
static void drawStatic() {
  lcd.fillScreen(COL_BG);
  lcd.drawFastHLine(10, Y_LINE1, lcd.width() - 20, COL_LINE);
  lcd.drawFastHLine(10, Y_LINE2, lcd.width() - 20, COL_LINE);
}

static void drawCenterMessage(const char *msg) {
  lcd.setFont(&fonts::lgfxJapanGothic_24);
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_YELLOW, COL_BG);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(lcd.width());
  lcd.drawString(msg, lcd.width() / 2, lcd.height() / 2);
}

// ============================================================
//  上段: 日付
// ============================================================
static void drawDate(const tm &t) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%d年%d月%d日(%s)",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, WEEKDAY_JA[t.tm_wday]);

  lcd.setFont(&fonts::lgfxJapanGothic_28);
  lcd.setTextSize(1);
  lcd.setTextColor(COL_DATE, COL_BG);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(lcd.width());
  lcd.drawString(buf, lcd.width() / 2, 25);
}

// ============================================================
//  中段: 時刻 (HH:MM 大 + 秒 小)
// ============================================================
static void initTimeLayout() {
  lcd.setFont(&fonts::Font7);
  lcd.setTextSize(1.4f);
  timeW = lcd.textWidth("00:00");
  timeH = lcd.fontHeight();
}

static void drawHourMin(const tm &t) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);

  lcd.setFont(&fonts::Font7);
  lcd.setTextSize(1.4f);
  lcd.setTextColor(COL_TIME, COL_BG);
  lcd.setTextDatum(top_left);
  lcd.setTextPadding(timeW);
  lcd.drawString(buf, TIME_X, TIME_Y);
}

static void drawSecond(const tm &t) {
  char buf[4];
  snprintf(buf, sizeof(buf), "%02d", t.tm_sec);

  lcd.setFont(&fonts::Font4);
  lcd.setTextSize(1);
  lcd.setTextColor(COL_SEC, COL_BG);
  lcd.setTextDatum(bottom_left);
  lcd.setTextPadding(lcd.textWidth("00"));
  lcd.drawString(buf, TIME_X + timeW + 6, TIME_Y + timeH);
}

// ============================================================
//  中段右: 月齢
// ============================================================
static const double SYNODIC = 29.530588853;  // 朔望月（日）

// 2000-01-06 18:14 UTC（新月）からの経過日数で近似計算
static double calcMoonAge(time_t when) {
  double d = (double)(when - 947182440L) / 86400.0;
  double a = fmod(d, SYNODIC);
  if (a < 0) a += SYNODIC;
  return a;
}

static void drawMoon(const tm &t) {
  // 日本の暦に合わせ、その日の正午(JST)の月齢を使う
  tm n = t;
  n.tm_hour = 12; n.tm_min = 0; n.tm_sec = 0;
  double age = calcMoonAge(mktime(&n));

  lcd.fillRect(MOON_CX - 36, Y_LINE1 + 4, 72, Y_LINE2 - Y_LINE1 - 8, COL_BG);

  const int R = MOON_R;
  lcd.fillCircle(MOON_CX, MOON_CY, R, COL_MOON_DARK);

  double phase = age / SYNODIC;                 // 0=新月, 0.5=満月
  double c = cos(2.0 * M_PI * phase);
  bool waxing = phase < 0.5;                    // 満ちていく間は右側が明るい（北半球）

  for (int dy = -R; dy <= R; dy++) {
    int w  = (int)sqrtf((float)(R * R - dy * dy));
    int xt = (int)lroundf(c * w);
    int x0, x1;
    if (waxing) { x0 = MOON_CX + xt; x1 = MOON_CX + w; }
    else        { x0 = MOON_CX - w;  x1 = MOON_CX - xt; }
    if (x1 >= x0) lcd.drawFastHLine(x0, MOON_CY + dy, x1 - x0 + 1, COL_MOON_LIT);
  }
  lcd.drawCircle(MOON_CX, MOON_CY, R, COL_DARK);

  char buf[16];
  snprintf(buf, sizeof(buf), "月齢%.1f", age);
  lcd.setFont(&fonts::lgfxJapanGothic_16);
  lcd.setTextSize(1);
  lcd.setTextColor(COL_DATE, COL_BG);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(0);
  lcd.drawString(buf, MOON_CX, 124);
}

// ============================================================
//  下段: 天気予報
// ============================================================
struct DayForecast {
  int code = 0;
  int tmin = 0;
  int tmax = 0;
  int wday = 0;
};
static DayForecast forecast[5];
static bool weatherLoaded = false;

enum IconKind { ICON_SUN, ICON_PARTLY, ICON_CLOUD, ICON_FOG, ICON_RAIN, ICON_SNOW, ICON_THUNDER };

// WMO weather code -> アイコン種別
static IconKind iconKind(int code) {
  if (code == 0 || code == 1) return ICON_SUN;
  if (code == 2) return ICON_PARTLY;
  if (code == 3) return ICON_CLOUD;
  if (code == 45 || code == 48) return ICON_FOG;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return ICON_RAIN;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return ICON_SNOW;
  if (code >= 95) return ICON_THUNDER;
  return ICON_CLOUD;
}

static void drawSunShape(int cx, int cy, int r, int rayIn, int rayOut) {
  lcd.fillCircle(cx, cy, r, COL_SUN);
  for (int i = 0; i < 8; i++) {
    float a = i * M_PI / 4.0f;
    int x0 = cx + (int)lroundf(cosf(a) * rayIn);
    int y0 = cy + (int)lroundf(sinf(a) * rayIn);
    int x1 = cx + (int)lroundf(cosf(a) * rayOut);
    int y1 = cy + (int)lroundf(sinf(a) * rayOut);
    lcd.drawLine(x0, y0, x1, y1, COL_SUN);
    lcd.drawLine(x0 + 1, y0, x1 + 1, y1, COL_SUN);
  }
}

// 雲（中心 cx, cy 付近。幅約34 x 高さ約23）
static void drawCloudShape(int cx, int cy, uint16_t col) {
  lcd.fillCircle(cx - 9, cy + 2, 7, col);
  lcd.fillCircle(cx,     cy - 4, 10, col);
  lcd.fillCircle(cx + 10, cy + 1, 8, col);
  lcd.fillRect(cx - 9, cy + 2, 20, 8, col);
}

static void drawWeatherIcon(int code, int cx, int cy) {
  switch (iconKind(code)) {
    case ICON_SUN:
      drawSunShape(cx, cy, 9, 13, 19);
      break;
    case ICON_PARTLY:
      drawSunShape(cx - 8, cy - 8, 7, 10, 14);
      drawCloudShape(cx + 3, cy + 4, COL_CLOUD);
      break;
    case ICON_CLOUD:
      drawCloudShape(cx, cy + 2, COL_CLOUD);
      break;
    case ICON_FOG:
      for (int i = 0; i < 4; i++) {
        int w = (i % 2 == 0) ? 34 : 26;
        lcd.fillRoundRect(cx - w / 2, cy - 14 + i * 9, w, 4, 2, COL_DARK);
      }
      break;
    case ICON_RAIN:
      drawCloudShape(cx, cy - 6, COL_DARK);
      for (int i = 0; i < 3; i++) {
        int x = cx - 9 + i * 9;
        lcd.drawLine(x, cy + 7, x - 3, cy + 17, COL_RAIN);
        lcd.drawLine(x + 1, cy + 7, x - 2, cy + 17, COL_RAIN);
      }
      break;
    case ICON_SNOW:
      drawCloudShape(cx, cy - 6, COL_DARK);
      for (int i = 0; i < 3; i++) {
        int x = cx - 9 + i * 9;
        int y = cy + 11 + ((i % 2) ? 4 : 0);
        lcd.fillCircle(x, y, 2, TFT_WHITE);
      }
      break;
    case ICON_THUNDER:
      drawCloudShape(cx, cy - 6, COL_DARK);
      lcd.fillTriangle(cx + 4, cy + 4, cx - 4, cy + 14, cx + 1, cy + 14, COL_SUN);
      lcd.fillTriangle(cx - 1, cy + 11, cx + 6, cy + 11, cx - 3, cy + 22, COL_SUN);
      break;
  }
}

static void drawWeatherMessage(const char *msg) {
  lcd.fillRect(0, W_Y0, lcd.width(), lcd.height() - W_Y0, COL_BG);
  lcd.setFont(&fonts::lgfxJapanGothic_16);
  lcd.setTextSize(1);
  lcd.setTextColor(COL_DARK, COL_BG);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(0);
  lcd.drawString(msg, lcd.width() / 2, W_Y0 + 50);
}

static void drawWeather() {
  if (!weatherLoaded) { drawWeatherMessage("天気取得失敗"); return; }

  lcd.fillRect(0, W_Y0, lcd.width(), lcd.height() - W_Y0, COL_BG);
  lcd.setFont(&fonts::lgfxJapanGothic_16);
  lcd.setTextSize(1);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(0);

  for (int i = 0; i < 5; i++) {
    const DayForecast &f = forecast[i];
    int cx = i * W_COL_W + W_COL_W / 2;

    // 曜日ラベル
    uint16_t lc = (f.wday == 0) ? COL_SUNDAY : (f.wday == 6) ? COL_SATURDAY : COL_DATE;
    lcd.setTextColor(lc, COL_BG);
    lcd.drawString(i == 0 ? "今日" : WEEKDAY_JA[f.wday], cx, W_Y0 + 10);

    // アイコン
    drawWeatherIcon(f.code, cx, W_Y0 + 44);

    // 最高気温（上） / 最低気温（下）
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", f.tmax);
    lcd.setTextColor(COL_TMAX, COL_BG);
    lcd.drawString(buf, cx, W_Y0 + 74);

    snprintf(buf, sizeof(buf), "%d", f.tmin);
    lcd.setTextColor(COL_TMIN, COL_BG);
    lcd.drawString(buf, cx, W_Y0 + 90);
  }
}

// Open-Meteo から5日分を取得
static bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[300];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=Asia%%2FTokyo&forecast_days=5",
           WEATHER_LATITUDE, WEATHER_LONGITUDE);

  WiFiClientSecure client;
  client.setInsecure();  // 証明書検証を省略（表示用途のため）
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("weather HTTP error: %d\n", status);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonArray times = doc["daily"]["time"];
  JsonArray codes = doc["daily"]["weather_code"];
  JsonArray tmaxs = doc["daily"]["temperature_2m_max"];
  JsonArray tmins = doc["daily"]["temperature_2m_min"];
  if (times.isNull() || codes.isNull() || tmaxs.isNull() || tmins.isNull()) return false;
  if (times.size() < 5) return false;

  for (int i = 0; i < 5; i++) {
    int y = 0, m = 0, d = 0;
    sscanf(times[i] | "", "%d-%d-%d", &y, &m, &d);
    tm tmp = {};
    tmp.tm_year = y - 1900;
    tmp.tm_mon  = m - 1;
    tmp.tm_mday = d;
    tmp.tm_hour = 12;
    mktime(&tmp);

    forecast[i].code = codes[i] | 0;
    forecast[i].tmax = (int)lroundf(tmaxs[i].as<float>());
    forecast[i].tmin = (int)lroundf(tmins[i].as<float>());
    forecast[i].wday = tmp.tm_wday;
  }
  return true;
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(3);  // 横向き。上下逆なら 1 に変更
  lcd.setBrightness(BRIGHTNESS_LEVELS[brightnessIdx]);
  initColors();
  initTimeLayout();

  drawStatic();
  drawCenterMessage("Wi-Fi接続中...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }

  configTzTime(TZ_JST, NTP_SV1, NTP_SV2);

  drawStatic();
  drawCenterMessage("時刻同期中...");
}

void loop() {
  static int lastSec = -1, lastMin = -1, lastDay = -1;
  static bool synced = false;
  static uint32_t nextWeatherMs = 0;

  // Wi-Fi が切れたら再接続（NTP は自動で定期再同期される）
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  handleTouch();

  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);

  if (!timeIsValid(t)) {
    delay(200);
    return;
  }

  if (!synced) {  // 初回同期完了
    synced = true;
    drawStatic();
    drawWeatherMessage("天気取得中...");
    lastSec = lastMin = lastDay = -1;
  }

  if (t.tm_mday != lastDay) {
    drawDate(t);
    drawMoon(t);
    if (lastDay != -1) nextWeatherMs = millis();  // 日付が変わったら天気も更新
    lastDay = t.tm_mday;
  }
  if (t.tm_min != lastMin) { drawHourMin(t); lastMin = t.tm_min; }
  if (t.tm_sec != lastSec) { drawSecond(t);  lastSec = t.tm_sec; }

  // 天気の取得・描画（取得中は数秒間、秒表示が止まります）
  if (WiFi.status() == WL_CONNECTED && (int32_t)(millis() - nextWeatherMs) >= 0) {
    bool ok = fetchWeather();
    if (ok) weatherLoaded = true;
    nextWeatherMs = millis() + (ok ? WEATHER_INTERVAL_MIN * 60000UL : 60000UL);
    drawWeather();
  }

  delay(50);
}
