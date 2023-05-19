#include <Arduino.h>
#include <algorithm>

#include "OC_apps.h"
#include "OC_bitmaps.h"
#include "OC_calibration.h"
#include "OC_config.h"
#include "OC_core.h"
#include "OC_gpio.h"
#include "OC_menus.h"
#include "OC_ui.h"
#include "OC_options.h"
#include "src/drivers/display.h"

#ifdef VOR
#include "VBiasManager.h"
VBiasManager *VBiasManager::instance = 0;
#endif

extern uint_fast8_t MENU_REDRAW;

namespace OC {

Ui ui;

void Ui::Init() {
  ticks_ = 0;
  set_screensaver_timeout(SCREENSAVER_TIMEOUT_S);
  set_blanking_timeout(BLANKING_TIMEOUT_M);

#if defined(VOR) && !defined(VOR_NO_RANGE_BUTTON)
  static const int button_pins[] = { but_top, but_bot, butL, butR, but_mid };
#else
  static const int button_pins[] = { but_top, but_bot, butL, butR };
#endif

  for (size_t i = 0; i < CONTROL_BUTTON_LAST; ++i) {
    buttons_[i].Init(button_pins[i], OC_GPIO_BUTTON_PINMODE);
  }
  std::fill(button_press_time_, button_press_time_ + 4, 0);
  button_state_ = 0;
  button_ignore_mask_ = 0;
  screensaver_mode_ = SCREENSAVER_OFF;
  preempt_screensaver_ = false;

  encoder_right_.Init(OC_GPIO_ENC_PINMODE);
  encoder_left_.Init(OC_GPIO_ENC_PINMODE);

  event_queue_.Init();
}

void Ui::configure_encoders(EncoderConfig encoder_config) {
  SERIAL_PRINTLN("Configuring encoders: %s (%x)", OC::Strings::encoder_config_strings[encoder_config], encoder_config);

  encoder_right_.reverse(encoder_config & ENCODER_CONFIG_R_REVERSED);
  encoder_left_.reverse(encoder_config & ENCODER_CONFIG_L_REVERSED);
}

void Ui::set_screensaver_timeout(uint32_t seconds) {
  uint32_t timeout = seconds * 1000U;
  if (timeout < kLongPressTicks * 2)
    timeout = kLongPressTicks * 2;

  screensaver_timeout_ = timeout;
  SERIAL_PRINTLN("Set screensaver timeout to %lu", timeout);
  event_queue_.Poke();
}

void Ui::set_blanking_timeout(uint32_t minutes) {
  blanking_timeout_ = minutes * 60U * 1000U;
}

void FASTRUN Ui::_Poke() {
  event_queue_.Poke();
}

void Ui::_preemptScreensaver(bool v) {
  preempt_screensaver_ = v;
}

void FASTRUN Ui::Poll() {

  uint32_t now = ++ticks_;
  uint16_t button_state = 0;

  for (size_t i = 0; i < CONTROL_BUTTON_LAST; ++i) {
    if (buttons_[i].Poll())
      button_state |= control_mask(i);
  }

  for (size_t i = 0; i < CONTROL_BUTTON_LAST; ++i) {
    auto &button = buttons_[i];
    auto t = now - button_press_time_[i];
    if (button.just_pressed()) {
      button_press_time_[i] = now;
      PushEvent(UI::EVENT_BUTTON_DOWN, control_mask(i), 0, button_state, 0);
    } else if (button.released()) {
      if (t < kLongPressTicks)
        PushEvent(UI::EVENT_BUTTON_PRESS, control_mask(i), 0, button_state, t);
      button_press_time_[i] = 0;
    } else if (button.pressed() && (t == kLongPressTicks)) {
      button_state &= ~control_mask(i);
      PushEvent(UI::EVENT_BUTTON_LONG_PRESS, control_mask(i), 0, button_state, t);
    }
  }

  encoder_right_.Poll();
  encoder_left_.Poll();

  int32_t increment;
  increment = encoder_right_.Read();
  if (increment)
    PushEvent(UI::EVENT_ENCODER, CONTROL_ENCODER_R, increment, button_state, 0);

  increment = encoder_left_.Read();
  if (increment)
    PushEvent(UI::EVENT_ENCODER, CONTROL_ENCODER_L, increment, button_state, 0);

  button_state_ = button_state;
}

UiMode Ui::DispatchEvents(App *app) {

  while (event_queue_.available()) {
    const UI::Event event = event_queue_.PullEvent();
    if (IgnoreEvent(event))
      continue;

    switch (event.type) {
      case UI::EVENT_BUTTON_PRESS:
        app->HandleButtonEvent(event);
        break;
      case UI::EVENT_BUTTON_DOWN:
#ifdef VOR
    #ifdef VOR_NO_RANGE_BUTTON
        if (OC::CONTROL_BUTTON_UP == event.control) {
            VBiasManager *vbias_m = vbias_m->get();
            if (vbias_m->IsEditing()) vbias_m->AdvanceBias();
            else app->HandleButtonEvent(event);
        } else app->HandleButtonEvent(event);
    #else
        if (OC::CONTROL_BUTTON_M == event.control) {
            VBiasManager *vbias_m = vbias_m->get();
            vbias_m->AdvanceBias();
        } else app->HandleButtonEvent(event);
    #endif
#else
        app->HandleButtonEvent(event);
#endif
        break;
      case UI::EVENT_BUTTON_LONG_PRESS:
        if (OC::CONTROL_BUTTON_UP == event.control) {
        #ifdef VOR_NO_RANGE_BUTTON
            VBiasManager *vbias_m = vbias_m->get();
            vbias_m->AdvanceBias();
        #else
            if (!preempt_screensaver_) {
              SetButtonIgnoreMask();
              screensaver_mode_ = event.ticks > kXLongPressTicks
                ? SCREENSAVER_BLANKING
                : SCREENSAVER_ACTIVE;
            }
        #endif
        }
        else if (OC::CONTROL_BUTTON_R == event.control)
          return UI_MODE_APP_SETTINGS;
        else
          app->HandleButtonEvent(event);
        break;
      case UI::EVENT_ENCODER:
        app->HandleEncoderEvent(event);
        break;
      default:
        break;
    }
    MENU_REDRAW = 1;
  }

  auto screensaver_mode = screensaver_mode_;
  switch (screensaver_mode) {
    case SCREENSAVER_OFF:
    case SCREENSAVER_ACTIVE: {
      if (idle_time() > screensaver_timeout())
        screensaver_mode = SCREENSAVER_ACTIVE;
      if (blanking_timeout() && idle_time() > blanking_timeout()) 
        screensaver_mode = SCREENSAVER_BLANKING;
    } break;
    case SCREENSAVER_BLANKING: break;
  }
  if (screensaver_mode != screensaver_mode_) {
      SetButtonIgnoreMask();
      screensaver_mode_ = screensaver_mode;
  }

  if (screensaver_mode_)
    return UI_MODE_SCREENSAVER;
  else
    return UI_MODE_MENU;
}

UiMode Ui::Splashscreen(bool &reset_settings) {

  UiMode mode = UI_MODE_MENU;

  unsigned long start = millis();
  unsigned long now = start;
  do {

    mode = UI_MODE_MENU;
    if (read_immediate(CONTROL_BUTTON_L))
      mode = UI_MODE_CALIBRATE;
    if (read_immediate(CONTROL_BUTTON_R))
      mode = UI_MODE_APP_SETTINGS;

    reset_settings =
    #if defined(BUCHLA_4U) && !defined(IO_10V)
       read_immediate(CONTROL_BUTTON_UP) && read_immediate(CONTROL_BUTTON_R);
    #else
       read_immediate(CONTROL_BUTTON_UP) && read_immediate(CONTROL_BUTTON_DOWN);
    #endif

    now = millis();

    // This graphics frame is necessary for the keys to be read. I don't know why this is the case,
    // since the keys are read above, but I don't have time to figure that out right now. --jj
    GRAPHICS_BEGIN_FRAME(true);

    /* Splash Screen 
    menu::DefaultTitleBar::Draw();
    graphics.print(Strings::NAME);
    weegfx::coord_t y = menu::CalcLineY(0);

    graphics.setPrintPos(menu::kIndentDx, y + menu::kTextDy);
    graphics.print("[L] => Calibration");
    if (UI_MODE_CALIBRATE == mode)
      graphics.invertRect(menu::kIndentDx, y, 128, menu::kMenuLineH);

    y += menu::kMenuLineH;
    graphics.setPrintPos(menu::kIndentDx, y + menu::kTextDy);
    graphics.print("[R] => Select app");
    if (UI_MODE_APP_SETTINGS == mode)
      graphics.invertRect(menu::kIndentDx, y, 128, menu::kMenuLineH);

    y += menu::kMenuLineH;
    graphics.setPrintPos(menu::kIndentDx, y + menu::kTextDy);
    if (reset_settings)
      graphics.print("!! RESET EEPROM !!");

    y += menu::kMenuLineH;
    graphics.setPrintPos(menu::kIndentDx, y + menu::kTextDy);
    graphics.print(Strings::VERSION);

    weegfx::coord_t w;
    if (now - start < SPLASHSCREEN_DELAY_MS)
      w = 128;
    else
      w = ((start + SPLASHSCREEN_DELAY_MS + SPLASHSCREEN_TIMEOUT_MS - now) << 7) / SPLASHSCREEN_TIMEOUT_MS;
    graphics.drawRect(0, 62, w, 2);
    */

    /* fixes spurious button presses when booting ? */
    while (event_queue_.available())
      (void)event_queue_.PullEvent();

    GRAPHICS_END_FRAME();

  } while (now - start < SPLASHSCREEN_DELAY_MS);

  SetButtonIgnoreMask();
  return mode;
}

} // namespace OC
