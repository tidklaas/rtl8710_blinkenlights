/**
 * RTL8710 Blinkenlights. WiFi-controlled WS2812B LED strip.
 * Copyright (C) 2016  Tido Klaassen <tido@4gh.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "dlist.h"
#include <autoconf.h>
#include <spi_api.h>
#include <spi_ex_api.h>
#include <flash_api.h>
#include <platform_opts.h>
#include <platform_stdlib.h>
#include "device_lock.h"
#include "ws2812.h"
#include "blinken.h"

enum strip_state
{

    state_rainbow,
    state_flicker,
    state_eye,
};

struct blinken_cfg strip_cfg;
volatile unsigned int cfg_updated = 0;
ws2812_t *ws2812_cfg = NULL;

hsvValue_t hsv_buffer[BLINKEN_MAX_LEDS];

struct strip_handler
{
    enum strip_state state;
    struct list_head filters;
    hsvValue_t *hsv_vals;
    volatile size_t strip_len;
    volatile uint32_t brightness;
    volatile uint32_t delay;
};

struct led_filter;
typedef void (*filter_fn)(struct led_filter *, enum strip_state *,
              hsvValue_t[], unsigned int);
typedef int (*init_fn)(struct led_filter *, struct blinken_cfg *, bool);
typedef void (*deinit_fn)(struct led_filter *);

struct led_filter
{
    char *name;
    struct list_head filters;
    filter_fn filter;
    init_fn init;
    deinit_fn deinit;
    void *priv;
};

struct strip_handler handler;

SemaphoreHandle_t cfg_sema = NULL;
volatile uint8_t ledstrip_terminate = 0;

static uint64_t state0 = 1;
static uint64_t state1 = 2;

unsigned int urand(void)
{
    uint64_t s1 = state0;
    uint64_t s0 = state1;

    state0 = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    state1 = s1;

    return (unsigned int) (state0 + state1);
}

static void filter_deinit(struct led_filter *filter)
{
    void *priv;

    if(filter->priv != NULL){
        priv = filter->priv;
        filter->priv = NULL;

        free(priv);
    }
    filter->filter = NULL;
    filter->name = NULL;
    filter->init = NULL;
}

#define DEF_STRIP_LEN       250
#define MAX_STRIP_LEN       500
#define MAX_STRIP_BRIGHT    255
#define MAX_STRIP_DELAY     100
static int init_handler(struct strip_handler *this,
                        struct blinken_cfg *cfg,
                        struct ws2812_t *ws2812,
                        bool update)
{
    struct led_filter *filter;
    int result;

    result = 0;
    if(cfg->magic != BLINKEN_CFG_MAGIC){
        memset(cfg, 0xff, sizeof(cfg));
        cfg->magic = BLINKEN_CFG_MAGIC;
        cfg->version = BLINKEN_CFG_VER;
        cfg->strip_len = DEF_STRIP_LEN;
        cfg->delay = 0;
        cfg->brightness = MAX_STRIP_BRIGHT;

        cfg_updated = 1;
    }

    if(cfg->strip_len > MAX_STRIP_LEN){
        cfg->strip_len = MAX_STRIP_LEN;
        cfg_updated = 1;
    }

    if(cfg->delay > MAX_STRIP_DELAY){
        cfg->delay = MAX_STRIP_DELAY;
        cfg_updated = 1;
    }

    if(cfg->brightness > MAX_STRIP_BRIGHT){
        cfg->brightness = MAX_STRIP_BRIGHT;
        cfg_updated = 1;
    }
    
    if(!update){
        INIT_LIST_HEAD(&this->filters);
        this->state = state_rainbow;
    }
            
    this->strip_len = cfg->strip_len;
    this->delay = cfg->delay;
    this->brightness = cfg->brightness;
    this->hsv_vals = hsv_buffer;

    ws2812_set_len(ws2812, this->strip_len);

    if(update){
        list_for_each_entry(filter, &this->filters, filters, struct led_filter){
            result = filter->init(filter, cfg, true);
            if(result != 0){
                printf("[%s] updating filter %s failed.\n",
                        __func__,
                        filter->name != NULL ? filter->name : "unknown");
                goto err_out;
            }
        }
    }

err_out:
    return result;
}

struct ctx_rainbow
{
    int32_t hue_min;
    int32_t hue_max;
    int32_t hue_step;
    int32_t cycle_step;
    int32_t curr_hue;
};

void filter_rainbow(struct led_filter *this,
                    enum strip_state *state,
                    hsvValue_t hsv_vals[],
                    unsigned int strip_len)
{
    int i;
    hsvValue_t tmp_hsv;
    uint32_t tmp_hue;
    struct ctx_rainbow *ctx;

    ctx = (struct ctx_rainbow *) this->priv;

    tmp_hue = ctx->curr_hue;
    tmp_hsv.saturation = 255u;
    tmp_hsv.value = 255u;

    for(i = 0;i < strip_len; ++i){
        tmp_hsv.hue = scale_down(tmp_hue);
        hsv_vals[i] = tmp_hsv;

        tmp_hue += ctx->hue_step;
        if(ctx->hue_min == 0 && ctx->hue_max == scale_up(255u)){
            tmp_hue %= scale_up(256);
        } else {
            if(tmp_hue > ctx->hue_max){
                tmp_hue = ctx->hue_max;
                ctx->hue_step = -ctx->hue_step;
            }else if(tmp_hue < ctx->hue_min){
                tmp_hue = ctx->hue_min;
                ctx->hue_step = -ctx->hue_step;
            }
        }
    }

    ctx->curr_hue += ctx->cycle_step;
    if(ctx->hue_min == 0 && ctx->hue_max == scale_up(255u)){
        ctx->curr_hue %= scale_up(256);
    } else {
        if(ctx->curr_hue > ctx->hue_max){
            ctx->curr_hue = ctx->hue_max;
            ctx->cycle_step = -ctx->cycle_step;
        }else if(ctx->curr_hue < ctx->hue_min){
            ctx->curr_hue = ctx->hue_min;
            ctx->cycle_step = -ctx->cycle_step;
        }
    }
}

int init_rainbow(struct led_filter *this, struct blinken_cfg *cfg, bool update)
{
    int result;
    struct ctx_rainbow *ctx;

    result = 0;

    if(update){
        ctx = (struct ctx_rainbow *) this->priv;
    } else {
        this->name = "rainbow";
        this->filter = filter_rainbow;
        this->init = init_rainbow;
        this->deinit = filter_deinit;
        INIT_LIST_HEAD(&(this->filters));
        
        ctx = malloc(sizeof(*ctx));
        if(ctx == NULL){
            printf("[%s] malloc() failed\n", __func__);
            result = -1;
            goto err_out;
        }

        memset(ctx, 0x0, sizeof(*ctx));
        this->priv = ctx;
    }


    if(cfg->rainbow.valid == ~0x0){
        cfg->rainbow.valid = 0;
        cfg->rainbow.hue_min = 0u;
        cfg->rainbow.hue_max = 255u;
        cfg->rainbow.hue_steps = 255u;
        cfg->rainbow.cycle_steps = 255u;

        cfg_updated = 1;
    }

    if(cfg->rainbow.hue_max <= cfg->rainbow.hue_min){
        cfg->rainbow.hue_max = cfg->rainbow.hue_min;
        ctx->hue_step = 0u;
        ctx->cycle_step = 0u;
        cfg_updated = 1;
    }

    ctx->hue_min = scale_up(cfg->rainbow.hue_min);
    ctx->hue_max = scale_up(cfg->rainbow.hue_max);

    if(ctx->hue_min != ctx->hue_max){
        if(cfg->rainbow.hue_steps > 0){
            ctx->hue_step = (ctx->hue_max - ctx->hue_min)
                                    / cfg->rainbow.hue_steps;
        }
        if(cfg->rainbow.cycle_steps > 0){
            ctx->cycle_step = (ctx->hue_max - ctx->hue_min)
                                    / cfg->rainbow.cycle_steps;
        }
    }

    ctx->curr_hue = min(ctx->curr_hue, ctx->hue_max);
    ctx->curr_hue = max(ctx->curr_hue, ctx->hue_min);

err_out:
    return result;
}

struct ctx_fade
{
    int32_t min;
    int32_t max;
    int32_t curr_val;
    int32_t curr_step;
};


void filter_fade(struct led_filter *this,
                 enum strip_state *state,
                 hsvValue_t hsv_vals[],
                 unsigned int strip_len)
{
    int i;
    struct ctx_fade *ctx;

    ctx = (struct ctx_fade *) this->priv;

    for(i = 0; i < strip_len; ++i){
        hsv_vals[i].value = scale_down(ctx->curr_val);
    }

    if(ctx->curr_step == 0){
        return;
    }

    ctx->curr_val += ctx->curr_step;

    if(ctx->curr_val <= ctx->min){
        ctx->curr_step = -ctx->curr_step;
        ctx->curr_val = ctx->min;
    } else if(ctx->curr_val >= ctx->max){
        ctx->curr_step =  -ctx->curr_step;
        ctx->curr_val = ctx->max;
    }
}

int init_fade(struct led_filter *this, struct blinken_cfg *cfg, bool update)
{
    int result;
    struct ctx_fade *ctx;

    result = 0;

    if(update){
        ctx = (struct ctx_fade *) this->priv;
    } else {
        this->name = "fade";
        this->priv = ctx;
        this->filter = filter_fade;
        this->init = init_fade;
        this->deinit = filter_deinit;
        INIT_LIST_HEAD(&(this->filters));
        
        ctx = malloc(sizeof(*ctx));
        if(ctx == NULL){
            printf("[%s] malloc() failed\n", __func__);
            result = -1;
            goto err_out;
        }

        memset(ctx, 0x0, sizeof(*ctx));
        this->priv = ctx;
    }

    if(cfg->fade.valid == ~0x0){
        cfg->fade.valid = 0;
        cfg->fade.min = 0u;
        cfg->fade.max = 255u;
        cfg->fade.steps = BLINKEN_MAX_STEPS;
        cfg_updated = 1;
    }

    ctx->min = scale_up(cfg->fade.min);
    ctx->max = scale_up(cfg->fade.max);

    if(cfg->fade.min >= cfg->fade.max){
        cfg->fade.max = cfg->fade.min;
        cfg_updated = 1;
    } else {
        if(cfg->fade.steps > 0){
            if(ctx->curr_step < 0){
                ctx->curr_step = - (int32_t)((ctx->max - ctx->min)
                                        / cfg->fade.steps);
            }else{
                ctx->curr_step =  (ctx->max - ctx->min) / cfg->fade.steps;
            }
        }
    }

    ctx->curr_val = max(ctx->curr_val, ctx->min);
    ctx->curr_val = min(ctx->curr_val, ctx->max);

err_out:
    return result;
}

struct ctx_flicker
{
    uint32_t rate;
    unsigned int active;

    unsigned int off_delay;
    unsigned int on_delay;
    unsigned int next_off;
    unsigned int next_on;
};

void filter_flicker(struct led_filter *this,
                    enum strip_state *state,
                    hsvValue_t hsv_vals[],
                    unsigned int strip_len)
{
    int i;
    struct ctx_flicker *ctx;

    if(*state != state_flicker){
        return;
    }

    ctx = (struct ctx_flicker *) this->priv;

    if(ctx->next_off > 0){
        --(ctx->next_off);
        if(ctx->next_off == 0){
            do{
                ctx->next_on = urand() % ctx->on_delay;
            }while(ctx->next_on == 0);
        }
    }

    if(ctx->next_off == 0){
        for(i = 0; i < strip_len; ++i){
            hsv_vals[i].value = 0;
        }
    }

    if(ctx->next_on > 0){
        --(ctx->next_on);
        if(ctx->next_on == 0){
            ctx->off_delay /= 2;
            if(ctx->off_delay <= 1){
                ctx->next_off = 200;
                ctx->next_on = 0;
                ctx->off_delay = 200;
                ctx->on_delay = 40;
                ctx->active = 0;
                *state = state_eye;
            }else{
                do{
                    ctx->next_off = urand() % ctx->off_delay;
                }while(ctx->next_off == 0);
            }
        }
    }
}

int init_flicker(struct led_filter *this, struct blinken_cfg *cfg, bool update)
{
    int result;
    struct ctx_flicker *ctx;

    result = 0;

    if(update){
        ctx = (typeof(ctx)) this->priv;
    } else {
        this->name = "flicker";
        this->filter = filter_flicker;
        this->init = init_flicker;
        this->deinit = filter_deinit;
        INIT_LIST_HEAD(&(this->filters));
        
        ctx = malloc(sizeof(*ctx));
        if(ctx == NULL){
            printf("[%s] malloc() failed\n", __func__);
            result = -1;
            goto err_out;
        }

        memset(ctx, 0x0, sizeof(*ctx));
        this->priv = ctx;
    }

    if(cfg->flicker.valid == ~0x0){
        cfg->flicker.valid = 0;
        cfg->flicker.rate = 100;
        cfg_updated = 1;
    }

    ctx->next_off = 200;
    ctx->next_on = 0;
    ctx->off_delay = 200;
    ctx->on_delay = 40;
    
err_out:
    return result;
}

enum eye_state
{
    eye_init, eye_waking, eye_searching, eye_found, eye_hiding, eye_sleeping
};

struct ctx_eye
{
    enum eye_state state;
    uint32_t rate;
    uint32_t brightness;
    int32_t inc;
    int32_t tmp_peak;
    int32_t tmp_floor;
    uint32_t level;
    uint32_t curr_speed;
    uint32_t curr_pos;
    unsigned int target_pos;
    unsigned int jump_len;
    unsigned int jumps;
    unsigned int wait;
    unsigned int updates;
};

void filter_eye(struct led_filter *this,
                enum strip_state *state,
                hsvValue_t hsv_vals[],
                unsigned int strip_len)
{
    int i, jump;
    struct ctx_eye *ctx;

    ctx = (struct ctx_eye *) this->priv;

    if(ctx->rate == 0){
        *state = state_rainbow;
        return;
    }

    if(*state != state_eye){
        if(*state != state_flicker){
            ++ctx->updates;
            if(ctx->updates >= strip_len){
                ctx->updates = 0;
                if(urand() % ctx->rate == 0){
                    *state = state_flicker;
                }
            }
        }

        return;
    }

    switch (ctx->state) {
    case eye_init:
        ctx->brightness = scale_up(127u);
        ctx->tmp_floor = 0;
        ctx->tmp_peak = scale_up(8u);
        ctx->inc = ctx->tmp_peak / 16;
        ctx->level = 0;
        ctx->target_pos = scale_down(ctx->curr_pos);
        ctx->state = eye_waking;
        ctx->jumps = 5 + urand() % 10;
        ctx->jump_len = strip_len / 2;
        ctx->curr_speed = 1;
        ctx->wait = 60;
        // no break
    case eye_waking:
        if(ctx->level >= ctx->brightness){
            ctx->state = eye_searching;
            ctx->wait = 200;
        }else{
            ctx->level += ctx->inc;
            if(ctx->level >= ctx->tmp_peak){
                ctx->inc = -ctx->inc;
            }else if(ctx->inc < 0 && ctx->level <= ctx->tmp_floor){
                ctx->level = ctx->tmp_floor;
                ctx->tmp_floor = ctx->tmp_peak;
                ctx->tmp_peak *= 2;
                ctx->tmp_peak = min(ctx->tmp_peak, ctx->brightness);
                ctx->inc = (ctx->tmp_peak - ctx->tmp_floor) / 96;
            }
        }
        ctx->level = min(ctx->level, ctx->brightness);
        break;
    case eye_searching:
        if(scale_down(ctx->curr_pos) > ctx->target_pos){
            ctx->curr_pos -= ctx->curr_speed;
            ctx->curr_speed = min(ctx->curr_pos -
                                scale_up(ctx->target_pos), ctx->curr_speed + 1);

        }else if(scale_down(ctx->curr_pos) < ctx->target_pos){
            ctx->curr_pos += ctx->curr_speed;
            ctx->curr_speed = min(scale_up(ctx->target_pos) -
                                ctx->curr_pos, ctx->curr_speed + 1);
        }else{
            if(ctx->wait == 0){
                if(ctx->jumps > 0){
                    --ctx->jumps;
                    jump = 2 + urand() % (1 + ctx->jump_len);
                    ctx->jump_len /= 2;
                    if(urand() % 2){
                        jump = -jump;
                    }
                    if((ctx->target_pos + jump) < 2
                            || (ctx->target_pos + jump) >= (strip_len - 2)){
                        jump = -jump;
                    }
                    ctx->target_pos += jump;
                    ctx->curr_speed = scale_up(1);
                    ctx->wait = 10 + urand() % 10;
                }else{
                    ctx->state = eye_found;
                    ctx->wait = 100;
                }
            }else{
                --ctx->wait;
            }
        }
        break;
    case eye_found:
        if(ctx->wait == 0){
            ctx->state = eye_hiding;
            ctx->wait = 100;
        }else{
            --ctx->wait;
        }
        break;
    case eye_hiding:
        if(ctx->level <= scale_up(1u)){
            ctx->state = eye_sleeping;
            ctx->wait = 100;
        }else{
            ctx->level -= scale_up(1u);
        }
        break;
    case eye_sleeping:
        if(ctx->wait == 0){
            ctx->state = eye_init;
            *state = state_rainbow;
        }else{
            --ctx->wait;
        }
    }

    if(ctx->state != eye_sleeping){
        uint32_t pos;

        for(i = 0;i < strip_len;++i){
            hsv_vals[i].value = 0;
        }

        pos = max(scale_down(ctx->curr_pos), 2);
        pos = min(pos, strip_len - 2);

        hsv_vals[pos].hue = 0;
        hsv_vals[pos].saturation = 255;
        hsv_vals[pos].value = (uint8_t) scale_down(ctx->level);
        hsv_vals[pos - 1] = hsv_vals[pos];
        hsv_vals[pos - 1].value = hsv_vals[pos].value / 4;
        hsv_vals[pos + 1] = hsv_vals[pos];
        hsv_vals[pos + 1].value = hsv_vals[pos].value / 4;

    }

}

int init_eye(struct led_filter *this, struct blinken_cfg *cfg, bool update)
{
    int result;
    struct ctx_eye *ctx;

    result = 0;

    if(update){
        ctx = (typeof(ctx)) this->priv;
    } else {
        this->name = "eye";
        this->filter = filter_eye;
        this->init = init_eye;
        this->deinit = filter_deinit;
        INIT_LIST_HEAD(&(this->filters));
        
        ctx = malloc(sizeof(*ctx));
        if(ctx == NULL){
            printf("[%s] malloc() failed\n", __func__);
            result = -1;
            goto err_out;
        }

        memset(ctx, 0x0, sizeof(*ctx));
        this->priv = ctx;
    
        ctx->state = eye_init;
        ctx->wait = 100;
        ctx->updates = 0;
    }

    if(cfg->eye.valid == ~0x0){
        cfg->eye.valid = 0;
        cfg->eye.rate = 10;
        cfg_updated = 1;
    }

    if(cfg->eye.rate != 0){
        ctx->curr_pos = scale_up(cfg->strip_len / 2);
        ctx->rate = 100 / cfg->eye.rate;
    } else {
        ctx->rate = 0;
    }

err_out:
    return result;
}

static void load_config(void)
{
    flash_t flash;
    int result;

    printf("[%s] reading config from flash\n", __func__);

    device_mutex_lock(RT_DEV_LOCK_FLASH);
    result = flash_stream_read(&flash,
                               LED_SETTINGS_SECTOR,
                               sizeof(strip_cfg),
                               (uint8_t *) &strip_cfg);
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
}

static void save_config(void)
{
    flash_t flash;
    int result;

    printf("[%s] saving config to flash\n", __func__);

    cfg_updated = 0;

    device_mutex_lock(RT_DEV_LOCK_FLASH);
    flash_erase_sector(&flash, LED_SETTINGS_SECTOR);
    result = flash_stream_write(&flash, LED_SETTINGS_SECTOR,
                                sizeof(strip_cfg), (uint8_t *)&strip_cfg);

    if(result != 1){
        printf("[%s] Saving config failed\n", __func__);
        flash_erase_sector(&flash, LED_SETTINGS_SECTOR);
    }
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
}

struct blinken_cfg *blinken_get_config(void)
{
    struct blinken_cfg *cfg = NULL;

    if(strip_cfg.magic == BLINKEN_CFG_MAGIC){
        cfg = malloc(sizeof(*cfg));
        if(cfg != NULL){
            memmove(cfg, &strip_cfg, sizeof(*cfg));
        }
    }

    return cfg;
}

int blinken_set_config(struct blinken_cfg *cfg)
{
    int result;
    BaseType_t status;

    result = 0;
    if(cfg == NULL || cfg->magic != BLINKEN_CFG_MAGIC){
        result = -1;
        goto err_out;
    } 

    status = xSemaphoreTake(cfg_sema, 15 * configTICK_RATE_HZ);
    if(status != pdTRUE){
        printf("[%s] Timeout waiting for config sema.\n", __func__);
        result = -1;
        goto err_out;
    }

    result = init_handler(&handler, cfg, ws2812_cfg, true);
    if(result == 0){
        memmove(&strip_cfg, cfg, sizeof(strip_cfg));
        save_config();
    }

    xSemaphoreGive(cfg_sema);

err_out:
    return result;
}

void run_strip(void *pvParameters __attribute__((unused)))
{
    struct led_filter rainbow;
    struct led_filter fade;
    struct led_filter flicker;
    struct led_filter eye;
    struct led_filter *filter;
    enum strip_state state;
    int result;
    BaseType_t status;

    load_config();

    ws2812_cfg = ws2812_init(BLINKEN_MAX_LEDS);
    if(ws2812_cfg == NULL){
        printf("[%s] ws2812_init() failed\n", __func__);
        goto err_out;
    }

    result = init_handler(&handler, &strip_cfg, ws2812_cfg, false);
    if(result != 0){
        printf("[%s] init_handler() failed\n", __func__);
        goto err_out;
    }

    result = init_rainbow(&rainbow, &strip_cfg, false);
    if(result != 0){
        printf("[%s] init_rainbow() failed\n", __func__);
        goto err_out;
    }
    list_add_tail(&(rainbow.filters), &(handler.filters));

    result = init_fade(&fade, &strip_cfg, false);
    if(result != 0){
        printf("[%s] init_fade() failed\n", __func__);
        goto err_out;
    }
    list_add_tail(&(fade.filters), &(handler.filters));

    result = init_flicker(&flicker, &strip_cfg, false);
    if(result != 0){
        printf("[%s] init_flicker() failed\n", __func__);
        goto err_out;
    }
    list_add_tail(&(flicker.filters), &(handler.filters));

    result = init_eye(&eye, &strip_cfg, false);
    if(result != 0){
        printf("[%s] init_eye() failed\n", __func__);
        goto err_out;
    }
    list_add_tail(&(eye.filters), &(handler.filters));

    if(cfg_updated != 0){
        save_config();
    }

    while(1){
        status = xSemaphoreTake(cfg_sema, 5 * configTICK_RATE_HZ);
        if(status != pdTRUE){
            printf("[%s] timeout waiting for config sema\n", __func__);
            goto err_out;
        }

        list_for_each_entry(filter,
                            &(handler.filters),
                            filters,
                            struct led_filter)
        {
            filter->filter(filter, &state, handler.hsv_vals, handler.strip_len);
        }
        
        ws2812_send(ws2812_cfg, handler.hsv_vals, handler.strip_len,
                        handler.delay);

        xSemaphoreGive(cfg_sema);
    }

err_out:
    while(1){
        vTaskDelay(1000);
    }
}

#define STACKSIZE               512
TaskHandle_t ledstrip_task = NULL;

static int ledstrip_start(void)
{
    int result;
    BaseType_t status;

    printf("[%s] Called\n", __func__);
    
    result = 0;
    ledstrip_terminate = 0;

    memset(&handler, 0x0, sizeof(handler));

    if(cfg_sema == NULL){
        cfg_sema = xSemaphoreCreateMutex();
    }

    if(cfg_sema == NULL){
        printf("[%s] Creating cfg_sema failed\n", __func__);
        result = -1;
        goto err_out;
    }

    if(ledstrip_task == NULL){
        status = xTaskCreate(run_strip, (const char * )"led_strip", STACKSIZE,
                                 NULL, tskIDLE_PRIORITY + 1, &ledstrip_task);

        if(status != pdPASS){
            printf("[%s] Create ledstrip task failed!\n", __func__);
            result = -1;
            goto err_out;
        }
    }

err_out:
    return result;
}

void main(void)
{
    int result;

    console_init();
    
    printf("[%s] g_user_ap_sta_num: %d\n", __func__, g_user_ap_sta_num);

    result = ledstrip_start();
    if(result == 0){
        start_blinken_server();
        vTaskStartScheduler();
    }
}
