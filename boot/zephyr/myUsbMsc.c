#include <stdio.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/bos.h>
#include <zephyr/fs/fs.h>
#include <zephyr/device.h>

#include <ff.h>

#include "bootutil/bootutil_log.h"
BOOT_LOG_MODULE_REGISTER(myUsbMsc);

USBD_DEFINE_MSC_LUN(nand, "NAND", "Vestfrost", "EMD", "1.00");

#define FLEXTRACK_USB_VID 0x2fe3
#define FLEXTRACK_USBD_PID 0x0008

#define USBD_MAX_POWER 50

#define DESC_USBD_PRODUCT "EMD"
#define DESC_USBD_MANUFACTURER "Vestfrost"

//
static bool usb_initialized = false;

/* By default, do not register the USB DFU class DFU mode instance. */
static const char *const blocklist[] = {
    "dfu_dfu",
    NULL,
};

/* doc device instantiation start */
/*
 * Instantiate a context named sample_usbd using the default USB device
 * controller, the Zephyr project vendor ID, and the sample product ID.
 * Zephyr project vendor ID must not be used outside of Zephyr samples.
 */
USBD_DEVICE_DEFINE(flex_usbd_context,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_usb)),
                   FLEXTRACK_USB_VID, FLEXTRACK_USBD_PID);
/* doc device instantiation end */

/* doc string instantiation start */
USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, DESC_USBD_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(sample_product, DESC_USBD_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(sample_sn)));

/* doc string instantiation end */

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

/* doc configuration instantiation start */
static const uint8_t attributes = (IS_ENABLED(CONFIG_SAMPLE_USBD_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0) |
                                  (IS_ENABLED(CONFIG_SAMPLE_USBD_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0);

/* Full speed configuration */
USBD_CONFIGURATION_DEFINE(sample_fs_config,
                          attributes,
                          USBD_MAX_POWER, &fs_cfg_desc);

/* High speed configuration */
USBD_CONFIGURATION_DEFINE(sample_hs_config,
                          attributes,
                          USBD_MAX_POWER, &hs_cfg_desc);
/* doc configuration instantiation end */

#if CONFIG_SAMPLE_USBD_20_EXTENSION_DESC
/*
 * This does not yet provide valuable information, but rather serves as an
 * example, and will be improved in the future.
 */
static const struct usb_bos_capability_lpm bos_cap_lpm = {
    .bLength = sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes = 0UL,
};

USBD_DESC_BOS_DEFINE(sample_usbext, sizeof(bos_cap_lpm), &bos_cap_lpm);
#endif

static void fix_code_triple(struct usbd_context *uds_ctx,
                            const enum usbd_speed speed)
{
    /* Always use class code information from Interface Descriptors */
    if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS) ||
        IS_ENABLED(CONFIG_USBD_CDC_ECM_CLASS) ||
        IS_ENABLED(CONFIG_USBD_CDC_NCM_CLASS) ||
        IS_ENABLED(CONFIG_USBD_MIDI2_CLASS) ||
        IS_ENABLED(CONFIG_USBD_AUDIO2_CLASS) ||
        IS_ENABLED(CONFIG_USBD_VIDEO_CLASS))
    {
        /*
         * Class with multiple interfaces have an Interface
         * Association Descriptor available, use an appropriate triple
         * to indicate it.
         */
        usbd_device_set_code_triple(uds_ctx, speed,
                                    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    }
    else
    {
        usbd_device_set_code_triple(uds_ctx, speed, 0, 0, 0);
    }
}

struct usbd_context *usbd_setup_device(usbd_msg_cb_t msg_cb)
{
    int err;

    /* doc add string descriptor start */
    err = usbd_add_descriptor(&flex_usbd_context, &sample_lang);
    if (err)
    {
        LOG_ERR("Failed to initialize language descriptor (%d)", err);
        return NULL;
    }

    err = usbd_add_descriptor(&flex_usbd_context, &sample_mfr);
    if (err)
    {
        LOG_ERR("Failed to initialize manufacturer descriptor (%d)", err);
        return NULL;
    }

    err = usbd_add_descriptor(&flex_usbd_context, &sample_product);
    if (err)
    {
        LOG_ERR("Failed to initialize product descriptor (%d)", err);
        return NULL;
    }

    IF_ENABLED(CONFIG_HWINFO, (err = usbd_add_descriptor(&flex_usbd_context, &sample_sn);))
    if (err)
    {
        LOG_ERR("Failed to initialize SN descriptor (%d)", err);
        return NULL;
    }
    /* doc add string descriptor end */

    if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&flex_usbd_context) == USBD_SPEED_HS)
    {
        err = usbd_add_configuration(&flex_usbd_context, USBD_SPEED_HS, &sample_hs_config);
        if (err)
        {
            LOG_ERR("Failed to add High-Speed configuration");
            return NULL;
        }

        err = usbd_register_all_classes(&flex_usbd_context, USBD_SPEED_HS, 1, blocklist);
        if (err)
        {
            LOG_ERR("Failed to add register classes");
            return NULL;
        }

        fix_code_triple(&flex_usbd_context, USBD_SPEED_HS);
    }

    /* doc configuration register start */
    err = usbd_add_configuration(&flex_usbd_context, USBD_SPEED_FS, &sample_fs_config);
    if (err)
    {
        LOG_ERR("Failed to add Full-Speed configuration");
        return NULL;
    }
    /* doc configuration register end */

    /* doc functions register start */
    err = usbd_register_all_classes(&flex_usbd_context, USBD_SPEED_FS, 1, blocklist);
    if (err)
    {
        LOG_ERR("Failed to add register classes");
        return NULL;
    }
    /* doc functions register end */

    fix_code_triple(&flex_usbd_context, USBD_SPEED_FS);
    usbd_self_powered(&flex_usbd_context, attributes & USB_SCD_SELF_POWERED);

    if (msg_cb != NULL)
    {
        /* doc device init-and-msg start */
        err = usbd_msg_register_cb(&flex_usbd_context, msg_cb);
        if (err)
        {
            LOG_ERR("Failed to register message callback");
            return NULL;
        }
        /* doc device init-and-msg end */
    }

#if CONFIG_SAMPLE_USBD_20_EXTENSION_DESC
    (void)usbd_device_set_bcd_usb(&flex_usbd_context, USBD_SPEED_FS, 0x0201);
    (void)usbd_device_set_bcd_usb(&flex_usbd_context, USBD_SPEED_HS, 0x0201);

    err = usbd_add_descriptor(&flex_usbd_context, &sample_usbext);
    if (err)
    {
        LOG_ERR("Failed to add USB 2.0 Extension Descriptor");
        return NULL;
    }
#endif

    return &flex_usbd_context;
}

bool myUsbMsc_isConnected(void)
{
    if (!usb_initialized)
    {
        LOG_WRN("USB not initialized, cannot get connection state");
        return false;
    }

#define USB_VBUS_DET_STAT_OFF (0x1C0)
#define VBUS_VALID (1 << 3)

    uint32_t base_addr = DT_REG_ADDR(DT_NODELABEL(anatop));
    bool connected = *(uint32_t *)(base_addr + USB_VBUS_DET_STAT_OFF) & (1 << 3);
    return connected;
}

int myUsbMsc_init(struct usbd_context *ctx, usbd_msg_cb_t msg_cb)
{
    int ret;

    ctx = usbd_setup_device(msg_cb);
    if (ctx == NULL)
    {
        LOG_ERR("Failed to setup USB device");
        return 0;
    }

    ret = usbd_init(ctx);
    if (ret)
    {
        LOG_ERR("Failed to initialize USB device");
        return ret;
    }

    ret = usbd_enable(ctx);
    if (ret)
    {
        LOG_ERR("Failed to enable USB device");
        return ret;
    }

    usb_initialized = true;

    return 0;
}
