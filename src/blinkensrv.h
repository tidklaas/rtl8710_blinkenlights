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

#ifndef BASIC_WEB_SERVER_H
#define BASIC_WEB_SERVER_H
#include <wifi/wifi_conf.h>
/*------------------------------------------------------------------------------*/
/*                            MACROS                                             */
/*------------------------------------------------------------------------------*/
#define basicwebBLINKSERVER_PRIORITY      ( tskIDLE_PRIORITY + 2 )

#define lwipBASIC_SERVER_STACK_SIZE	256

/*------------------------------------------------------------------------------*/

/* The function that implements the WEB server task. */
extern void	start_blinken_server(void);
extern void start_blinken_server(void);

#endif  /* 
 */

