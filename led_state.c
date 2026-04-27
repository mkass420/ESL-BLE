#include "led_state.h"

#include <math.h>
#include <stddef.h>

void led_state_init_defaults(led_state_t* p_state) {
    if(p_state == NULL) {
        return;
    }

    p_state->power_on   = true;
    p_state->hue        = LED_DEFAULT_HUE;
    p_state->saturation = LED_DEFAULT_SATURATION;
    p_state->value      = LED_DEFAULT_VALUE;
}

bool led_state_is_valid_hue(uint16_t hue) {
    return hue <= 360U;
}

bool led_state_is_valid_saturation(uint8_t saturation) {
    return saturation <= 100U;
}

bool led_state_is_valid_value(uint8_t value) {
    return value <= 100U;
}

bool led_state_is_valid(led_state_t const* p_state) {
    if(p_state == NULL) {
        return false;
    }

    return led_state_is_valid_hue(p_state->hue) &&
           led_state_is_valid_saturation(p_state->saturation) &&
           led_state_is_valid_value(p_state->value);
}

bool led_state_equals(led_state_t const* p_lhs, led_state_t const* p_rhs) {
    if((p_lhs == NULL) || (p_rhs == NULL)) {
        return false;
    }

    return (p_lhs->power_on == p_rhs->power_on) &&
           (p_lhs->hue == p_rhs->hue) &&
           (p_lhs->saturation == p_rhs->saturation) &&
           (p_lhs->value == p_rhs->value);
}

led_rgb_t led_state_hsv_to_rgb(led_state_t const* p_state) {
    if((p_state == NULL) || !p_state->power_on) {
        return (led_rgb_t){0.0f, 0.0f, 0.0f};
    }

    float const h = p_state->hue / 360.0f;
    float const s = p_state->saturation / 100.0f;
    float const v = p_state->value / 100.0f;

    if(p_state->saturation == 0U) {
        return (led_rgb_t){v, v, v};
    }

    int   sector = (int)(h * 6.0f);
    float f      = (h * 6.0f) - sector;
    float p      = v * (1.0f - s);
    float q      = v * (1.0f - (s * f));
    float t      = v * (1.0f - (s * (1.0f - f)));

    switch(sector % 6) {
        case 0: return (led_rgb_t){v, t, p};
        case 1: return (led_rgb_t){q, v, p};
        case 2: return (led_rgb_t){p, v, t};
        case 3: return (led_rgb_t){p, q, v};
        case 4: return (led_rgb_t){t, p, v};
        case 5: return (led_rgb_t){v, p, q};
        default: return (led_rgb_t){0.0f, 0.0f, 0.0f};
    }
}
