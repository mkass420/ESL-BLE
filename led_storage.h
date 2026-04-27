#ifndef LED_STORAGE_H__
#define LED_STORAGE_H__

#include "led_state.h"

#include "sdk_errors.h"

ret_code_t led_storage_init(led_state_t * p_state);
ret_code_t led_storage_schedule_save(led_state_t const * p_state);

#endif /* LED_STORAGE_H__ */
