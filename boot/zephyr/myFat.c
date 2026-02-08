#include <zephyr/fs/fs.h>
#include <zephyr/dfu/flash_img.h>
#include <ff.h>

#include "myFoilLeds.h"

#include "bootutil/bootutil_log.h"
#include "bootutil/image.h"

BOOT_LOG_MODULE_REGISTER(myFat);

#define DISK_LABEL "PARROT"

#define FIRMWARE_IMAGE_FILENAME "fw.bin"

static BYTE work[FF_MAX_SS];

static FATFS fat_fs;
static struct fs_mount_t mnt = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = "/NAND:",
};

#define FILENAME_PATH_SIZE (128)
char full_filename[FILENAME_PATH_SIZE];
char firmware_buf[CONFIG_IMG_BLOCK_BUF_SIZE];

static inline void fmt_bytes(char *out, size_t out_sz, uint32_t bytes)
{
    if (bytes < 1024U)
    {
        snprintk(out, out_sz, "%u B", (unsigned)bytes);
    }
    else if (bytes < (1024U * 1024U))
    {
        /* KiB with 1 decimal place */
        unsigned kib_int = bytes >> 10;                     /* /1024 */
        unsigned kib_frac = ((bytes & 0x3FFU) * 10U) >> 10; /* *10 /1024 */

        snprintk(out, out_sz, "%u.%u KiB", kib_int, kib_frac);
    }
    else
    {
        /* MiB with 2 decimal places */
        unsigned mib_int = bytes >> 20;                        /* /1MiB */
        unsigned mib_frac = ((bytes & 0xFFFFFU) * 100U) >> 20; /* *100 /1MiB */

        snprintk(out, out_sz, "%u.%02u MiB", mib_int, mib_frac);
    }
}

static void fmt_kb_mb(char *buf, unsigned int buf_len, unsigned int bytes)
{
    if (bytes < (1024 * 1024))
    {
        /* KB */
        int kb = (bytes + 1023) / 1024; /* zaokrąglenie w górę */
        snprintf(buf, buf_len, "%u KB", kb);
    }
    else
    {
        /* MB with 2 decimal places */
        unsigned int mb_int = bytes / (1024 * 1024);
        unsigned int mb_frac = (bytes % (1024 * 1024)) * 100 / (1024 * 1024);

        snprintf(buf, buf_len, "%u.%02u MB", mb_int, mb_frac);
    }
}

static void log_progress_line(unsigned int written, unsigned int total)
{
    char line[96];
    char bar[21]; /* 20 + '\0' */
    char wbuf[16];
    char tbuf[16];

    int percent = total ? (int)((written * 100U) / total) : 0;
    int bars = percent / 5; /* 0..20 */

    for (int i = 0; i < 20; i++)
    {
        bar[i] = (i < bars) ? '#' : ' ';
    }
    bar[20] = '\0';

    fmt_kb_mb(wbuf, sizeof(wbuf), written);
    fmt_kb_mb(tbuf, sizeof(tbuf), total);

    /* bar ma zawsze dokładnie 20 znaków, więc nie potrzeba %-20s */
    (void)snprintf(line, sizeof(line), "Flashing [%s] %3d%%  %s / %s", bar, percent, wbuf, tbuf);

    printk("\r%s", line);
}

int myFat_installFirmwareFromFatFile(uint8_t upload_slot)
{
    int rc;
    struct fs_dirent entry;

    memset(&fat_fs, 0, sizeof(fat_fs));

    // BOOT_LOG_INF("Checking if new firmware image is waiting in FAT partition");

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
        // BOOT_LOG_INF("No new firmware image file \"%s\" on FAT partition", FIRMWARE_IMAGE_FILENAME);
        fs_unmount(&mnt);
        return -1;
    }
    else
    {

        rc = fs_stat(full_filename, &entry);
        if (rc < 0)
        {
            // BOOT_LOG_ERR("fs_stat failed (%d)", rc);
            fs_unmount(&mnt);
            return -1;
        }
        char size_buf[16];
        fmt_kb_mb(size_buf, sizeof(size_buf), entry.size);
        BOOT_LOG_INF("New firmware image file \"%s\" found! (%s)", FIRMWARE_IMAGE_FILENAME, size_buf);
    }

    int written = 0;

    while (1)
    {
        int bytes_read = fs_read(&fs_file_image, firmware_buf, CONFIG_IMG_BLOCK_BUF_SIZE);
        if (bytes_read > 0)
        {
            bool flush_remainder = bytes_read != CONFIG_IMG_BLOCK_BUF_SIZE;
            rc = flash_img_buffered_write(&flash_img_ctx, firmware_buf, bytes_read, flush_remainder);
            if (rc == 0)
            {
                written += bytes_read;

                log_progress_line(written, entry.size);
                myFoilLeds_setState(LED_FOIL_TOGGLE_BOTH);
                MCUBOOT_WATCHDOG_FEED();

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
            break;
        }
    }
    printk("\n");

    myFoilLeds_setState(LED_FOIL_OFF);
    written = flash_img_bytes_written(&flash_img_ctx);
    BOOT_LOG_INF("Written %d bytes", written);

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

    if (written < sizeof(struct image_header))
    {
        BOOT_LOG_ERR("Invalid image: image size < image header!");
        return -1;
    }

    return 0;
}

int myFat_setupUsbMscDisk(void)
{
    int rc;
    char label[35];

    memset(&fat_fs, 0, sizeof(fat_fs));

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
        BOOT_LOG_ERR("Failed to get disk label (%d)", rc);
    }
    else
    {
        BOOT_LOG_INF("Disk label: %s", label);
        if (strcmp(label, DISK_LABEL) != 0)
        {
            BOOT_LOG_INF("Changing disk label to: %s", DISK_LABEL);
            rc = f_setlabel(DISK_LABEL);
            if (rc < 0)
            {
                BOOT_LOG_ERR("Failed to set disk label (%d)", rc);
            }
        }
    }

    fs_unmount(&mnt);

    return 0;
}
