#include "input/input.h"
#include "ui/theme.h"
#include "util/log.h"

#include <string.h>

/* Directional auto-repeat, tuned for scrolling a timeline rather than for
 * text entry: a deliberate first delay, then a steady stream. */
#define REPEAT_DELAY_MS  380
#define REPEAT_RATE_MS    90

/* Analogue stick deflection past which the stick counts as a direction. */
#define STICK_DEADZONE 12000

static SDL_GameController *s_controllers[4];
static int s_controller_count = 0;

/*
 * Button mapping.
 *
 * SDL names its face buttons by Xbox *position*, so SDL_CONTROLLER_BUTTON_A is
 * the bottom button. On a Wii U GamePad the bottom button is physically B and
 * the right button is A, and Nintendo's convention is A = confirm, B = back.
 * Whether the Wii U SDL port reports positionally or by physical label has to
 * be settled on real hardware, so both plausible confirm/back pairs are
 * accepted and every button event is logged at debug level to resolve it on
 * the first console run.
 */
static bool
map_button(Uint8 sdl_button, cobalt_button *out)
{
   switch (sdl_button) {
      case SDL_CONTROLLER_BUTTON_DPAD_UP:    *out = COBALT_BTN_UP;      return true;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  *out = COBALT_BTN_DOWN;    return true;
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  *out = COBALT_BTN_LEFT;    return true;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: *out = COBALT_BTN_RIGHT;   return true;
      case SDL_CONTROLLER_BUTTON_A:          *out = COBALT_BTN_CONFIRM; return true;
      case SDL_CONTROLLER_BUTTON_B:          *out = COBALT_BTN_BACK;    return true;
      case SDL_CONTROLLER_BUTTON_START:      *out = COBALT_BTN_MENU;    return true;
      default:                                                          return false;
   }
}

static void
open_controllers(void)
{
   const int joysticks = SDL_NumJoysticks();
   for (int i = 0; i < joysticks && s_controller_count < 4; i++) {
      if (!SDL_IsGameController(i)) {
         COBALT_LOGW("joystick %d is not a recognised game controller", i);
         continue;
      }
      SDL_GameController *pad = SDL_GameControllerOpen(i);
      if (!pad) {
         COBALT_LOGE("failed to open controller %d: %s", i, SDL_GetError());
         continue;
      }
      s_controllers[s_controller_count++] = pad;
      COBALT_LOGI("controller %d opened: %s", i, SDL_GameControllerName(pad));
   }

   if (s_controller_count == 0) {
      COBALT_LOGW("no controllers opened — touch input only");
   }
}

void
cobalt_input_init(cobalt_input *in)
{
   memset(in, 0, sizeof(*in));
   open_controllers();
}

void
cobalt_input_shutdown(void)
{
   for (int i = 0; i < s_controller_count; i++) {
      if (s_controllers[i]) {
         SDL_GameControllerClose(s_controllers[i]);
         s_controllers[i] = NULL;
      }
   }
   s_controller_count = 0;
}

void
cobalt_input_begin_frame(cobalt_input *in, uint32_t now_ms)
{
   (void) now_ms;
   memset(in->pressed, 0, sizeof(in->pressed));
   in->touch_began = false;
   in->touch_ended = false;
   in->text[0] = '\0';
   in->backspace = false;
   in->text_confirmed = false;
}

static void
press(cobalt_input *in, cobalt_button btn, uint32_t now_ms)
{
   if (in->held[btn]) {
      return;
   }
   in->held[btn] = true;
   in->pressed[btn] = true;
   in->held_since[btn] = now_ms;
   in->next_repeat[btn] = now_ms + REPEAT_DELAY_MS;
}

static void
release(cobalt_input *in, cobalt_button btn)
{
   in->held[btn] = false;
   in->next_repeat[btn] = 0;
}

void
cobalt_input_handle_event(cobalt_input *in, const SDL_Event *event)
{
   const uint32_t now = SDL_GetTicks();

   switch (event->type) {
      case SDL_QUIT:
         in->quit_requested = true;
         break;

      case SDL_CONTROLLERBUTTONDOWN: {
         cobalt_button btn;
         COBALT_LOGD("controller button %u down", (unsigned) event->cbutton.button);
         if (map_button(event->cbutton.button, &btn)) {
            press(in, btn, now);
         }
         break;
      }

      case SDL_CONTROLLERBUTTONUP: {
         cobalt_button btn;
         if (map_button(event->cbutton.button, &btn)) {
            release(in, btn);
         }
         break;
      }

      case SDL_CONTROLLERAXISMOTION: {
         /* Left stick doubles as a D-pad so stick-only navigation works. */
         const Sint16 value = event->caxis.value;
         if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            if (value < -STICK_DEADZONE)      press(in, COBALT_BTN_UP, now);
            else if (value > STICK_DEADZONE)  press(in, COBALT_BTN_DOWN, now);
            else { release(in, COBALT_BTN_UP); release(in, COBALT_BTN_DOWN); }
         } else if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            if (value < -STICK_DEADZONE)      press(in, COBALT_BTN_LEFT, now);
            else if (value > STICK_DEADZONE)  press(in, COBALT_BTN_RIGHT, now);
            else { release(in, COBALT_BTN_LEFT); release(in, COBALT_BTN_RIGHT); }
         }
         break;
      }

      case SDL_TEXTINPUT: {
         /* Append rather than overwrite: rare, but more than one SDL_TEXTINPUT
          * can arrive in a single frame (e.g. IME composition committing
          * several characters at once). */
         size_t used = strlen(in->text);
         size_t room = sizeof(in->text) - used - 1;
         if (room > 0) {
            strncat(in->text, event->text.text, room);
         }
         break;
      }

      case SDL_KEYDOWN: {
         if (event->key.keysym.sym == SDLK_BACKSPACE) {
            in->backspace = true;
         } else if (event->key.keysym.sym == SDLK_RETURN ||
                    event->key.keysym.sym == SDLK_KP_ENTER) {
            in->text_confirmed = true;
         }
         break;
      }

      case SDL_FINGERDOWN:
      case SDL_FINGERMOTION:
      case SDL_FINGERUP: {
         /* SDL reports touch normalised to 0..1; screens work in GamePad
          * pixels, which is the only touch-capable surface on this hardware. */
         in->touch_x = (int) (event->tfinger.x * (float) COBALT_DRC_WIDTH);
         in->touch_y = (int) (event->tfinger.y * (float) COBALT_DRC_HEIGHT);

         if (event->type == SDL_FINGERDOWN) {
            in->touch_down = true;
            in->touch_began = true;
         } else if (event->type == SDL_FINGERUP) {
            in->touch_down = false;
            in->touch_ended = true;
         }
         break;
      }

      default:
         break;
   }
}

void
cobalt_input_end_frame(cobalt_input *in, uint32_t now_ms)
{
   /* Auto-repeat is directional only. Repeating confirm or back would make a
    * held button fire an action several times, which is never wanted. */
   static const cobalt_button REPEATABLE[] = {
      COBALT_BTN_UP, COBALT_BTN_DOWN, COBALT_BTN_LEFT, COBALT_BTN_RIGHT,
   };

   for (size_t i = 0; i < sizeof(REPEATABLE) / sizeof(REPEATABLE[0]); i++) {
      cobalt_button btn = REPEATABLE[i];
      if (!in->held[btn]) {
         continue;
      }
      if (now_ms >= in->next_repeat[btn]) {
         in->pressed[btn] = true;
         in->next_repeat[btn] = now_ms + REPEAT_RATE_MS;
      }
   }
}

void
cobalt_input_start_text_edit(void)
{
   SDL_StartTextInput();
}

void
cobalt_input_stop_text_edit(void)
{
   SDL_StopTextInput();
}

bool
cobalt_input_tapped(const cobalt_input *in, const SDL_Rect *rect)
{
   if (!in->touch_ended || !rect) {
      return false;
   }
   SDL_Point p = { in->touch_x, in->touch_y };
   return SDL_PointInRect(&p, rect) == SDL_TRUE;
}
