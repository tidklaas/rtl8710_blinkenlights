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
#include "event_groups.h"
#include <spi_api.h>
#include <spi_ex_api.h>
#include <autoconf.h>
#include <platform_stdlib.h>
#include "ws2812.h"
#include "blinken.h"

/* Quick and dirty, we use one big DMA buffer for the whole strip length.
 * TODO: use smaller DMA buffer and fill in bit patterns on the fly */
uint8_t dma_buffer[WS2812_DMABUF_LEN(WS2812_MAX_LEDS)];

// SPI0
#define SCLK_FREQ       3200000 // four "bits per bit" -> 800kHz
#define SPI0_MOSI       PC_2
#define SPI0_MISO       PC_3
#define SPI0_SCLK       PC_1
#define SPI0_CS         PC_0

/* Events to signal completion of DMA transfer */
#define BIT_START       (1 << 0)
#define BIT_DONE        (1 << 1)

/* wake up waiting tasks when DMA transfer is complete */
static void master_tr_done_callback(void *pdata, SpiIrq event)
{
    BaseType_t task_woken, result;
    ws2812_t *cfg;

    task_woken = pdFALSE;
    cfg = (ws2812_t *) pdata;

    switch (event) {
    case SpiRxIrq:
        break;
    case SpiTxIrq:
        result = xEventGroupSetBitsFromISR(cfg->events, BIT_DONE, &task_woken);
        if(result == pdPASS){
            portYIELD_FROM_ISR(task_woken);
        }
        break;
    default:
        DBG_8195A("unknown interrput evnent!\n");
    }
}

static void hsv2rgb(hsvValue_t *hsv, rgbValue_t *rgb)
{
    uint8_t r, g, b;
    uint8_t hue, sat, val;
    uint8_t base, sector, offset;
    uint8_t rise, fall;

    /* scale hue to range 0- 3*64. Makes subsequent calculations easier */
    hue = scale(hsv->hue, 192);
    sat = hsv->saturation;
    val = hsv->value;

    sector = hue / 64;
    offset = hue % 64;

    /* get common white base level and remaining colour amplitude */
    base = 255 - sat;

    rise = (offset * sat * 4) / 256;
    fall = 255 - base - rise;

    rise = (rise * val) / 256;
    fall = (fall * val) / 256;
    base = (base * val) / 256;

    rgb->red = base;
    rgb->green = base;
    rgb->blue = base;

    switch (sector) {
    case 0:
        rgb->red += fall;
        rgb->green += rise;
        break;
    case 1:
        rgb->green += fall;
        rgb->blue += rise;
        break;
    case 2:
        rgb->red += rise;
        rgb->blue += fall;
        break;
    }
}

/* convert a RGB byte into SPI data stream with 2 bits per byte */
static uint8_t *rgb2pwm(uint8_t *dst, const uint8_t colour)
{
    unsigned int cnt;
    uint32_t data = colour;

    for(cnt = 0;cnt < 4; ++cnt){
        switch (data & 0xC0) {
        case 0x00:
            *dst = WS_BITS_00;
            break;
        case 0x40:
            *dst = WS_BITS_01;
            break;
        case 0x80:
            *dst = WS_BITS_10;
            break;
        case 0xC0:
            *dst = WS_BITS_11;
            break;
        }

        dst++;
        data <<= 2;
    }

    return dst;
}

static int ws2812_tx(ws2812_t *cfg, uint16_t delay)
{
    int result;
    EventBits_t rcvd_events;
    TickType_t timeout;
    BaseType_t status;

    /* wait for any SPI transfer to finish */
    while(cfg->spi_master.state & SPI_STATE_TX_BUSY){
        vTaskDelay(0);
    }

    result = 0;
    
    /* obey requested delay */ 
    if(delay > 0){
        vTaskDelay(delay);
    }

    /* lock the DMA buffer mutex while it is transferred */
    status = xSemaphoreTake(cfg->mutex, configTICK_RATE_HZ);
    if(status != pdTRUE){
        printf("[%s] Timeout waiting for config mutex.\n", __func__);
        result = -1;
        goto err_out;
    }
    
    if(cfg->dma_buff == NULL || cfg->buff_len == 0){
        printf("[%s] DMA buffer invalid\n", __func__);
        result = -1;
        goto err_out;
    }

    xEventGroupClearBits(cfg->events, BIT_DONE);
    spi_master_write_stream_dma(&cfg->spi_master,
                                &(cfg->dma_buff[0]),
                                cfg->buff_len);

    timeout = 1000 / portTICK_PERIOD_MS;
    rcvd_events = xEventGroupWaitBits(
                        cfg->events, 
                        BIT_DONE,        // wait for DMA TX done
                        pdTRUE,          // clear event bit
                        pdFALSE,         // do not wait for all bits to be set
                        timeout );

    if(!(rcvd_events & BIT_DONE)){
        printf("[%s] DMA timeout\n", __func__);
        result = -1;
        goto err_out;
    }

err_out:
    /* release buffer mutex */
    xSemaphoreGive(cfg->mutex);
 
    return result;
}

int ws2812_send(ws2812_t *cfg, hsvValue_t hsv_values[],
                unsigned int strip_len, uint16_t delay)
{
    uint32_t i;
    uint8_t *bufp;
    uint16_t len, empty;
    BaseType_t status;
    int result;
    rgbValue_t rgb;

    /* make sure DMA buffer is not in use */
    while(cfg->spi_master.state & SPI_STATE_TX_BUSY){
        vTaskDelay(0);
    }

    /* lock the DMA buffer mutex while we fill it */
    status = xSemaphoreTake(cfg->mutex, configTICK_RATE_HZ);
    if(status != pdTRUE){
        printf("[%s] Timeout waiting for config mutex.\n", __func__);
        result = -1;
        goto err_out;
    }

    bufp = &(cfg->dma_buff[0]);
    
    /* make sure that we do not exceed the buffer */
    len = min(strip_len, cfg->strip_len);

    /* copy pixel data into DMA buffer */
    for(i = 0; i < len; ++i){
        hsv2rgb(&hsv_values[i], &rgb);
        bufp = rgb2pwm(bufp, rgb.green);
        bufp = rgb2pwm(bufp, rgb.red);
        bufp = rgb2pwm(bufp, rgb.blue);
    }
    
    /* turn unused pixels at end of strip off */ 
    if(cfg->strip_len > len){
        memset(bufp, WS_BITS_00, cfg->strip_len - len);
        bufp += cfg->strip_len - len;
    }

    /* add reset pulse */
    memset(bufp, 0x0, WS2812_RESET_LEN); 

    /* release buffer mutex */
    xSemaphoreGive(cfg->mutex);
 
    /* send it off to the strip */   
    result = ws2812_tx(cfg, delay);

err_out:    
    return result;
}

ws2812_t *ws2812_init(uint16_t strip_len)
{
    int result;
    BaseType_t status;
    uint32_t reset_off;
    ws2812_t *cfg;

    result = 0;

    cfg = malloc(sizeof(*cfg));
    if(cfg == NULL){
        printf("[%s] malloc for cfg failed\n", __func__);
        result = -1;
        goto err_out;
    }

    memset(cfg, 0x0, sizeof(*cfg));

    cfg->mutex = xSemaphoreCreateMutex();
    if(cfg->mutex == NULL){
        printf("[%s] Mutex creation failed\n", __func__);
        result = -1;
        goto err_out;
    }
    
    cfg->events = xEventGroupCreate();
    if(cfg->events == NULL){
        printf("[%s] Creating event group failed\n", __func__);
        result = -1;
        goto err_out;
    }

    spi_init(&(cfg->spi_master), SPI0_MOSI, SPI0_MISO, SPI0_SCLK, SPI0_CS);
    spi_format(&(cfg->spi_master), 8, 3, 0);
    spi_frequency(&(cfg->spi_master), SCLK_FREQ);
    spi_irq_hook(&(cfg->spi_master), master_tr_done_callback, (uint32_t)cfg);

    result = ws2812_set_len(cfg, strip_len);
    if(result != 0){
        printf("[%s] ws2812_set_len() failed\n", __func__);
    }

err_out:
    if(result != 0 && cfg != NULL){
        if(cfg->mutex != NULL){
            vQueueDelete(cfg->mutex);
        }

        if(cfg->events != NULL){
            vEventGroupDelete(cfg->events);
        }

        if(cfg->dma_buff != NULL){
            free(cfg->dma_buff);
        }

        free(cfg);
        cfg = NULL;
    }
    
    return cfg;
}

int ws2812_set_len(ws2812_t *cfg, uint16_t strip_len)
{
    int result;
    BaseType_t status;
    uint32_t reset_off;

    result = 0;

    if(cfg == NULL){
        printf("[%s] no config given\n", __func__);
        result = -1;
        goto err_out;
    }

    /* lock the config mutex */
    status = xSemaphoreTake(cfg->mutex, configTICK_RATE_HZ);
    if(status != pdTRUE){
        printf("[%s] Timeout waiting for config mutex.\n", __func__);
        result = -1;
        goto err_out;
    }

    if(strip_len <= WS2812_MAX_LEDS){
        /* TODO: use dynamically allocated buffer */
        cfg->dma_buff = dma_buffer;

        /* initialise LEDs to off and add reset pulse at end of strip */
        reset_off = WS2812_DMABUF_LEN(strip_len) - WS2812_RESET_LEN;
        memset(&(cfg->dma_buff[0]), WS_BITS_00, reset_off);
        memset(&(cfg->dma_buff[reset_off]), 0x0, WS2812_RESET_LEN);
        cfg->strip_len = strip_len;
        cfg->buff_len = WS2812_DMABUF_LEN(strip_len);
    } else {
        printf("[%s] Strip too long for DMA buffer\n", __func__);
        result = -1;
    }

    xSemaphoreGive(cfg->mutex);

err_out:
    return result;
}

