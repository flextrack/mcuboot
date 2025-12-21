#include <zephyr/fs/fs.h>
#include <zephyr/dfu/flash_img.h>
#include <ff.h>

#include "bootutil/bootutil_log.h"

BOOT_LOG_MODULE_REGISTER(myFat);

#define DISK_LABEL "PARROT"

#define FIRMWARE_IMAGE_FILENAME "fw.bin"

static BYTE work[FF_MAX_SS];

#define FILENAME_PATH_SIZE (128)
char full_filename[FILENAME_PATH_SIZE];
char firmware_buf[CONFIG_IMG_BLOCK_BUF_SIZE];

int myFat_installFirmwareFromFatFile(uint8_t upload_slot)
{
    int rc;
    struct fs_mount_t mnt;
    static FATFS fat_fs;

    memset(&mnt, 0, sizeof(mnt));
    memset(&fat_fs, 0, sizeof(fat_fs));

    mnt.type = FS_FATFS;
    mnt.fs_data = &fat_fs;
    mnt.mnt_point = "/NAND:";

    BOOT_LOG_INF("Checking if new firmware image is waiting in FAT partition");

    rc = fs_mount(&mnt);
    if (rc < 0)
    {
        BOOT_LOG_ERR("Failed to open FAT filesystem");
        return -1;
    }

    struct flash_img_context flash_img_ctx;

    rc = flash_img_init_id(&flash_img_ctx, upload_slot);
    if (rc != 0)
    {
        BOOT_LOG_ERR("Image context init failure %u", upload_slot);
        fs_unmount(&mnt);
        return -1;
    }

    snprintf(full_filename, FILENAME_PATH_SIZE, "%s/%s", mnt.mnt_point, FIRMWARE_IMAGE_FILENAME);

    struct fs_file_t fs_file_image;
    fs_file_t_init(&fs_file_image);
    rc = fs_open(&fs_file_image, full_filename, FS_O_READ);
    if (rc < 0)
    {
        BOOT_LOG_INF("No new firmware image file \"%s\" on FAT partition", FIRMWARE_IMAGE_FILENAME);
        fs_unmount(&mnt);
        return -1;
    }
    else
    {
        BOOT_LOG_INF("New firmware image file \"%s\" found!", FIRMWARE_IMAGE_FILENAME);
    }

    int written = 0;
    int log_counter = 0;

    while (1)
    {
        int bytes_read = fs_read(&fs_file_image, firmware_buf, CONFIG_IMG_BLOCK_BUF_SIZE);
        if (bytes_read >= 0)
        {
            bool flush_remainder = bytes_read != CONFIG_IMG_BLOCK_BUF_SIZE;
            rc = flash_img_buffered_write(&flash_img_ctx, firmware_buf, bytes_read, flush_remainder);
            if (rc == 0)
            {
                written += bytes_read;
                if (!(log_counter++ % 100))
                {
                    BOOT_LOG_INF("Wrote %dB", written);
                    MCUBOOT_WATCHDOG_FEED();
                }
                if (flush_remainder)
                {
                    break;
                }
            }
            else
            {
                BOOT_LOG_ERR("Failed to write data to slot %u", upload_slot);
                break;
            }
        }
        else
        {
            BOOT_LOG_ERR("Failed to read data from \"%s\"", FIRMWARE_IMAGE_FILENAME);
        }
    }

    written = flash_img_bytes_written(&flash_img_ctx);
    BOOT_LOG_INF("Wrote %d bytes", written);

    fs_close(&fs_file_image);

    rc = fs_unlink(full_filename);
    if (rc < 0)
    {
        BOOT_LOG_ERR("Failed to remove firmware upgrade file \"%s\"", full_filename);
    }
    else
    {
        BOOT_LOG_INF("Removed firmware upgrade file \"%s\" after installation", full_filename);
    }

    fs_unmount(&mnt);

    return 0;
}

int myFat_setupUsbMscDisk(void)
{
    struct fs_mount_t mnt;
    int rc;
    char label[35];
    FATFS fat_fs;

    memset(&mnt, 0, sizeof(mnt));
    memset(&fat_fs, 0, sizeof(fat_fs));

    mnt.type = FS_FATFS;
    mnt.fs_data = &fat_fs;
    mnt.mnt_point = "/NAND:";

    rc = fs_mount(&mnt);
    if (rc < 0)
    {
        BOOT_LOG_WRN("Failed to mount, creating new FAT filesystem");
        rc = f_mkfs("/NAND", NULL, work, sizeof(work));
        if (rc < 0)
        {
            BOOT_LOG_ERR("Failed to create new FAT filesystem!");
            return -1;
        }
        else
        {
            rc = fs_mount(&mnt);
            if (rc == 0)
            {
                BOOT_LOG_INF("New filesystem is mountable");
            }
            else
            {
                BOOT_LOG_ERR("Failed to mount newly created filesystem!");
                return -1;
            }
        }
    }

    rc = f_getlabel("", label, NULL);
    if (rc < 0)
    {
        BOOT_LOG_ERR("Failed to get volume label (%d)", rc);
    }
    else
    {
        BOOT_LOG_INF("Current volume label: %s", label);
        if (strcmp(label, DISK_LABEL) != 0)
        {
            BOOT_LOG_INF("Changing volume label to: %s", DISK_LABEL);
            rc = f_setlabel(DISK_LABEL);
            if (rc < 0)
            {
                BOOT_LOG_ERR("Failed to set volume label (%d)", rc);
            }
        }
    }

    fs_unmount(&mnt);

    return 0;
}
