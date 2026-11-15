// Native ADC reads for the Lilac diagnostic target (image 9). The six
// channels and pads of stock ec-lilac laptop13/adc.dtsi (identical in the
// release azalea/adc.dtsi), configured and converted like the stock shim
// (zephyr/shim/src/adc.c): gain 1, internal reference, default acquisition
// time, 10-bit resolution, no oversampling, millivolts with mul/div 1.
// Read-only: Python gets millivolts by channel name, nothing else.
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "lilac_diag.h"

LOG_MODULE_REGISTER(lilac_adc, LOG_LEVEL_INF);

static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc0));

#define ADC_RESOLUTION 10  // stock CONFIG_PLATFORM_EC_ADC_RESOLUTION default

static const struct lilac_adc_channel channels[] = {
    { "main_board_id", 0 },   // ADC_MAIN_BOARD_ID, GP45
    { "psys", 4 },            // ADC_PSYS, GP41
    { "adapter_current", 5 }, // ADC_ADAPTER_CURRENT, GP37
    { "tp_board_id", 7 },     // ADC_TOUCHPAD_ID, GPE1 (stock pulses tp_board_id_pug)
    { "audio_board_id", 8 },  // ADC_AUDIO_ID, GPF1
    { "batt_temp", 9 },       // ADC_VCIN1_BATT_TEMP, GPF0
};

static int setup_result = -EAGAIN;
static K_MUTEX_DEFINE(adc_lock);

const struct lilac_adc_channel *lilac_adc_channels(size_t *count) {
    *count = ARRAY_SIZE(channels);
    return channels;
}

static int lilac_adc_init(void) {
    if (!device_is_ready(adc_dev)) {
        setup_result = -ENODEV;
        LOG_ERR("ADC not ready");
        return 0;
    }
    for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
        struct adc_channel_cfg cfg = {
            .gain = ADC_GAIN_1,
            .reference = ADC_REF_INTERNAL,
            .acquisition_time = ADC_ACQ_TIME_DEFAULT,
            .channel_id = channels[i].input,
        };
        int ret = adc_channel_setup(adc_dev, &cfg);
        if (ret) {
            setup_result = ret;
            LOG_ERR("ADC channel %u setup failed: %d", channels[i].input, ret);
            return 0;
        }
    }
    setup_result = 0;
    return 0;
}
SYS_INIT(lilac_adc_init, APPLICATION, 1);

int lilac_adc_read(size_t index, int32_t *mv) {
    if (index >= ARRAY_SIZE(channels)) {
        return -EINVAL;
    }
    if (setup_result) {
        return setup_result;
    }
    int32_t raw = 0;
    struct adc_sequence seq = {
        .channels = BIT(channels[index].input),
        .buffer = &raw,
        .buffer_size = sizeof(raw),
        .resolution = ADC_RESOLUTION,
    };
    k_mutex_lock(&adc_lock, K_FOREVER);
    int ret = adc_read(adc_dev, &seq);
    k_mutex_unlock(&adc_lock);
    if (ret) {
        return ret;
    }
    ret = adc_raw_to_millivolts(adc_ref_internal(adc_dev), ADC_GAIN_1, ADC_RESOLUTION, &raw);
    if (ret) {
        return ret;
    }
    *mv = raw;
    return 0;
}
