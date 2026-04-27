#include "led_pwm.h"

#include "boards.h"
#include "nrf_pwm.h"
#include "nrfx_pwm.h"

#include <stdbool.h>

static nrfx_pwm_t m_rgb_pwm = NRFX_PWM_INSTANCE(0);
static bool       m_initialized;

static nrf_pwm_values_individual_t m_pwm_values;
static nrf_pwm_sequence_t          m_pwm_sequence =
    {
        .values.p_individual = &m_pwm_values,
        .length              = NRF_PWM_VALUES_LENGTH(m_pwm_values),
        .repeats             = 0,
        .end_delay           = 0,
};

static uint16_t duty_from_unit_interval(float value) {
    if(value <= 0.0f) {
        return 0U;
    }

    if(value >= 1.0f) {
        return 1000U;
    }

    return (uint16_t)((value * 1000.0f) + 0.5f);
}

ret_code_t led_pwm_init(void) {
    if(m_initialized) {
        return NRF_SUCCESS;
    }

    nrfx_pwm_config_t config = NRFX_PWM_DEFAULT_CONFIG;

    config.output_pins[0] = LED2_R | NRFX_PWM_PIN_INVERTED;
    config.output_pins[1] = LED2_G | NRFX_PWM_PIN_INVERTED;
    config.output_pins[2] = LED2_B | NRFX_PWM_PIN_INVERTED;
    config.output_pins[3] = NRFX_PWM_PIN_NOT_USED;
    config.load_mode      = NRF_PWM_LOAD_INDIVIDUAL;

    ret_code_t err_code = nrfx_pwm_init(&m_rgb_pwm, &config, NULL);
    if(err_code != NRF_SUCCESS) {
        return err_code;
    }

    m_pwm_values.channel_0 = 0U;
    m_pwm_values.channel_1 = 0U;
    m_pwm_values.channel_2 = 0U;
    m_pwm_values.channel_3 = 0U;

    (void)nrfx_pwm_simple_playback(&m_rgb_pwm, &m_pwm_sequence, 1, NRFX_PWM_FLAG_LOOP);

    m_initialized = true;
    return NRF_SUCCESS;
}

void led_pwm_apply(led_state_t const* p_state) {
    if(!m_initialized || (p_state == NULL)) {
        return;
    }

    led_rgb_t const rgb = led_state_hsv_to_rgb(p_state);

    m_pwm_values.channel_0 = duty_from_unit_interval(rgb.r);
    m_pwm_values.channel_1 = duty_from_unit_interval(rgb.g);
    m_pwm_values.channel_2 = duty_from_unit_interval(rgb.b);
    m_pwm_values.channel_3 = 0U;
}
