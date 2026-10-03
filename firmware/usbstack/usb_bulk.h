#ifndef USB_BULK_H
#define USB_BULK_H

#include "usb_class_driver.h"

/* Vendor-specific bulk interface for the companion link (WebUSB). */
#define USB_BULK_SUBCLASS 0x49 /* 'I' */
#define USB_BULK_PROTOCOL 0x50 /* 'P' */

/* The stream API (usb_bulk_read/write/connected/get_stats) is declared in usb.h,
 * alongside the other class drivers' app-facing calls. */
#include "usb.h"

int usb_bulk_interface(void);

extern struct usb_class_driver usb_cdrv_bulk;

#endif
