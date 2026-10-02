/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Christian Gmeiner
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef USB_SERIAL_H
#define USB_SERIAL_H

#include "usb_class_driver.h"

void usb_serial_send(const unsigned char *data, int length);

/* Stream API (ACM data interface). read is non-blocking and returns bytes copied;
 * write blocks (yielding) until everything is queued, or returns -1 on disconnect. */
int usb_serial_read(void *buf, int maxlen);
int usb_serial_write(const void *data, int length);
bool usb_serial_connected(void);

extern struct usb_class_driver usb_cdrv_serial;

#endif
