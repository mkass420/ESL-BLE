#ifndef LED_PWM_H__
#define LED_PWM_H__

#include "led_state.h"

#include "sdk_errors.h"

ret_code_t led_pwm_init(void);
void       led_pwm_apply(led_state_t const* p_state);

#endif /* LED_PWM_H__ */
