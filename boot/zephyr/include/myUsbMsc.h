#ifndef _MY_USBMSC_H_
#define _MY_USBMSC_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <zephyr/usb/usbd.h>

    bool myUsbMsc_isConnected(void);
    int myUsbMsc_init(struct usbd_context *ctx, usbd_msg_cb_t msg_cb);

#ifdef __cplusplus
}
#endif

#endif // _MY_USBMSC_H_
