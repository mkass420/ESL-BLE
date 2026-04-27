#ifndef LED_STATE_H__
#define LED_STATE_H__

#include <stdbool.h>
#include <stdint.h>

#define LED_DEVICE_ID          6593U
#define LED_DEFAULT_HUE        (((LED_DEVICE_ID % 100U) * 360U) / 100U)
#define LED_DEFAULT_SATURATION 100U
#define LED_DEFAULT_VALUE      100U

typedef struct {
    float r;
    float g;
    float b;
} led_rgb_t;

typedef struct {
    bool     power_on;
    uint16_t hue;
    uint8_t  saturation;
    uint8_t  value;
} led_state_t;

void led_state_init_defaults(led_state_t* p_state);

bool led_state_is_valid(led_state_t const* p_state);
bool led_state_equals(led_state_t const* p_lhs, led_state_t const* p_rhs);

bool led_state_is_valid_hue(uint16_t hue);
bool led_state_is_valid_saturation(uint8_t saturation);
bool led_state_is_valid_value(uint8_t value);

led_rgb_t led_state_hsv_to_rgb(led_state_t const* p_state);

#endif /* LED_STATE_H__ */
