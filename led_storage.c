#include "led_storage.h"

#include "app_error.h"
#include "app_util.h"
#include "nrf.h"
#include "nrf_fstorage.h"
#include "nrf_fstorage_sd.h"
#include "nrf_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LED_STORAGE_VERSION 1U
#define LED_STORAGE_MAGIC   0x3144454CU

extern uint32_t __etext;

typedef enum {
    LED_STORAGE_OP_IDLE,
    LED_STORAGE_OP_ERASE,
    LED_STORAGE_OP_WRITE,
} led_storage_op_t;

typedef struct {
    uint16_t version;
    uint16_t hue;
    uint8_t  saturation;
    uint8_t  value;
    uint8_t  power_on;
    uint8_t  reserved;
    uint32_t magic;
} led_storage_record_t;

STATIC_ASSERT((sizeof(led_storage_record_t) % sizeof(uint32_t)) == 0U,
              "Storage record size must be word-aligned");

static void led_storage_evt_handler(nrf_fstorage_evt_t* p_evt);

NRF_FSTORAGE_DEF(nrf_fstorage_t m_led_storage_fs) =
    {
        .evt_handler = led_storage_evt_handler,
};

static led_state_t          m_saved_state;
static led_state_t          m_pending_state;
static led_state_t          m_inflight_state;
static led_storage_record_t m_record_buffer;
static uint32_t             m_storage_start_addr;
static uint32_t             m_storage_end_addr;
static uint32_t             m_next_write_addr;
static bool                 m_saved_state_valid;
static bool                 m_pending_state_valid;
static led_storage_op_t     m_active_op = LED_STORAGE_OP_IDLE;

static uint32_t led_storage_page_size(void) {
    return NRF_FICR->CODEPAGESIZE;
}

static uint32_t led_storage_flash_end_addr(void) {
    return NRF_FICR->CODESIZE * NRF_FICR->CODEPAGESIZE;
}

static uint32_t led_storage_upper_bound_addr(void) {
    uint32_t upper_bound = led_storage_flash_end_addr();

    if((BOOTLOADER_ADDRESS != 0xFFFFFFFFU) && (BOOTLOADER_ADDRESS < upper_bound)) {
        upper_bound = BOOTLOADER_ADDRESS;
    }

    if((MBR_PARAMS_PAGE_ADDRESS != 0xFFFFFFFFU) && (MBR_PARAMS_PAGE_ADDRESS < upper_bound)) {
        upper_bound = MBR_PARAMS_PAGE_ADDRESS;
    }

    return upper_bound;
}

static uint32_t led_storage_page_start_addr(void) {
    return led_storage_upper_bound_addr() - led_storage_page_size();
}

static uint32_t led_storage_page_end_addr(void) {
    return led_storage_page_start_addr() + led_storage_page_size() - 1U;
}

static ret_code_t led_storage_bounds_validate(void) {
    uint32_t const page_size      = led_storage_page_size();
    uint32_t const storage_limit  = led_storage_upper_bound_addr();
    uint32_t const app_flash_end  = ((uint32_t)&__etext + page_size - 1U) & ~(page_size - 1U);

    if(storage_limit < page_size) {
        return NRF_ERROR_INVALID_ADDR;
    }

    if(app_flash_end > led_storage_page_start_addr()) {
        return NRF_ERROR_NO_MEM;
    }

    return NRF_SUCCESS;
}

static bool led_storage_record_is_empty(led_storage_record_t const* p_record) {
    uint8_t const* p_bytes = (uint8_t const*)p_record;

    for(size_t i = 0; i < sizeof(*p_record); ++i) {
        if(p_bytes[i] != 0xFFU) {
            return false;
        }
    }

    return true;
}

static bool led_storage_record_is_valid(led_storage_record_t const* p_record) {
    led_state_t const state =
        {
            .power_on   = (p_record->power_on != 0U),
            .hue        = p_record->hue,
            .saturation = p_record->saturation,
            .value      = p_record->value,
        };

    return (p_record->version == LED_STORAGE_VERSION) &&
           (p_record->magic == LED_STORAGE_MAGIC) &&
           ((p_record->power_on == 0U) || (p_record->power_on == 1U)) &&
           led_state_is_valid(&state);
}

static led_storage_record_t led_storage_record_from_state(led_state_t const* p_state) {
    return (led_storage_record_t){
        .version    = LED_STORAGE_VERSION,
        .hue        = p_state->hue,
        .power_on   = p_state->power_on ? 1U : 0U,
        .saturation = p_state->saturation,
        .value      = p_state->value,
        .reserved   = 0U,
        .magic      = LED_STORAGE_MAGIC,
    };
}

static led_state_t led_storage_state_from_record(led_storage_record_t const* p_record) {
    return (led_state_t){
        .power_on   = (p_record->power_on != 0U),
        .hue        = p_record->hue,
        .saturation = p_record->saturation,
        .value      = p_record->value,
    };
}

static bool led_storage_page_has_free_slot(void) {
    return (m_next_write_addr + sizeof(led_storage_record_t)) <= (m_storage_end_addr + 1U);
}

static ret_code_t led_storage_start_next_operation(void) {
    if(!m_pending_state_valid) {
        return NRF_SUCCESS;
    }

    if(m_saved_state_valid && led_state_equals(&m_pending_state, &m_saved_state)) {
        m_pending_state_valid = false;
        return NRF_SUCCESS;
    }

    if(!led_storage_page_has_free_slot()) {
        ret_code_t err_code = nrf_fstorage_erase(&m_led_storage_fs, m_storage_start_addr, 1U, NULL);
        if(err_code == NRF_SUCCESS) {
            m_active_op = LED_STORAGE_OP_ERASE;
        }
        return err_code;
    }

    m_inflight_state      = m_pending_state;
    m_pending_state_valid = false;
    m_record_buffer       = led_storage_record_from_state(&m_inflight_state);

    ret_code_t err_code = nrf_fstorage_write(&m_led_storage_fs,
                                             m_next_write_addr,
                                             &m_record_buffer,
                                             sizeof(m_record_buffer),
                                             NULL);
    if(err_code == NRF_SUCCESS) {
        m_active_op = LED_STORAGE_OP_WRITE;
    }

    return err_code;
}

static void led_storage_evt_handler(nrf_fstorage_evt_t* p_evt) {
    if(p_evt->result != NRF_SUCCESS) {
        NRF_LOG_ERROR("LED storage operation failed: event=%u error=0x%08x",
                      (unsigned int)p_evt->id,
                      (unsigned int)p_evt->result);
        m_active_op = LED_STORAGE_OP_IDLE;
        return;
    }

    switch(p_evt->id) {
        case NRF_FSTORAGE_EVT_ERASE_RESULT:
            m_saved_state_valid = false;
            m_next_write_addr   = m_storage_start_addr;
            m_active_op         = LED_STORAGE_OP_IDLE;
            APP_ERROR_CHECK(led_storage_start_next_operation());
            break;

        case NRF_FSTORAGE_EVT_WRITE_RESULT:
            m_saved_state       = m_inflight_state;
            m_saved_state_valid = true;
            m_next_write_addr += sizeof(led_storage_record_t);
            m_active_op = LED_STORAGE_OP_IDLE;
            APP_ERROR_CHECK(led_storage_start_next_operation());
            break;

        default:
            break;
    }
}

ret_code_t led_storage_init(led_state_t* p_state) {
    if(p_state == NULL) {
        return NRF_ERROR_NULL;
    }

    ret_code_t err_code = led_storage_bounds_validate();
    if(err_code != NRF_SUCCESS) {
        return err_code;
    }

    m_storage_start_addr  = led_storage_page_start_addr();
    m_storage_end_addr    = led_storage_page_end_addr();
    m_next_write_addr     = m_storage_start_addr;
    m_saved_state_valid   = false;
    m_pending_state_valid = false;
    m_active_op           = LED_STORAGE_OP_IDLE;

    m_led_storage_fs.start_addr = m_storage_start_addr;
    m_led_storage_fs.end_addr   = m_storage_end_addr;

    NRF_LOG_INFO("LED storage page: 0x%08x-0x%08x (bootloader=0x%08x, mbr_params=0x%08x)",
                 (unsigned int)m_storage_start_addr,
                 (unsigned int)m_storage_end_addr,
                 (unsigned int)BOOTLOADER_ADDRESS,
                 (unsigned int)MBR_PARAMS_PAGE_ADDRESS);

    err_code = nrf_fstorage_init(&m_led_storage_fs, &nrf_fstorage_sd, NULL);
    if(err_code != NRF_SUCCESS) {
        return err_code;
    }

    for(uint32_t addr = m_storage_start_addr;
        (addr + sizeof(led_storage_record_t)) <= (m_storage_end_addr + 1U);
        addr += sizeof(led_storage_record_t)) {
        led_storage_record_t record;
        memcpy(&record, (void const*)addr, sizeof(record));

        if(led_storage_record_is_empty(&record)) {
            m_next_write_addr = addr;
            break;
        }

        if(!led_storage_record_is_valid(&record)) {
            m_next_write_addr = m_storage_end_addr + 1U;
            break;
        }

        m_saved_state       = led_storage_state_from_record(&record);
        m_saved_state_valid = true;
        m_next_write_addr   = addr + sizeof(led_storage_record_t);
    }

    if(m_saved_state_valid) {
        *p_state = m_saved_state;
        NRF_LOG_INFO("Recovered LED state from flash: on=%u hue=%u sat=%u val=%u",
                     (unsigned int)p_state->power_on,
                     (unsigned int)p_state->hue,
                     (unsigned int)p_state->saturation,
                     (unsigned int)p_state->value);
    }
    else {
        NRF_LOG_INFO("No valid LED state found in flash, using defaults");
    }

    return NRF_SUCCESS;
}

ret_code_t led_storage_schedule_save(led_state_t const* p_state) {
    if(p_state == NULL) {
        return NRF_ERROR_NULL;
    }

    if(!led_state_is_valid(p_state)) {
        return NRF_ERROR_INVALID_PARAM;
    }

    if(m_saved_state_valid && led_state_equals(p_state, &m_saved_state) &&
       !m_pending_state_valid && (m_active_op == LED_STORAGE_OP_IDLE)) {
        return NRF_SUCCESS;
    }

    m_pending_state       = *p_state;
    m_pending_state_valid = true;

    if(m_active_op != LED_STORAGE_OP_IDLE) {
        return NRF_SUCCESS;
    }

    return led_storage_start_next_operation();
}
