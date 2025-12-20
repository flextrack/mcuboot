#include <zephyr/fs/fs.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include "bootutil/mcuboot_status.h"

#include "bootutil/bootutil.h"
#include "bootutil/bootutil_log.h"
#include "bootutil/fault_injection_hardening.h"

#include "mcuboot_config/mcuboot_config.h"

#include "myUsbMsc.h"
#include "myFat.h"
#include "myAps823.h"
#include <parrotBootstate.h>

BOOT_LOG_MODULE_REGISTER(myHooks);

static uint8_t slot_flash_area_ids[SLOT_NUM] = {
	FIXED_PARTITION_ID(slot0_partition),
	FIXED_PARTITION_ID(slot1_partition),
};

#define BOOT_TMPBUF_SZ 256
static uint8_t tmpbuf[BOOT_TMPBUF_SZ];
static struct image_header hdr;

static struct usbd_context *usb_ctx;

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (msg->type == USBD_MSG_CONFIGURATION)
	{
		LOG_INF("USB configured");
	}

	if (usbd_can_detect_vbus(ctx))
	{
		if (msg->type == USBD_MSG_VBUS_READY)
		{
			BOOT_LOG_INF("USB VBUS ready - device connected");
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED)
		{
			BOOT_LOG_INF("USB VBUS removed - device disconnected");
		}
	}
}

uint8_t get_flash_area_id(int slot)
{
	if (IS_SLOT_VALID(slot))
	{
		return slot_flash_area_ids[slot];
	}
	else
	{
		return INVALID_SLOT_NUM;
	}
}

int get_inactive_slot(int active_slot)
{
	if (IS_SLOT_VALID(active_slot))
	{
		return !active_slot;
	}
	else
	{
		return INVALID_SLOT_NUM;
	}
}

int load_slot_bootinfo(int slot, struct boot_rsp *rsp)
{
	struct boot_loader_state *state;
	FIH_DECLARE(fih_rc, FIH_FAILURE);
	const struct flash_area *fa_p;

	int flash_id = get_flash_area_id(slot);
	if (flash_id < 0)
	{
		return -EINVAL;
	}

	int rc = flash_area_open(flash_id, &fa_p);
	if (rc != 0)
	{
		BOOT_LOG_ERR("Failed to open flash id %d (%d)", flash_id, rc);
		return rc;
	}

	rc = boot_image_load_header(fa_p, &hdr);
	if (rc != 0)
	{
		BOOT_LOG_ERR("Failed to read image header (%d)", rc);
		flash_area_close(fa_p);
		return rc;
	}

	state = boot_get_loader_state();

	rc = boot_load_image_from_flash_to_sram(state, &hdr, fa_p);
	if (rc != 0)
	{
		BOOT_LOG_ERR("Failed to read image to RAM (%d)", rc);
		flash_area_close(fa_p);
		return rc;
	}

	FIH_CALL(bootutil_img_validate, fih_rc, NULL, &hdr, fa_p, tmpbuf, BOOT_TMPBUF_SZ, NULL, 0, NULL);
	if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS))
	{
		BOOT_LOG_ERR("Failed to validate image (%d)", rc);
		flash_area_close(fa_p);
		boot_remove_image_from_sram(state);
		return -EACCES;
	}

	rsp->br_flash_dev_id = flash_area_get_device_id(fa_p);
	rsp->br_image_off = flash_area_get_off(fa_p);
	rsp->br_hdr = &hdr;

	flash_area_close(fa_p);

	return 0;
}

void pring_big_info(int slot)
{
	static const char *big_zero_string = "\n"
										 "   ###  \n"
										 "  #   # \n"
										 " #     #\n"
										 " #     #\n"
										 " #     #\n"
										 "  #   # \n"
										 "   ###  \n";

	static const char *big_one_string = "\n"
										"   #  \n"
										"  ##  \n"
										" # #  \n"
										"   #  \n"
										"   #  \n"
										"   #  \n"
										" #####\n";

	if (slot == 0)
	{
		BOOT_LOG_INF("%s", big_zero_string);
	}
	else if (slot == 1)
	{
		BOOT_LOG_INF("%s", big_one_string);
	}
	else
	{
		BOOT_LOG_ERR("\n\r !!!\n\r");
	}
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
	// Open "boot_state" partition
	int err = boot_state_init();
	if (err)
	{
		BOOT_LOG_ERR("Failed to open 'boot_state' partition: %d", err);
		return FIH_FAILURE;
	}

	struct boot_state_data bsdata;
	err = boot_state_read(&bsdata);
	if (err)
	{
		BOOT_LOG_ERR("Failed to read boot state (%d)", err);
	}

	int inactive_slot = get_inactive_slot(bsdata.active_slot);

	// Checking for new firmware image on FAT drive
	int installed_from_fat_rc = myFat_installFirmwareFromFatFile(get_flash_area_id(inactive_slot));
	if (installed_from_fat_rc == 0)
	{
		BOOT_LOG_INF("Firmware installed from USB");
		if (bsdata.upgrade_request)
		{
			BOOT_LOG_WRN("If another firmware upgrade was triggered from the app, it will be overwritten!");
		}
		bsdata.upgrade_request = 1;
	}

	// Slot selection logic
	bool bsdata_changed = false;
	if (bsdata.upgrade_request)
	{
		BOOT_LOG_INF("FIRMWARE UPGRADE: Booting from inactive slot, clearing 'upgrade_request' flag");
		bsdata.upgrade_request = 0;
		bsdata.booted_slot = inactive_slot;
		bsdata_changed = true;
	}
	else
	{
		BOOT_LOG_INF("NORMAL BOOT: Booting from active slot");
		/* If 'upgrade_request' is false, this should be true only if previous upgrade was not activated. */
		if (bsdata.booted_slot != bsdata.active_slot)
		{
			bsdata.booted_slot = bsdata.active_slot;
			bsdata_changed = true;
		}
	}

	if (bsdata_changed)
	{
		boot_state_write(&bsdata);
	}

	// Booting from slot 'slot_to_boot'
	BOOT_LOG_INF(" === Booting from slot %d, flash id %d ===", bsdata.booted_slot, get_flash_area_id(bsdata.booted_slot));
	pring_big_info(bsdata.booted_slot);

	// Close "boot_state" partition
	boot_state_deinit();

	err = load_slot_bootinfo(bsdata.booted_slot, rsp);
	if (err)
	{
		BOOT_LOG_ERR("Failed to load boot info (%d)", err);
		return FIH_FAILURE;
	}

	return FIH_SUCCESS;
}

static bool initUsb(void)
{
	// Setup FAT filesystem for USB MSC if corrupted or not set yet
	if (myFat_setupUsbMscDisk() != 0)
	{
		BOOT_LOG_ERR("Failed to setup USB MSC disk");
		return false;
	}

	// Initialize USB MSC
	if (myUsbMsc_init(usb_ctx, usb_msg_cb) != 0)
	{
		BOOT_LOG_ERR("Failed to initialize USB MSC");
		return false;
	}

	LOG_INF("USB initialized successfully");

	return true;
}

void mcuboot_status_change(mcuboot_status_type_t status)
{
	switch (status)
	{
	case MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND:
	case MCUBOOT_STATUS_BOOT_FAILED:
		const char *error_msg_str = "OTHER BOOT ERROR";
		if (status == MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND)
		{
			error_msg_str = "NO BOOTABLE IMAGE FOUND";
		}
		else if (status == MCUBOOT_STATUS_BOOT_FAILED)
		{
			error_msg_str = "BOOT FAILED";
		}
		initUsb();
		while (1)
		{
			BOOT_LOG_ERR("%s! Enabling USB MSC for recovery upgrade!", error_msg_str);
			k_sleep(K_SECONDS(3));
		}
		break;

	default:
		break;
	}
}
