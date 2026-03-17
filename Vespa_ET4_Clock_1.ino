// ============================================================
//  Vespa Clock Firmware v1
//  Hardware: ESP32-C3 SuperMini
//  Display:  Sharp Memory LCD 160x68 (Adafruit_SharpMem)
//  RTC:      DS3231 (RTClib)
//  Button A: GPIO 5  (mode advance)
//  Button B: GPIO 10 (value change)
// ============================================================

#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <Preferences.h>
#include <RTClib.h>
#include "constants.h"

// ── Display ─────────────────────────────────────────────────
#define SHARP_SCK   4
#define SHARP_MOSI  6
#define SHARP_SS    7
#define BLACK       0
#define WHITE       1

Adafruit_SharpMem display(SHARP_SCK, SHARP_MOSI, SHARP_SS, 160, 68);

// ── Hardware ─────────────────────────────────────────────────
#define BTN_A       GPIO_NUM_5
#define BTN_B       GPIO_NUM_10

// ── RTC ──────────────────────────────────────────────────────
RTC_DS3231 rtc;

// ── Preferences ──────────────────────────────────────────────
Preferences prefs;

// ── Settings ─────────────────────────────────────────────────
bool use24hour  = false;  // default 12hr
bool useCelsius = true;   // default Celsius

// ── State machine ────────────────────────────────────────────
enum AppMode {
  MODE_CLOCK,
  MODE_TEMPERATURE,
  MODE_SET_HOURS,
  MODE_SET_MINUTES,
  MODE_SET_HOUR_FORMAT,
  MODE_SET_TEMPERATURE
};

AppMode currentMode = MODE_CLOCK;

// ── Timed-mode tracking ──────────────────────────────────────
unsigned long modeEntryTime   = 0;
unsigned long setTimeoutStart = 0;

// ── Working time (used during set modes) ─────────────────────
int  setHour        = 0;
int  setMinute      = 0;
bool timeWasChanged = false;

// ── Button state ─────────────────────────────────────────────
bool          btnA_lastState  = HIGH;
bool          btnB_lastState  = HIGH;
bool          btnA_consumed   = false;
unsigned long btnB_holdStart  = 0;
bool          btnB_holding    = false;
unsigned long btnB_lastRepeat = 0;
#define BTN_HOLD_MS    500
#define BTN_REPEAT_MS  500

// ── Display refresh tracking ─────────────────────────────────
int  lastDisplayedMinute = -1;
int  lastDisplayedHour   = -1;
bool forceRedraw         = true;

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(9600);
  Serial.println("Vespa Clock Starting");

  gpio_set_direction(BTN_A, GPIO_MODE_INPUT);
  gpio_set_direction(BTN_B, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BTN_A, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(BTN_B, GPIO_PULLUP_ONLY);

  display.begin();
  display.clearDisplay();

  display.drawBitmap(0, 6, epd_bitmap_cropped_vespa_logo, 160, 56, BLACK);
  display.refresh();

  delay(3000);

  bool rtcOK = false;
  for (int attempt = 0; attempt < 15; attempt++) {
    delay(700);
    if (rtc.begin()) {
      rtcOK = true;
      break;
    }
    Serial.print("RTC init attempt ");
    Serial.print(attempt + 1);
    Serial.println(" failed, retrying...");
  }

  if (!rtcOK) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(BLACK);
    display.setCursor(10, 24);
    display.println("RTC ERROR");
    display.refresh();
    while (true) delay(1000);
  }

  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  delay(1000);
  display.clearDisplay();
  display.refresh();

  loadPreferences();
  enterMode(MODE_CLOCK);
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  switch (currentMode) {
    case MODE_CLOCK:           loopClock();          break;
    case MODE_TEMPERATURE:     loopTemperature();    break;
    case MODE_SET_HOURS:       loopSetHours();       break;
    case MODE_SET_MINUTES:     loopSetMinutes();     break;
    case MODE_SET_HOUR_FORMAT: loopSetHourFormat();  break;
    case MODE_SET_TEMPERATURE: loopSetTemperature(); break;
  }
}

// ============================================================
//  MODE ENTRY
// ============================================================
void enterMode(AppMode newMode) {
  currentMode   = newMode;
  modeEntryTime = millis();
  forceRedraw   = true;

  if (newMode == MODE_SET_HOURS || newMode == MODE_SET_MINUTES ||
      newMode == MODE_SET_HOUR_FORMAT || newMode == MODE_SET_TEMPERATURE) {
    setTimeoutStart = millis();
    if (newMode == MODE_SET_HOURS) {
      DateTime now   = rtc.now();
      setHour        = now.hour();
      setMinute      = now.minute();
      timeWasChanged = false;
    }
  }

  if (newMode == MODE_CLOCK) {
    savePreferences();
  }

  display.clearDisplay();
  lastDisplayedHour   = -1;
  lastDisplayedMinute = -1;
}

// ============================================================
//  BUTTON HANDLING
// ============================================================
bool btnA_pressed() {
  bool state = digitalRead(BTN_A);
  if (state == HIGH) {
    btnA_consumed  = false;
    btnA_lastState = HIGH;
    return false;
  }
  if (!btnA_consumed && btnA_lastState == HIGH) {
    btnA_consumed  = true;
    btnA_lastState = LOW;
    return true;
  }
  btnA_lastState = LOW;
  return false;
}

bool btnB_pressed() {
  bool state = digitalRead(BTN_B);
  if (state == HIGH) {
    btnB_holding   = false;
    btnB_lastState = HIGH;
    return false;
  }
  unsigned long now = millis();
  if (btnB_lastState == HIGH) {
    btnB_lastState  = LOW;
    btnB_holdStart  = now;
    btnB_lastRepeat = now;
    btnB_holding    = false;
    return true;
  }
  if (!btnB_holding && (now - btnB_holdStart >= BTN_HOLD_MS)) {
    btnB_holding    = true;
    btnB_lastRepeat = now;
    return true;
  }
  if (btnB_holding && (now - btnB_lastRepeat >= BTN_REPEAT_MS)) {
    btnB_lastRepeat = now;
    return true;
  }
  return false;
}

// ============================================================
//  CLOCK MODE
// ============================================================
void loopClock() {
  if (btnA_pressed()) {
    enterMode(MODE_TEMPERATURE);
    return;
  }
  DateTime now = rtc.now();
  if (!forceRedraw &&
      now.hour()   == lastDisplayedHour &&
      now.minute() == lastDisplayedMinute) {
    return;
  }
  forceRedraw         = false;
  lastDisplayedHour   = now.hour();
  lastDisplayedMinute = now.minute();
  display.clearDisplay();
  drawTime(now.hour(), now.minute(), true);
  display.refresh();
}

// ============================================================
//  TEMPERATURE MODE
// ============================================================
void loopTemperature() {
  if (btnA_pressed()) {
    enterMode(MODE_SET_HOURS);
    return;
  }
  if (millis() - modeEntryTime >= 6000) {
    enterMode(MODE_CLOCK);
    return;
  }
  if (!forceRedraw) return;
  forceRedraw = false;
  float tempC = rtc.getTemperature();
  float temp  = useCelsius ? tempC : (tempC * 9.0f / 5.0f) + 32.0f;
  display.clearDisplay();
  drawTemperature(temp);
  display.refresh();
}

// ============================================================
//  SET MODE – HOURS
// ============================================================
void loopSetHours() {
  if (millis() - setTimeoutStart >= 10000) {
    saveTimeToRTC();
    enterMode(MODE_CLOCK);
    return;
  }
  if (btnA_pressed()) {
    setTimeoutStart = millis();
    enterMode(MODE_SET_MINUTES);
    return;
  }
  if (btnB_pressed()) {
    setTimeoutStart = millis();
    setHour        = (setHour + 1) % 24;
    timeWasChanged = true;
    forceRedraw    = true;
  }
  if (!forceRedraw) return;
  forceRedraw = false;
  display.clearDisplay();
  drawSetHours();
  display.refresh();
}

// ============================================================
//  SET MODE – MINUTES
// ============================================================
void loopSetMinutes() {
  if (millis() - setTimeoutStart >= 10000) {
    saveTimeToRTC();
    enterMode(MODE_CLOCK);
    return;
  }
  if (btnA_pressed()) {
    setTimeoutStart = millis();
    enterMode(MODE_SET_HOUR_FORMAT);
    return;
  }
  if (btnB_pressed()) {
    setTimeoutStart = millis();
    setMinute      = (setMinute + 1) % 60;
    timeWasChanged = true;
    forceRedraw    = true;
  }
  if (!forceRedraw) return;
  forceRedraw = false;
  display.clearDisplay();
  drawSetMinutes();
  display.refresh();
}

// ============================================================
//  SET MODE – HOUR FORMAT
// ============================================================
void loopSetHourFormat() {
  if (millis() - setTimeoutStart >= 10000) {
    savePreferences();
    enterMode(MODE_CLOCK);
    return;
  }
  if (btnA_pressed()) {
    savePreferences();
    setTimeoutStart = millis();
    enterMode(MODE_SET_TEMPERATURE);
    return;
  }
  if (btnB_pressed()) {
    setTimeoutStart = millis();
    use24hour   = !use24hour;
    forceRedraw = true;
  }
  if (!forceRedraw) return;
  forceRedraw = false;
  display.clearDisplay();
  drawSetHourFormat();
  display.refresh();
}

// ============================================================
//  SET MODE – TEMPERATURE
// ============================================================
void loopSetTemperature() {
  if (millis() - setTimeoutStart >= 10000) {
    savePreferences();
    saveTimeToRTC();
    enterMode(MODE_CLOCK);
    return;
  }
  if (btnA_pressed()) {
    savePreferences();
    saveTimeToRTC();
    enterMode(MODE_CLOCK);
    return;
  }
  if (btnB_pressed()) {
    setTimeoutStart = millis();
    useCelsius  = !useCelsius;
    forceRedraw = true;
  }
  if (!forceRedraw) return;
  forceRedraw = false;
  display.clearDisplay();
  drawSetTemperature();
  display.refresh();
}

// ============================================================
//  DRAW HELPERS
// ============================================================
void drawTime(int hour24, int minute, bool showColon) {
  int dispHour = hour24;
  if (!use24hour) {
    dispHour = hour24 % 12;
    if (dispHour == 0) dispHour = 12;
  }
  int h1 = dispHour / 10;
  int h2 = dispHour % 10;
  int m1 = minute / 10;
  int m2 = minute % 10;
  if (!use24hour && h1 == 0) {
    // suppress leading zero
  } else {
    drawDigit(DIGIT_0_X, LCD_Y_OFFSET, h1);
  }
  drawDigit(DIGIT_1_X, LCD_Y_OFFSET, h2);
  if (showColon) drawColon(COLON_X, LCD_Y_OFFSET);
  drawDigit(DIGIT_2_X, LCD_Y_OFFSET, m1);
  drawDigit(DIGIT_3_X, LCD_Y_OFFSET, m2);
}

void drawTemperature(float temp) {
  bool negative = (temp < 0);
  int  abstemp  = (int)(negative ? -temp : temp);
  if (abstemp > 999) abstemp = 999;
  int hundreds = abstemp / 100;
  int tens     = (abstemp / 10) % 10;
  int ones     = abstemp % 10;
  drawCustomSegments(DIGIT_3_X, LCD_Y_OFFSET, 0b1100011);
  drawDigit(DIGIT_2_X, LCD_Y_OFFSET, ones);
  if (abstemp >= 10 || hundreds > 0) {
    drawDigit(DIGIT_1_X, LCD_Y_OFFSET, tens);
  }
  if (hundreds > 0) {
    drawDigit(DIGIT_0_X, LCD_Y_OFFSET, hundreds);
  } else if (negative) {
    drawCustomSegments(DIGIT_0_X, LCD_Y_OFFSET, 0b0000001);
  }
}

void drawSetHours() {
  int  dispHour = setHour;
  bool isPM     = false;
  if (!use24hour) {
    isPM     = (setHour >= 12);
    dispHour = setHour % 12;
    if (dispHour == 0) dispHour = 12;
  }
  int h1 = dispHour / 10;
  int h2 = dispHour % 10;
  if (!use24hour && h1 == 0) {
    // blank
  } else {
    drawDigit(DIGIT_0_X, LCD_Y_OFFSET, h1);
  }
  drawDigit(DIGIT_1_X, LCD_Y_OFFSET, h2);
  drawColon(COLON_X, LCD_Y_OFFSET);
  if (!use24hour) {
    drawLetterInDigit(DIGIT_2_X, LCD_Y_OFFSET, isPM ? 'P' : 'A');
  }
}

void drawSetMinutes() {
  drawColon(COLON_X, LCD_Y_OFFSET);
  drawDigit(DIGIT_2_X, LCD_Y_OFFSET, setMinute / 10);
  drawDigit(DIGIT_3_X, LCD_Y_OFFSET, setMinute % 10);
}

void drawSetHourFormat() {
  if (use24hour) {
    drawDigit(DIGIT_0_X, LCD_Y_OFFSET, 2);
    drawDigit(DIGIT_1_X, LCD_Y_OFFSET, 4);
  } else {
    drawDigit(DIGIT_0_X, LCD_Y_OFFSET, 1);
    drawDigit(DIGIT_1_X, LCD_Y_OFFSET, 2);
  }
  drawLetterInDigit(DIGIT_2_X, LCD_Y_OFFSET, 'H');
}

void drawSetTemperature() {
  drawLetterInDigit(DIGIT_0_X, LCD_Y_OFFSET, 'T');
  drawLetterInDigit(DIGIT_1_X, LCD_Y_OFFSET, 'E');
  drawLetterInDigit(DIGIT_2_X, LCD_Y_OFFSET, useCelsius ? 'C' : 'F');
}

// ============================================================
//  SEGMENT DRAWING
// ============================================================
void drawDigit(int x, int y, int digit) {
  if (digit < 0 || digit > 9) return;
  drawCustomSegments(x, y, digitSegments[digit]);
}

void drawCustomSegments(int x, int y, byte segs) {
  if (segs & 0b1000000) display.drawBitmap(x, y, epd_bitmap_seg0, 32, 64, BLACK);
  if (segs & 0b0100000) display.drawBitmap(x, y, epd_bitmap_seg1, 32, 64, BLACK);
  if (segs & 0b0010000) display.drawBitmap(x, y, epd_bitmap_seg2, 32, 64, BLACK);
  if (segs & 0b0001000) display.drawBitmap(x, y, epd_bitmap_seg3, 32, 64, BLACK);
  if (segs & 0b0000100) display.drawBitmap(x, y, epd_bitmap_seg4, 32, 64, BLACK);
  if (segs & 0b0000010) display.drawBitmap(x, y, epd_bitmap_seg5, 32, 64, BLACK);
  if (segs & 0b0000001) display.drawBitmap(x, y, epd_bitmap_seg6, 32, 64, BLACK);
}

void drawColon(int x, int y) {
  display.fillCircle(x, y + 20, 3, BLACK);
  display.fillCircle(x, y + 44, 3, BLACK);
}

void drawLetterInDigit(int x, int y, char c) {
  byte segs = 0;
  switch (toupper(c)) {
    case 'A': segs = 0b1110111; break;
    case 'C': segs = 0b1001110; break;
    case 'E': segs = 0b1001111; break;
    case 'F': segs = 0b1000110; break;
    case 'H': segs = 0b0110111; break;
    case 'P': segs = 0b1100111; break;
    case 'T': segs = 0b0001111; break;
    default:  segs = 0b0000001; break;
  }
  drawCustomSegments(x, y, segs);
}

// ============================================================
//  RTC
// ============================================================
void saveTimeToRTC() {
  if (!timeWasChanged) return;
  DateTime now = rtc.now();
  rtc.adjust(DateTime(now.year(), now.month(), now.day(),
                      setHour, setMinute, 0));
  Serial.println("RTC updated from manual set");
}

// ============================================================
//  PREFERENCES
// ============================================================
void loadPreferences() {
  prefs.begin("vespa-clock", true);
  uint32_t magic1 = prefs.getUInt("magic_s", 0);
  uint32_t magic2 = prefs.getUInt("magic_e", 0);
  if (magic1 != 0 && magic1 == magic2) {
    use24hour  = prefs.getBool("use24h", false);
    useCelsius = prefs.getBool("useC",   true);
    Serial.println("Preferences loaded.");
  } else {
    Serial.println("No valid preferences, using defaults.");
    use24hour  = false;
    useCelsius = true;
  }
  prefs.end();
}

void savePreferences() {
  prefs.begin("vespa-clock", false);
  uint32_t magic;
  do { magic = esp_random(); } while (magic == 0);
  prefs.putUInt("magic_s", magic);
  prefs.putBool("use24h",  use24hour);
  prefs.putBool("useC",    useCelsius);
  prefs.putUInt("magic_e", magic);
  prefs.end();
  Serial.println("Preferences saved.");
}