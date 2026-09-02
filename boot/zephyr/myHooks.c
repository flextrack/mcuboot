#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include "bootutil/mcuboot_status.h"

#include "bootutil/bootutil.h"
#include "bootutil/bootutil_log.h"
#include "bootutil/fault_injection_hardening.h"

#include "mcuboot_config/mcuboot_config.h"

#include "myUsbMsc.h"
#include "myFat.h"
#include "myTest.h"
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

uint8_t get_flash_area_id(int slot);

#define EMD_VERSION_SCAN_CHUNK_SIZE 4096U
#define EMD_VERSION_SCAN_OVERLAP 16U
#define EMD_VERSION_MAJOR(version) (((version) >> 24) & 0xffU)
#define EMD_VERSION_MINOR(version) (((version) >> 16) & 0xffU)
#define EMD_VERSION_PATCH(version) ((version) & 0xffffU)

static uint8_t version_scan_buffer[EMD_VERSION_SCAN_CHUNK_SIZE + EMD_VERSION_SCAN_OVERLAP];

static bool version_char_is_digit(uint8_t ch)
{
	return (ch >= '0') && (ch <= '9');
}

static bool parse_version_component(const uint8_t *data, size_t size,
									size_t *position, uint32_t maximum,
									uint32_t *value)
{
	uint32_t parsed = 0U;
	size_t pos = *position;

	if ((pos >= size) || !version_char_is_digit(data[pos]))
	{
		return false;
	}

	while ((pos < size) && version_char_is_digit(data[pos]))
	{
		parsed = (parsed * 10U) + (uint32_t)(data[pos] - '0');
		if (parsed > maximum)
		{
			return false;
		}
		pos++;
	}

	*position = pos;
	*value = parsed;
	return true;
}

static bool parse_version_at(const uint8_t *data, size_t size, size_t start,
							 uint32_t *version)
{
	uint32_t major;
	uint32_t minor;
	uint32_t patch;
	size_t pos = start;

	if ((pos >= size) || (data[pos] != 'v'))
	{
		return false;
	}
	pos++;

	if (!parse_version_component(data, size, &pos, UINT8_MAX, &major) ||
		(pos >= size) || (data[pos++] != '.') ||
		!parse_version_component(data, size, &pos, UINT8_MAX, &minor) ||
		(pos >= size) || (data[pos++] != '.') ||
		!parse_version_component(data, size, &pos, UINT16_MAX, &patch))
	{
		return false;
	}

	*version = (major << 24) | (minor << 16) | patch;
	return true;
}

static int read_slot_emd_version(int slot, uint32_t *version)
{
	const struct flash_area *area;
	struct image_header image_header;
	size_t carry = 0U;
	size_t offset;
	size_t remaining;
	bool found = false;
	int flash_id = get_flash_area_id(slot);
	int rc;

	if ((flash_id < 0) || (version == NULL))
	{
		return -EINVAL;
	}

	rc = flash_area_open(flash_id, &area);
	if (rc != 0)
	{
		return rc;
	}

	rc = boot_image_load_header(area, &image_header);
	if (rc != 0)
	{
		goto out;
	}

	offset = image_header.ih_hdr_size;
	remaining = image_header.ih_img_size;
	if ((offset > area->fa_size) || (remaining > (area->fa_size - offset)))
	{
		rc = -EINVAL;
		goto out;
	}

	*version = 0U;
	while (remaining > 0U)
	{
		size_t read_size = MIN(remaining, EMD_VERSION_SCAN_CHUNK_SIZE);
		size_t scan_size;

		rc = flash_area_read(area, (off_t)offset,
						 version_scan_buffer + carry, read_size);
		if (rc != 0)
		{
			goto out;
		}

		scan_size = carry + read_size;
		for (size_t i = 0U; i < scan_size; i++)
		{
			uint32_t candidate;
			if (parse_version_at(version_scan_buffer, scan_size, i, &candidate) &&
				(!found || (candidate > *version)))
			{
				*version = candidate;
				found = true;
			}
		}

		carry = MIN(scan_size, EMD_VERSION_SCAN_OVERLAP);
		memmove(version_scan_buffer,
				version_scan_buffer + scan_size - carry, carry);
		offset += read_size;
		remaining -= read_size;
		MCUBOOT_WATCHDOG_FEED();
	}

	rc = found ? 0 : -ENOENT;

out:
	flash_area_close(area);
	return rc;
}

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (msg->type == USBD_MSG_CONFIGURATION)
	{
		LOG_INF("USB configured");
		myUsbMsc_speed(ctx);
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
	static const char *big_zero_string = "\r\n"
										 "   ###  \r\n"
										 "  #   # \r\n"
										 " #     #\r\n"
										 " #     #\r\n"
										 " #     #\r\n"
										 "  #   # \r\n"
										 "   ###  \r\n";

	static const char *big_one_string = "\r\n"
										"   #  \r\n"
										"  ##  \r\n"
										" # #  \r\n"
										"   #  \r\n"
										"   #  \r\n"
										"   #  \r\n"
										" #####\r\n";

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
	int err = boot_state_initialize();
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
		BOOT_LOG_INF("New image written to inactive slot (%d) from FAT partition", inactive_slot);
		if (bsdata.upgrade_request)
		{
			BOOT_LOG_WRN("If another firmware upgrade was triggered from the app, it will be overwritten!");
		}
		bsdata.upgrade_request = 1;
	}

	bool downgrade_rejected = false;
	if (bsdata.upgrade_request)
	{
		uint32_t active_version;
		uint32_t candidate_version;
		int active_version_rc = read_slot_emd_version(bsdata.active_slot, &active_version);
		int candidate_version_rc = read_slot_emd_version(inactive_slot, &candidate_version);

		if ((active_version_rc == 0) && (candidate_version_rc == 0))
		{
			BOOT_LOG_INF("Firmware version check: active=v%u.%u.%u candidate=v%u.%u.%u",
						 EMD_VERSION_MAJOR(active_version), EMD_VERSION_MINOR(active_version), EMD_VERSION_PATCH(active_version),
						 EMD_VERSION_MAJOR(candidate_version), EMD_VERSION_MINOR(candidate_version), EMD_VERSION_PATCH(candidate_version));

			if (candidate_version < active_version)
			{
				BOOT_LOG_ERR("Firmware downgrade rejected: v%u.%u.%u < v%u.%u.%u",
							 EMD_VERSION_MAJOR(candidate_version), EMD_VERSION_MINOR(candidate_version), EMD_VERSION_PATCH(candidate_version),
							 EMD_VERSION_MAJOR(active_version), EMD_VERSION_MINOR(active_version), EMD_VERSION_PATCH(active_version));
				bsdata.upgrade_request = 0;
				bsdata.booted_slot = bsdata.active_slot;
				downgrade_rejected = true;
				(void)myFat_markFirmwareRejected("firmware downgrade rejected", -EPERM);
			}
		}
		else
		{
			BOOT_LOG_WRN("Firmware version comparison unavailable: active_rc=%d candidate_rc=%d; continuing existing flow",
						 active_version_rc, candidate_version_rc);
		}
	}

	// Slot selection logic
	bool bsdata_changed = downgrade_rejected;
	if (bsdata.upgrade_request)
	{
		BOOT_LOG_INF("Upgrade request");
		bsdata.upgrade_request = 0;
		bsdata.booted_slot = inactive_slot;
		bsdata_changed = true;
	}
	else
	{
		BOOT_LOG_INF("Normal boot");
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

void mcuboot_status_change(mcuboot_status_type_t status)
{
	static bool once = true;
	int timeout;

	switch (status)
	{
	case MCUBOOT_STATUS_STARTUP:
		LOG_INF("Compiled at %s %s", __DATE__, __TIME__);
		// mytest_show_config_and_clocks();
		MCUBOOT_WATCHDOG_FEED();

		// Setup FAT filesystem for USB MSC if corrupted or not set yet
		if (myFat_setupUsbMscDisk() != 0)
		{
			BOOT_LOG_ERR("Failed to setup USB MSC disk");
		}

		MCUBOOT_WATCHDOG_FEED();

		// Initialize USB MSC
		if (myUsbMsc_init(&usb_ctx, usb_msg_cb) != 0)
		{
			BOOT_LOG_ERR("Failed to initialize USB MSC");
		}

		MCUBOOT_WATCHDOG_FEED();

		// Enable USB MSC
		if (myUsbMsc_enable(usb_ctx) != 0)
		{
			BOOT_LOG_ERR("Failed to enable USB MSC");
		}

		// wait for fw.bin upload over USB MSC
		timeout = 5;
		do
		{
			MCUBOOT_WATCHDOG_FEED();
			k_msleep(100);
			if (!myUsbMsc_isVbusDetected())
			{
				timeout--;
			}
			else if (once)
			{
				once = false;
				LOG_WRN("Flashing mode entered... ");
				LOG_INF("Put fw.bin file to the PARROT drive and disconnect USB cable afterwards.");
			}
		} while (myUsbMsc_isVbusDetected() || (timeout > 0));

		MCUBOOT_WATCHDOG_FEED();
		k_msleep(100);

		// Disable USB MSC
		if (myUsbMsc_disable(usb_ctx) != 0)
		{
			BOOT_LOG_ERR("Failed to disable USB MSC");
		}

		MCUBOOT_WATCHDOG_FEED();

		if (myUsbMsc_shutdown(usb_ctx) != 0)
		{
			BOOT_LOG_ERR("Failed to shutdown USB MSC");
		}

		break;

	case MCUBOOT_STATUS_UPGRADING:
		LOG_INF("Upgrading boot image...");
		break;

	case MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND:
		MCUBOOT_WATCHDOG_FEED();
		LOG_INF("Booting image...");
		break;

	case MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND:
	case MCUBOOT_STATUS_BOOT_FAILED:
		LOG_ERR("%s", (status == MCUBOOT_STATUS_BOOT_FAILED) ? "Boot failed! ;(" : "No bootable image ;(");
		mytest_show_config_and_clocks();
		break;

	default:
		break;
	}
}

void mcuboot_watchdog_setup(void)
{
	myAps823_init();
	myAps823_disableWatchdogPulse();
}

void mcuboot_watchdog_feed(void)
{
	//	myAps823_toggleWatchdog();
	myAps823_disableWatchdogPulse();
}
