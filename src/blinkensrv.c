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
#include <string.h>
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "lwip/ip.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "netif/loopif.h"
#include "wlan_intf.h"

#include "blinkensrv.h"
#include "blinken.h"

#include "flash_api.h"
#include "device_lock.h"

#define SECTOR_SIZE             0x1000

#define MAX_PAGE_SIZE           (2048)

#define MAX_SOFTAP_SSID_LEN     32
#define MAX_PASSWORD_LEN        32
#define MAX_CHANNEL_NUM         13

#define HTTP_PORT   80
#define HTTP_OK     "HTTP/1.0 200 OK\r\nContent-type: text/html\r\n\r\n"
#define HTTP_404    "HTTP/1.0 404 Not Found\r\nContent-type: text/html\r\n\r\n"
#define HTTP_500    "HTTP/1.0 500 Internal Server Error\r\n" \
                        "Content-type: text/html\r\n\r\n"
#define SHORT_DELAY 10

/* HTML code mostly stolen from common/utilities/webserver.c */
#define HTML_HEAD_START \
"<html>\
<head>\
"

#define HTML_CSS \
"<style> \
body {\
text-align:center;\
font-family: 'Segoe UI';\
}\
.wrapper {\
text-align:left;\
margin:0 auto;\
margin-top:200px;\
border:#000;\
width:500px;\
}\
.header {\
background-color:#CF9;\
font-size:18px;\
line-height:50px;\
text-align:center;\
}\
.oneline {\
width:100%;\
border-left:#FC3 10px;\
font-size:15px;\
height:30px;\
margin-top:3px;\
}\
.left {\
background-color:#FF0;\
line-height:30px;\
height:100%;\
width:30%;\
float:left;\
padding-left:20px;\
}\
.right {\
margin-left:20px;\
}\
\
.box {\
width:40%;\
height:28px;\
margin-left:20px;\
\
}\
\
.btn {\
background-color:#CF9;\
height:40px;\
text-align:center;\
}\
\
.btn input {\
font-size:16px;\
height:30px;\
width:150px;\
border:0px;\
line-height:30px;\
margin-top:5px;\
border-radius:20px;\
background-color:#FFF;\
}\
.btn input:hover{\
cursor:pointer;\
background-color:#FB4044;\
}\
\
.foot {\
text-align:center;\
font-size:15px;\
line-height:20px;\
border:#CCC;\
}\
#pwd {\
display:none;\
}\
output {\
background-color:#FF0;\
line-height:30px;\
height:100%;\
width:10%;\
float:right;\
padding-left:20px;\
}\
</style>"

#define wifiHTML_TITLE \
"<title>Realtek SoftAP Config UI</title>"

#define wifiHTML_BODY_START \
"</head>\
<body  onLoad=\"onChangeSecType()\">\
<form method=\"post\" onSubmit=\"return onSubmitForm()\" >\
<div class=\"wrapper\">\
<div class=\"header\">\
Realtek SoftAP Configuration\
</div>"

#define wifiHTML_END \
" <div class=\"oneline btn\">\
<input  type=\"submit\" value=\"Submit\">\
</div>\
<div class=\"oneline foot\">\
Copyright &copy;realtek.com\
</div>\
 </div>\
 </form>\
</body>\
</html>\
"
#define wifiHTML_WAIT \
"HTTP/1.0 200 OK\r\nContent-type: text/html\r\n\r\n\
<head>\
</head>\
<body>\
<p>\
<h2>SoftAP is now restarting!</h2>\
<h2>Please wait a moment and reconnect!</h2>\
</p>"\
"</body>\r\n" \
"</html>"

#define onChangeSecType \
"<script>\
function onChangeSecType()\
{\
x=document.getElementById(\"sec\");\
y=document.getElementById(\"pwd\");\
if(x.value == \"open\"){\
y.style.display=\"none\";\
}else{\
y.style.display=\"block\";\
}\
}\
</script>"

#define onSubmitForm \
"<script>\
function onSubmitForm()\
{\
x=document.getElementById(\"Ssid\");\
y=document.getElementById(\"pwd\");\
z=document.getElementById(\"pwd_val\");\
if(x.value.length>32)\
{\
alert(\"SoftAP SSID is too long!(1-32)\");\
return false;\
}\
/*if(!(/^[A-Za-z0-9]+$/.test(x.value)))\
{\
alert(\"SoftAP SSID can only be [A-Za-z0-9]\");\
return false;\
}*/\
if(y.style.display == \"block\")\
{\
if((z.value.length < 8)||(z.value.length>32))\
{\
alert(\"Password length is between 8 to 32\");\
return false;\
}\
}\
}\
</script>"

#define ledHTML_TITLE \
"<title>Blinkenlights Config UI</title>"

#define ledHTML_BODY_START \
"</head>\
<body>\
<form method=\"post\" >\
<div class=\"wrapper\">\
<div class=\"header\">\
Blinkenlights Configuration\
</div>"

#define ledHTML_END \
" <div class=\"oneline btn\">\
<input  type=\"submit\" value=\"Submit\">\
</div>\
<div class=\"oneline foot\">\
CC-BY-SA\
</div>\
</div>\
</form>\
</body>\
</html>\
"

static int handle_conn(struct netconn *conn);
static rtw_wifi_setting_t wifi_setting = {
        .mode = RTW_MODE_NONE,
        .ssid = { 0 },
        .channel = 0,
        .security_type = RTW_SECURITY_OPEN,
        .password = { 0 }
};

#ifndef WLAN0_NAME
#define WLAN0_NAME      "wlan0"
#endif

#ifndef WLAN1_NAME
#define WLAN1_NAME      "wlan1"
#endif 

static void load_wifi_settings()
{
    const char *ifname = WLAN0_NAME;

    if(rltk_wlan_running(WLAN1_IDX)){
        ifname = WLAN1_NAME;
    }

    wifi_get_setting(ifname, &wifi_setting);
}

static void load_wifi_cfg()
{
    flash_t flash;
    rtw_wifi_config_t local_config;
    int result;

    device_mutex_lock(RT_DEV_LOCK_FLASH);
    result = flash_stream_read(&flash,
                               AP_SETTING_SECTOR,
                               sizeof(rtw_wifi_config_t),
                               (uint8_t *) (&local_config));

    device_mutex_unlock(RT_DEV_LOCK_FLASH);

#if 0
    printf("[%s] local_config.boot_mode=0x%x\n", __func__,
            local_config.boot_mode);
    printf("[%s] local_config.ssid=%s\n", __func__,
            local_config.ssid);
    printf("[%s] local_config.channel=%d\n", __func__,
            local_config.channel);
    printf("[%s] local_config.security_type=%d\n", __func__,
            local_config.security_type);
    printf("[%s] local_config.password=%s\n", __func__,
            local_config.password);
#endif

    if(local_config.boot_mode == 0x77665502){
        wifi_setting.mode = RTW_MODE_AP;

        if(local_config.ssid_len > 32){
            local_config.ssid_len = 32;
        }

        memset(&wifi_setting, 0x0, sizeof(wifi_setting));
        memcpy(wifi_setting.ssid, local_config.ssid, local_config.ssid_len);
        wifi_setting.ssid[local_config.ssid_len] = '\0';
        wifi_setting.channel = local_config.channel;

        if(local_config.security_type == 1){
            wifi_setting.security_type = RTW_SECURITY_WPA2_AES_PSK;
        }else{
            wifi_setting.security_type = RTW_SECURITY_OPEN;
        }

        if(local_config.password_len > 32){
            local_config.password_len = 32;
        }

        memcpy(wifi_setting.password,
               local_config.password,
               local_config.password_len);

        wifi_setting.password[local_config.password_len] = '\0';
    }else{
        load_wifi_settings();
    }
}

static int store_ap_info(void)
{
    flash_t flash;
    rtw_wifi_config_t wifi_config;
    uint8_t *buff;
    size_t ssid_len, passwd_len;
    int result;

    buff = malloc(SECTOR_SIZE);
    if(buff == NULL){
        printf("[%s] malloc() failed\n", __func__);
        return -1;
    }

    memset(&wifi_config, 0x00, sizeof(rtw_wifi_config_t));

    wifi_config.boot_mode = 0x77665502;

    ssid_len = strlen((char* )wifi_setting.ssid);
    wifi_config.ssid_len = ssid_len;
    memcpy(wifi_config.ssid, wifi_setting.ssid, ssid_len);

    passwd_len = strlen((char* )wifi_setting.password);
    wifi_config.password_len = passwd_len;
    memcpy(wifi_config.password, wifi_setting.password, passwd_len);

    wifi_config.security_type = wifi_setting.security_type != 0 ? 1 : 0;

    wifi_config.channel = wifi_setting.channel;

    device_mutex_lock(RT_DEV_LOCK_FLASH);

    result = flash_stream_read(&flash, AP_SETTING_SECTOR, SECTOR_SIZE, buff);
    if(result != 1){
        printf("[%s] Flash read failed\n", __func__);
        result = -1;
        goto err_out;
    }

    memcpy(buff, &wifi_config, sizeof(wifi_config));

    flash_erase_sector(&flash, AP_SETTING_SECTOR);

    result = flash_stream_write(&flash, AP_SETTING_SECTOR, SECTOR_SIZE, buff);

err_out:
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    if(buff != NULL){
        free(buff);
    }

    return result;
}

static void restart_soft_ap()
{
#if 0
    printf("[%s] ssid=%s\n", __func__, wifi_setting.ssid);
    printf("[%s] ssid_len=%d\n", __func__, strlen((char*)wifi_setting.ssid));
    printf("[%s] security_type=%d\n", __func__, wifi_setting.security_type);
    printf("[%s] password=%s\n", __func__, wifi_setting.password);
    printf("[%s] password_len=%d\n", __func__,
                strlen((char*)wifi_setting.password));
    printf("[%s] channel=%d\n", __func__, wifi_setting.channel);
#endif

    wifi_restart_ap(wifi_setting.ssid, wifi_setting.security_type,
            wifi_setting.password, strlen((char* )wifi_setting.ssid),
            strlen((char* )wifi_setting.password), wifi_setting.channel);
}

static int add_ssid_item(char *pbuf, size_t buf_len, u8_t *ssid, u8_t ssid_len)
{
    int written;
    char local_ssid[MAX_SOFTAP_SSID_LEN + 1];

    if(ssid_len > MAX_SOFTAP_SSID_LEN){
        ssid_len = MAX_SOFTAP_SSID_LEN;
    }

    memcpy(local_ssid, ssid, ssid_len);
    local_ssid[ssid_len] = '\0';

    written =
        snprintf(pbuf, buf_len,
                 "<div class=\"oneline\"><div class=\"left\">SoftAP SSID:</div>"
                 "<div class=\"right\">"
                 "<input class=\"box\" type=\"text\" name=\"Ssid\" id=\"Ssid\""
                 " value=\"%s\"></div></div>",
                 local_ssid);

    return written;
}

static int add_sec_type_item(char *pbuf, size_t buf_len, u32_t sectype)
{
    int written;
    u8_t flag[2] = { 0, 0 };

    if(sectype == RTW_SECURITY_OPEN){
        flag[0] = 1;
    }else if(sectype == RTW_SECURITY_WPA2_AES_PSK){
        flag[1] = 1;
    }else{
        return -1;
    }

    written =
        snprintf(pbuf, buf_len,
                "<div class=\"oneline\"><div class=\"left\">"
                "Security Type: </div><div class=\"right\">"
                "<select  class=\"box\" name=\"Security Type\"  id=\"sec\" "
                "onChange=onChangeSecType()>"
                "<option value=\"open\" %s>OPEN</option>"
                "<option value=\"wpa2-aes\" %s>WPA2-AES</option>"
                "</select></div></div>",
                flag[0] ? "selected" : "",
                flag[1] ? "selected" : "");

    return written;
}

static int add_passwd_item(char *pbuf, size_t buf_len, u8_t *password,
        u8_t passwd_len)
{
    int written;
    char local_passwd[MAX_PASSWORD_LEN + 1];

    if(passwd_len > MAX_PASSWORD_LEN){
        passwd_len = MAX_PASSWORD_LEN;
    }

    if(passwd_len > 0){
        memcpy(local_passwd, password, passwd_len);
        local_passwd[passwd_len] = '\0';
    }

    written =
        snprintf(pbuf, buf_len,
                 "<div class=\"oneline\" id=\"pwd\"><div class=\"left\">"
                 "Password: </div>"
                 "<div class=\"right\" >"
                 "<input  class=\"box\" id=\"pwd_val\" type=\"text\" "
                 "name=\"Password\" value=\"%s\" ></div></div>",
                 passwd_len ? local_passwd : "");

    return written;
}

static int add_channel_item(char *pbuf, size_t buf_len, u8_t channel)
{
    int written;
    u8_t flag[MAX_CHANNEL_NUM + 1] = { 0 };

    if(channel > MAX_CHANNEL_NUM){
        printf("Channel(%d) is out of range!\n", channel);
        channel = 1;
    }

    flag[channel] = 1;

    written =
        snprintf(pbuf, buf_len,
                "<div class=\"oneline\"><div class=\"left\">Channel: </div>"
                "<div class=\"right\"><select  class=\"box\" name=\"Channel\">"
                "<option value=\"1\" %s>1</option>"
                "<option value=\"2\" %s>2</option>"
                "<option value=\"3\" %s>3</option>"
                "<option value=\"4\" %s>4</option>"
                "<option value=\"5\" %s>5</option>"
                "<option value=\"6\" %s>6</option>"
                "<option value=\"7\" %s>7</option>"
                "<option value=\"8\" %s>8</option>"
                "<option value=\"9\" %s>9</option>"
                "<option value=\"10\" %s>10</option>"
                "<option value=\"11\" %s>11</option>"
                "</select> </div> </div>",
                flag[1] ? "selected" : "", flag[2] ? "selected" : "",
                flag[3] ? "selected" : "", flag[4] ? "selected" : "",
                flag[5] ? "selected" : "", flag[6] ? "selected" : "",
                flag[7] ? "selected" : "", flag[8] ? "selected" : "",
                flag[9] ? "selected" : "", flag[10] ? "selected" : "",
                flag[11] ? "selected" : "");

    return written;
}

static int add_range_item(char *pbuf, size_t buf_left, char *name,
                          char *desc, int min, int max, int step, int val)
{
    int written;

    written =
        snprintf(pbuf, buf_left,
                "<div class=\"oneline\">"
                "<div class=\"left\">%s:</div>"
                "<div class=\"right\">"
                "<input class=\"box\" "
                "oninput=\"%s_out.value=parseInt(%s_in.value)\" "
                "type=\"range\" name=\"%s_in\" id=\"%s_in\" min=\"%d\" "
                "max=\"%d\" value=\"%d\">"
                "<output class=\"out\" name=\"%s_out\" "
                "for=\"%s_in\">%d</output>"
                "</div></div>",
                desc, name, name, name, name, min, max, val, name, name, val);

    return written;
}

static int add_maincfg_item(char *pbuf, size_t buf_left,
        struct blinken_cfg *led_cfg)
{
    int written, total;

    total = 0;

    written = snprintf(pbuf, buf_left, "<p>Strip Config</p>");
    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "strip_len", "Strip length",
                             0, BLINKEN_MAX_LEDS, 1, led_cfg->strip_len);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "delay", "Update delay",
                             0, 100, 1, led_cfg->delay);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

err_out:
    return total;
}

static int add_rainbow_item(char *pbuf, size_t buf_left,
        struct blinken_cfg *led_cfg)
{
    int written, total;

    total = 0;

    written = snprintf(pbuf, buf_left, "<p>Rainbow Config</p>");
    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "hue_min", "Min. Hue",
                             0, 255, 1, led_cfg->rainbow.hue_min);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "hue_max", "Max. Hue",
                             0, 255, 1, led_cfg->rainbow.hue_max);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "hue_steps", "Hue Steps",
                             0, BLINKEN_MAX_STEPS, 1,
                             led_cfg->rainbow.hue_steps);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "cycle_steps", "Cycle Steps", 0,
            BLINKEN_MAX_STEPS, 1, led_cfg->rainbow.cycle_steps);
    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

err_out:
    return total;
}

static int add_fade_item(char *pbuf, size_t buf_left,
        struct blinken_cfg *led_cfg)
{
    int written, total;

    total = 0;

    written = snprintf(pbuf, buf_left, "<p>Fade Config</p>");
    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "fade_min", "Min.",
                             0, 255, 1, led_cfg->fade.min);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "fade_max", "Max.",
                             0, 255, 1, led_cfg->fade.max);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "fade_steps", "Step",
                             0, BLINKEN_MAX_STEPS, 1, led_cfg->fade.steps);

    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

err_out:
    return total;
}

static int add_eye_item(char *pbuf, size_t buf_left,
        struct blinken_cfg *led_cfg)
{
    int written, total;

    total = 0;

    written = snprintf(pbuf, buf_left, "<p>Eye Config</p>");
    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

    written = add_range_item(pbuf, buf_left, "eye_rate", "Rate",
                             0, 20, 1, led_cfg->eye.rate);
    if(written < 0 || written >= buf_left){
        total = -1;
        goto err_out;
    }
    pbuf += written;
    buf_left -= written;
    total += written;

err_out:
    return total;
}

#define handle_html_buff    \
    do{ \
        if(written < 0 || written >= MAX_PAGE_SIZE){    \
            result = -1;    \
            goto err_out;   \
        }   \
        status = netconn_write(conn, html_buff, (u16_t) strlen(html_buff), \
                                   NETCONN_COPY);   \
        if(status != ERR_OK){   \
            printf("[%s] %s\n", __func__, lwip_strerr(status)); \
            result = -1;    \
            goto err_out;   \
        }   \
    }while(0)

static int send_blinken_html(struct netconn *conn)
{
    char *html_buff;
    int written;
    struct blinken_cfg *led_cfg;
    err_t status;
    int result;

    result = 0;
    html_buff = NULL;
    led_cfg = NULL;

    html_buff = malloc(MAX_PAGE_SIZE);
    if(html_buff == NULL){
        printf("[%s] malloc failed\n", __func__);

        netconn_write(conn, HTTP_500, (u16_t) strlen(HTTP_500), NETCONN_COPY);

        result = -1;
        goto err_out;
    }

    memset(html_buff, 0x0, MAX_PAGE_SIZE);

    led_cfg = blinken_get_config();
    if(led_cfg == NULL){
        printf("[%s] LED config read failed\n", __func__);

        netconn_write(conn, HTTP_500, (u16_t) strlen(HTTP_500), NETCONN_COPY);

        result = -1;
        goto err_out;
    }

    status = netconn_write(conn, HTTP_OK, (u16_t) strlen(HTTP_OK),
                           NETCONN_COPY);
    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    status = netconn_write(conn, HTML_HEAD_START,
                          (u16_t) strlen(HTML_HEAD_START), NETCONN_COPY);
    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    /* add css */
    status = netconn_write(conn, HTML_CSS,
                           (u16_t) strlen(HTML_CSS), NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    status = netconn_write(conn, ledHTML_TITLE,
                          (u16_t) strlen(ledHTML_TITLE), NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    status = netconn_write(conn, ledHTML_BODY_START,
                           (u16_t) strlen(ledHTML_BODY_START), NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    written = add_maincfg_item(html_buff, MAX_PAGE_SIZE, led_cfg);
    handle_html_buff;

    written = add_rainbow_item(html_buff, MAX_PAGE_SIZE, led_cfg);
    handle_html_buff;

    written = add_fade_item(html_buff, MAX_PAGE_SIZE, led_cfg);
    handle_html_buff;

    written = add_eye_item(html_buff, MAX_PAGE_SIZE, led_cfg);
    handle_html_buff;

    status = netconn_write(conn, ledHTML_END, (u16_t) strlen(ledHTML_END),
                           NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

err_out:
    if(html_buff != NULL){
        free(html_buff);
    }

    if(led_cfg != NULL){
        free(led_cfg);
    }

    return result;
}

static void http_translate_url_encode(char *url)
{
    size_t data_len, offset;
    char buffer[3];
    unsigned int hexnum;

    data_len = strlen(url) + 1;
    offset = 0;
    buffer[3] = '\0';
    while(url[offset] != '\0'){
        if(url[offset] == '%'){
            if(isxdigit(url[offset + 1]) && isxdigit(url[offset + 2])){
                buffer[0] = url[offset + 1];
                buffer[1] = url[offset + 2];
                sscanf(buffer, "%x", &hexnum);
                url[offset] = (char) hexnum;
                data_len -= 2;
                /* destroy url */
                url[offset] = 0;
                url[offset + 1] = '\0';
                url[offset + 2] = '\0';

                memmove(&url[offset + 1], &url[offset + 3], data_len - offset);
            }
        }else if(url[offset] == '+'){
            url[offset] = ' ';
        }

        ++offset;
    }
}

#define CRLF "\r\n"

/* get value from post parameter, replace parameter delimiter by \0 */
static char *get_post_param(char *param)
{
    char *end;

    while(*param && *param != '='){
        ++param;
    }

    ++param;

    end = strstr(param, "&");
    if(*end != NULL){
        *end = '\0';
    }

    return param;
}

enum http_method
{
    http_get, http_post
};

struct http_handler;

typedef int (*http_handler_fn)(struct http_handler *this,
                               struct netconn *conn,
                               struct netbuf *buff);

struct http_handler
{
    struct list_head list;
    enum http_method method;
    char *path;
    http_handler_fn func;
    void *priv;
};

static struct netconn *listen_conn = NULL;

int handle_root_get(struct http_handler *this,
                    struct netconn *conn,
                    struct netbuf *rcv_buff)
{
    char *html_buff;
    int written;
    struct blinken_cfg *led_cfg;
    err_t status;
    int result;

    result = send_blinken_html(conn);

    netbuf_delete(rcv_buff);
    netconn_close(conn);
    netconn_delete(conn);

    return result;
}

int handle_root_post(struct http_handler *this, struct netconn *conn,
        struct netbuf *rcv_buff)
{
    char *req_str, *body, *end;
    char *strip_len, *delay;
    char *hue_min, *hue_max, *hue_steps, *cycle_steps;
    char *fade_min, *fade_max, *fade_steps;
    char *eye_rate;
    char *data;
    uint16_t data_len;
    err_t status;
    size_t req_len;
    unsigned int val;
    int result;
    size_t len;
    struct blinken_cfg *led_cfg;

    result = 0;
    led_cfg = NULL;

    led_cfg = blinken_get_config();
    if(led_cfg == NULL){
        result = -1;
        goto err_out;
    }

    req_str = malloc(MAX_PAGE_SIZE);
    if(req_str == NULL){
        printf("[%s] malloc failed\n", __func__);
        goto err_out;
    }

    req_len = 0;
    memset(req_str, 0x0, MAX_PAGE_SIZE);

    do{
        status = netbuf_data(rcv_buff, (void *) &data, &data_len);
        if(status != ERR_OK){
            break;
        }

        if(req_len + data_len < MAX_PAGE_SIZE){
            memcpy(req_str + req_len, data, data_len);
            req_len += data_len;

            if(netbuf_next(rcv_buff) < 0){
                break;
            }
        }else{
            status = ERR_MEM;
        }

    }while(status == ERR_OK);

    body = strstr(req_str, CRLF CRLF);
    if(body == NULL){
        printf("[%s] no request body found.\n", __func__);
        result = -1;
        goto err_out;
    }

    strip_len = strcasestr(body, "strip_len_in=");
    delay = strcasestr(body, "delay_in=");
    hue_min = strcasestr(body, "hue_min_in=");
    hue_max = strcasestr(body, "hue_max_in=");
    hue_steps = strcasestr(body, "hue_steps_in=");
    cycle_steps = strcasestr(body, "cycle_steps_in=");
    fade_min = strcasestr(body, "fade_min_in=");
    fade_max = strcasestr(body, "fade_max_in=");
    fade_steps = strcasestr(body, "fade_steps_in=");
    eye_rate = strcasestr(body, "eye_rate_in=");

    strip_len = get_post_param(strip_len);
    delay = get_post_param(delay);
    hue_min = get_post_param(hue_min);
    hue_max = get_post_param(hue_max);
    hue_steps = get_post_param(hue_steps);
    cycle_steps = get_post_param(cycle_steps);
    fade_min = get_post_param(fade_min);
    fade_max = get_post_param(fade_max);
    fade_steps = get_post_param(fade_steps);
    eye_rate = get_post_param(eye_rate);

    if(strip_len == NULL || delay == NULL || hue_min == NULL
            || hue_max == NULL || hue_steps == NULL || cycle_steps == NULL
            || fade_min == NULL || fade_max == NULL || fade_steps == NULL
            || eye_rate == NULL){
        printf("[%s] parameter missing\n", __func__);
        result = -1;
        goto err_out;
    }

#if 0
    printf("[%s] strip_len passed = %s\n", __func__, strip_len);
    printf("[%s] delay passed = %s\n", __func__, delay);
    printf("[%s] hue_min passed = %s\n", __func__, hue_min);
    printf("[%s] hue_max passed = %s\n", __func__, hue_max);
    printf("[%s] hue_steps passed = %s\n", __func__, hue_steps);
    printf("[%s] cycle_steps passed = %s\n", __func__, cycle_steps);
    printf("[%s] fade_min passed = %s\n", __func__, fade_min);
    printf("[%s] fade_max passed = %s\n", __func__, fade_max);
    printf("[%s] fade_steps passed = %s\n", __func__, fade_steps);
    printf("[%s] eye_rate passed = %s\n", __func__, eye_rate);
#endif
    val = strtoul(strip_len, NULL, 10);
    if((val <= BLINKEN_MAX_LEDS) && (val > 0)){
        led_cfg->strip_len = val;
    }

    val = strtoul(delay, NULL, 10);
    if((val <= 100)){
        led_cfg->delay = val;
    }

    val = strtoul(hue_min, NULL, 10);
    if((val <= 255)){
        led_cfg->rainbow.hue_min = val;
    }

    val = strtoul(hue_max, NULL, 10);
    if((val <= 255)){
        led_cfg->rainbow.hue_max = val;
    }

    val = strtoul(hue_steps, NULL, 10);
    if((val <= BLINKEN_MAX_STEPS)){
        led_cfg->rainbow.hue_steps = val;
    }

    val = strtoul(cycle_steps, NULL, 10);
    if((val <= BLINKEN_MAX_STEPS)){
        led_cfg->rainbow.cycle_steps = val;
    }

    val = strtoul(fade_min, NULL, 10);
    if((val <= 255)){
        led_cfg->fade.min = val;
    }

    val = strtoul(fade_max, NULL, 10);
    if((val <= 255)){
        led_cfg->fade.max = val;
    }

    val = strtoul(fade_steps, NULL, 10);
    if((val <= 255)){
        led_cfg->fade.steps = val;
    }

    val = strtoul(eye_rate, NULL, 10);
    if((val <= 20)){
        led_cfg->eye.rate = val;
    }

    result = blinken_set_config(led_cfg);

err_out:
    free(req_str);

    if(led_cfg != NULL){
        free(led_cfg);
    }
    result = send_blinken_html(conn);

    netbuf_delete(rcv_buff);
    netconn_close(conn);
    netconn_delete(conn);

    return result;
}

static int send_wifi_html(struct netconn *conn)
{
    char *html_buff;
    err_t status;
    int written, result;
    uint16_t req_len;

    result = 0;
    html_buff = malloc(MAX_PAGE_SIZE);
    if(html_buff == NULL){
        printf("[%s] malloc failed\n", __func__);
        netconn_write(conn, HTTP_500, (u16_t) strlen(HTTP_500), NETCONN_COPY);
        result = -1;
        goto err_out;
    }

    memset(html_buff, 0x0, MAX_PAGE_SIZE);

    /* Write out the HTTP OK header. */
    status = netconn_write(conn, HTTP_OK, (u16_t) strlen(HTTP_OK),
                           NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }
    status = netconn_write(conn, HTML_HEAD_START,
                           (u16_t) strlen(HTML_HEAD_START), NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    /* Add script */
    status = netconn_write(conn, onChangeSecType,
                           (u16_t) strlen(onChangeSecType), NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    status = netconn_write(conn, onSubmitForm, (u16_t) strlen(onSubmitForm),
                           NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    /* add css */
    status = netconn_write(conn, HTML_CSS, (u16_t) strlen(HTML_CSS),
                           NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    status = netconn_write(conn, wifiHTML_TITLE, (u16_t) strlen(wifiHTML_TITLE),
                           NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    /* Add Body start */
    status = netconn_write(conn, wifiHTML_BODY_START,
                           (u16_t) strlen(wifiHTML_BODY_START), NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

    /* Add SSID */
    written = add_ssid_item(html_buff, MAX_PAGE_SIZE, wifi_setting.ssid,
                            strlen((char* )wifi_setting.ssid));
    handle_html_buff;

    /* Add SECURITY TYPE  */
    written = add_sec_type_item(html_buff, MAX_PAGE_SIZE,
                                wifi_setting.security_type);
    handle_html_buff;

    /* Add PASSWORD */
    written = add_passwd_item(html_buff, MAX_PAGE_SIZE, wifi_setting.password,
                              strlen((char* )wifi_setting.password));
    handle_html_buff;

    /* Add CHANNEL  */
    written = add_channel_item(html_buff, MAX_PAGE_SIZE, wifi_setting.channel);
    handle_html_buff;

    /* Add page footer. */
    status = netconn_write(conn, wifiHTML_END, (u16_t) strlen(wifiHTML_END),
                           NETCONN_COPY);

    if(status != ERR_OK){
        printf("[%s] %s\n", __func__, lwip_strerr(status));
        result = -1;
        goto err_out;
    }

err_out:
    if(html_buff != NULL){
        free(html_buff);
    }

    return result;
}

int handle_wifi_get(struct http_handler *this, struct netconn *conn,
        struct netbuf *rcv_buff)
{
    char *request, *html_buff;
    err_t status;
    int written, result;

    uint16_t req_len;

    result = send_wifi_html(conn);

    netbuf_delete(rcv_buff);
    netconn_close(conn);
    netconn_delete(conn);

    return 0;
}

int handle_wifi_post(struct http_handler *this, struct netconn *conn,
        struct netbuf *rcv_buff)
{
    rtw_security_t secType;
    char *req_str, *body, *ssid, *sec_type, *passwd, *chan, *end;
    char *data;
    uint16_t data_len;
    err_t status;
    size_t req_len;
    unsigned int updated;
    unsigned int channel;
    size_t len;
    struct netconn *tmp_conn;
    int result;

    updated = 0;
    result = 0;

    req_str = malloc(MAX_PAGE_SIZE);
    if(req_str == NULL){
        printf("[%s] malloc failed\n", __func__);
        goto err_out;
    }

    req_len = 0;
    memset(req_str, 0x0, MAX_PAGE_SIZE);

    do{
        status = netbuf_data(rcv_buff, (void *) &data, &data_len);
        if(status != ERR_OK){
            break;
        }

        if(req_len + data_len < MAX_PAGE_SIZE){
            memcpy(req_str + req_len, data, data_len);
            req_len += data_len;

            if(netbuf_next(rcv_buff) < 0){
                break;
            }
        }else{
            status = ERR_MEM;
        }

    }while(status == ERR_OK);

    body = strstr(req_str, CRLF CRLF);
    if(body == NULL){
        printf("[%s] no request body found.\n", __func__);
        goto err_out;
    }

    ssid = strcasestr(body, "Ssid=");
    sec_type = strcasestr(body, "Security+Type=");
    passwd = strcasestr(body, "Password=");
    chan = strcasestr(body, "Channel=");

    ssid = get_post_param(ssid);
    sec_type = get_post_param(sec_type);
    passwd = get_post_param(passwd);
    chan = get_post_param(chan);

    if(ssid == NULL || sec_type == NULL || passwd == NULL || chan == NULL){
        printf("[%s] parameter missing\n", __func__);
        goto err_out;
    }

    http_translate_url_encode(ssid);
    http_translate_url_encode(passwd);

#if 0
    printf("[%s] ssid passed = %s\n", __func__, ssid);
    printf("[%s] sec_type passed = %s\n", __func__, sec_type);
    printf("[%s] passwd passed = %s\n", __func__, passwd);
    printf("[%s] chan passed = %s\n", __func__, chan);
#endif

    len = strlen(ssid);
    if(len > MAX_SOFTAP_SSID_LEN){
        len = MAX_SOFTAP_SSID_LEN;
        ssid[len] = '\0';
    }

    if(strcmp((char* )wifi_setting.ssid, ssid)){
        strcpy((char*) wifi_setting.ssid, ssid);

        updated = 1;
    }

    if(!strcmp(sec_type, "wpa2-aes")){
        secType = RTW_SECURITY_WPA2_AES_PSK;
    }else{
        secType = RTW_SECURITY_OPEN;
    }

    if(wifi_setting.security_type != secType){
        wifi_setting.security_type = secType;
        updated = 1;
    }

    if(wifi_setting.security_type > RTW_SECURITY_OPEN){
        len = strlen(passwd);
        if(len > MAX_PASSWORD_LEN){
            len = MAX_PASSWORD_LEN;
            passwd[len] = '\0';
        }

        if(strcmp((char* )wifi_setting.password, passwd)){
            strcpy((char*) wifi_setting.password, passwd);
            updated = 1;
        }
    }

    channel = strtoul(chan, NULL, 10);
    if((channel > MAX_CHANNEL_NUM) || (channel < 1)){
        channel = 1;
    }

    if(wifi_setting.channel != channel){
        wifi_setting.channel = channel;
        updated = 1;
    }

    memset(req_str, 0x0, MAX_PAGE_SIZE);

    if(updated){
        store_ap_info();
        /* Write out the HTTP OK header. */
        netconn_write(conn, wifiHTML_WAIT, (u16_t) strlen(wifiHTML_WAIT),
                      NETCONN_COPY);

        vTaskDelay(200 / portTICK_RATE_MS);

    }else{
        result = send_wifi_html(conn);
    }

err_out:
    if(req_str != NULL){
        free(req_str);
    }

    netbuf_delete(rcv_buff);
    netconn_close(conn);
    netconn_delete(conn);

    if(updated){
        restart_soft_ap();

        listen_conn->recv_timeout = 1;
        port_netconn_accept(listen_conn, tmp_conn, result);
        if(tmp_conn != NULL && result == ERR_OK){
            netconn_close(tmp_conn);
            while(netconn_delete(tmp_conn) != ERR_OK){
                vTaskDelay(SHORT_DELAY);
            }
        }
        listen_conn->recv_timeout = 0;
    }

    return result;
}

int handle_404(struct http_handler *this, struct netconn *conn,
        struct netbuf *rcv_buff)
{
    netconn_write(conn, HTTP_404, (u16_t) strlen(HTTP_404), NETCONN_COPY);
    netbuf_delete(rcv_buff);
    netconn_close(conn);
    netconn_delete(conn);

    return 0;
}

LIST_HEAD(req_handlers);
struct http_handler local_handlers[] = {
  {.method = http_get, .path = "/",    .func = handle_root_get, .priv = NULL},
  {.method = http_post,.path = "/",    .func = handle_root_post,.priv = NULL},
  {.method = http_get, .path = "/wifi",.func = handle_wifi_get, .priv = NULL},
  {.method = http_post,.path = "/wifi",.func = handle_wifi_post,.priv = NULL},
  {.path = NULL, .func = NULL }, };

struct http_handler handler_404 = {
        .method = http_get, .path = "", .func = handle_404, .priv = NULL };

#define ARRAY_SIZE(x)   (sizeof(x) / sizeof(*x))

static int handle_conn(struct netconn *conn)
{
    static portCHAR cDynamicPage[MAX_PAGE_SIZE];
    struct netbuf *rcv_buff;
    portCHAR *req_str;
    unsigned portSHORT req_len;
    u8_t bChanged = 0;
    int result;
    int ret_accept = ERR_OK;
    char *ptr = NULL;
    char method[5];
    char path[33];
    unsigned int i;
    enum http_method req_method;
    struct http_handler *handler, *tmp_handler;

    rcv_buff = NULL;
    port_netconn_recv(conn, rcv_buff, result);

    if(rcv_buff == NULL || result != ERR_OK){
        goto err_out;
    }

    netbuf_data(rcv_buff, (void *) &req_str, &req_len);

    result = sscanf(req_str, "%4s %32s HTTP/", method, path);
    if(result != 2){
        printf("[%s] Parsing request failed\n", __func__);
        goto err_out;
    }

    if(!strncmp(method, "GET", strlen("GET"))){
        req_method = http_get;
    }else if(!strncmp(method, "POST", strlen("POST"))){
        req_method = http_post;
    }else{
        printf("[%s] invalid HTTP method: %s\n", __func__, method);
        goto err_out;
    }

    handler = NULL;
    list_for_each_entry(tmp_handler, &req_handlers, list, struct http_handler)
    {
        if(tmp_handler->method != req_method){
            continue;
        }

        if(strncmp(path, tmp_handler->path, strlen(tmp_handler->path) + 1)){
            continue;
        }

        handler = tmp_handler;
        break;
    }

    if(handler == NULL){
        printf("[%s] no handler found for method %s and path %s\n",
                __func__, method, path);
        handler = &handler_404;
    }

    result = handler->func(handler, conn, rcv_buff);

    return result;

err_out:
    netbuf_delete(rcv_buff);
    netconn_close(conn);
    netconn_delete(conn);

    return result;
}

TaskHandle_t blinken_task = NULL;
SemaphoreHandle_t blinken_sema = NULL;
volatile u8_t blinken_terminate = 0;
extern p_wlan_init_done_callback;
void blinken_task_fn(void *pvParameters __attribute__((unused)))
{
    struct netconn *work_conn;
    int result;

    /* Create a new tcp connection handle */
    listen_conn = netconn_new(NETCONN_TCP);
    ip_set_option(listen_conn->pcb.ip, SOF_REUSEADDR);
    netconn_bind(listen_conn, NULL, HTTP_PORT);
    netconn_listen(listen_conn);


    /* Load wifi_config */
    wifi_on(RTW_MODE_AP);
    load_wifi_cfg();
    restart_soft_ap();

    result = ERR_OK;
    while(1){
        if(blinken_terminate){
            break;
        }

        /* Wait for connection. */
        port_netconn_accept(listen_conn, work_conn, result);

        if(work_conn != NULL && result == ERR_OK){
            /* Service connection. */
            handle_conn(work_conn);
        }
    }

    if(listen_conn){
        netconn_abort(listen_conn);
        netconn_close(listen_conn);
        netconn_delete(listen_conn);
        listen_conn = NULL;
    }

    /* signal that task has terminated */
    xSemaphoreGive(blinken_sema);
}

#define STACKSIZE				512
int blinken_server_init(void)
{
    BaseType_t result;
    struct http_handler *handler;
    unsigned int i;

    p_wlan_init_done_callback = NULL;

    printf("[%s] Starting web server.\n", __func__);

    blinken_terminate = 0;

    INIT_LIST_HEAD(&req_handlers);
    for(i = 0;i < ARRAY_SIZE(local_handlers);++i){
        list_add_tail(&(local_handlers[i].list), &req_handlers);
    }

    if(blinken_sema == NULL){
        blinken_sema = xSemaphoreCreateCounting(0xffffffff, 0);
    }

    if(blinken_task == NULL){
        result = xTaskCreate(blinken_task_fn,
                             "blinken_server",
                             STACKSIZE,
                             NULL,
                             tskIDLE_PRIORITY + 2,
                             &blinken_task);

        if(result != pdPASS){
            printf("[%s] Creating blinkserver task failed!\n", __func__);
        }
    }

    return 0;
}

void start_blinken_server()
{
    p_wlan_init_done_callback = blinken_server_init;
    wlan_network();
}

void stop_blinken_server()
{
    BaseType_t result;

    blinken_terminate = 1;

    if(blinken_sema){
        result = xSemaphoreTake(blinken_sema, 15 * configTICK_RATE_HZ);
        if(result != pdTRUE){
            if(listen_conn){
                netconn_abort(listen_conn);
                netconn_close(listen_conn);
                netconn_delete(listen_conn);
                listen_conn = NULL;
            }
            printf("[%s] Taking blinken sema(%p) failed!\n",
                    __func__, blinken_sema);
        }

        vSemaphoreDelete(blinken_sema);
        blinken_sema = NULL;
    }

    if(blinken_task){
        vTaskDelete(blinken_task);
        blinken_task = NULL;
    }
    printf("[%s] Web server exited.\n", __func__);
}
