#pragma once

// ===== Wi-Fi 設定（2.4GHz のみ対応）=====
#define WIFI_SSID     "your_ssid"
#define WIFI_PASSWORD "your_password"

// ===== 時刻設定 =====
#define TZ_JST   "JST-9"
#define NTP_SV1  "ntp.nict.jp"
#define NTP_SV2  "ntp.jst.mfeed.ad.jp"

// ===== 天気予報の地点（初期値: 盛岡市）=====
#define WEATHER_LATITUDE   39.7036
#define WEATHER_LONGITUDE  141.1527

// 天気の更新間隔（分）
#define WEATHER_INTERVAL_MIN 30
