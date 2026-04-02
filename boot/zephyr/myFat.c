#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <ff.h>

#include "myFoilLeds.h"

#include "bootutil/bootutil_log.h"
#include "bootutil/image.h"
#include "flash_map_backend/flash_map_backend.h"

BOOT_LOG_MODULE_REGISTER(myFat);

#define DISK_LABEL "PARROT"

#define FIRMWARE_IMAGE_FILENAME "fw.bin"

static BYTE work[FF_MAX_SS];

static FATFS fat_fs;
static struct fs_mount_t mnt = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = "/NOR:",
};

#define FILENAME_PATH_SIZE (128)
char full_filename[FILENAME_PATH_SIZE];
static uint8_t firmware_buf[CONFIG_IMG_BLOCK_BUF_SIZE];
static uint8_t flash_buf[CONFIG_IMG_BLOCK_BUF_SIZE];

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

static size_t myFat_min_size(size_t lhs, size_t rhs)
{
    return (lhs < rhs) ? lhs : rhs;
}

static int myFat_prepareChunk(struct fs_file_t *file,
                              size_t image_size,
                              off_t offset,
                              uint8_t erased_val,
                              uint8_t *buffer,
                              size_t chunk_size,
                              size_t *payload_size)
{
    size_t to_read = 0U;
    int rc;
    int read_len;

    if ((file == NULL) || (buffer == NULL) || (payload_size == NULL))
    {
        return -EINVAL;
    }

    memset(buffer, erased_val, chunk_size);

    if ((offset >= 0) && ((size_t)offset < image_size))
    {
        to_read = myFat_min_size(chunk_size, image_size - (size_t)offset);
        rc = fs_seek(file, offset, FS_SEEK_SET);
        if (rc < 0)
        {
            return rc;
        }

        read_len = fs_read(file, buffer, to_read);
        if (read_len < 0)
        {
            return read_len;
        }
        if ((size_t)read_len != to_read)
        {
            return -EIO;
        }
    }

    *payload_size = to_read;
    return 0;
}

static int myFat_sectorMatchesImage(struct fs_file_t *file,
                                    const struct flash_area *fa,
                                    const struct flash_sector *sector,
                                    size_t image_size,
                                    uint8_t erased_val,
                                    bool *matches)
{
    off_t offset;
    off_t sector_end;
    int rc;

    if ((file == NULL) || (fa == NULL) || (sector == NULL) || (matches == NULL))
    {
        return -EINVAL;
    }

    *matches = false;
    offset = sector->fs_off;
    sector_end = sector->fs_off + (off_t)sector->fs_size;

    while (offset < sector_end)
    {
        size_t chunk_size = myFat_min_size(sizeof(firmware_buf), (size_t)(sector_end - offset));
        size_t payload_size;

        rc = myFat_prepareChunk(file, image_size, offset, erased_val, firmware_buf, chunk_size, &payload_size);
        if (rc != 0)
        {
            return rc;
        }

        rc = flash_area_read(fa, offset, flash_buf, chunk_size);
        if (rc != 0)
        {
            return rc;
        }

        if (memcmp(firmware_buf, flash_buf, chunk_size) != 0)
        {
            return 0;
        }

        offset += (off_t)chunk_size;
    }

    *matches = true;
    return 0;
}

static int myFat_programSector(struct fs_file_t *file,
                               const struct flash_area *fa,
                               const struct flash_sector *sector,
                               size_t image_size,
                               uint8_t erased_val,
                               size_t *written_bytes)
{
    off_t offset;
    off_t sector_end;
    uint32_t align;
    int rc;

    if ((file == NULL) || (fa == NULL) || (sector == NULL) || (written_bytes == NULL))
    {
        return -EINVAL;
    }

    align = flash_area_align(fa);
    if (align == 0U)
    {
        align = 1U;
    }

    if (flash_area_erase_required(fa))
    {
        rc = flash_area_erase(fa, sector->fs_off, sector->fs_size);
        if (rc != 0)
        {
            return rc;
        }
    }

    offset = sector->fs_off;
    sector_end = sector->fs_off + (off_t)sector->fs_size;

    while ((offset < sector_end) && ((size_t)offset < image_size))
    {
        size_t payload_size = myFat_min_size(sizeof(firmware_buf), image_size - (size_t)offset);
        size_t sector_remaining = (size_t)(sector_end - offset);
        size_t write_size;

        payload_size = myFat_min_size(payload_size, sector_remaining);
        write_size = payload_size;
        if ((write_size % align) != 0U)
        {
            write_size += align - (write_size % align);
        }

        rc = myFat_prepareChunk(file, image_size, offset, erased_val, firmware_buf, write_size, &payload_size);
        if (rc != 0)
        {
            return rc;
        }

        rc = flash_area_write(fa, offset, firmware_buf, write_size);
        if (rc != 0)
        {
            return rc;
        }

        *written_bytes += payload_size;
        offset += (off_t)payload_size;
    }

    return 0;
}

int myFat_installFirmwareFromFatFile(uint8_t upload_slot)
{
    int rc;
    struct fs_dirent entry;
    const struct flash_area *upload_area = NULL;
    uint8_t erased_val;
    size_t processed = 0U;
    size_t written = 0U;
    size_t skipped = 0U;
    unsigned int compared_sectors = 0U;
    unsigned int written_sectors = 0U;
    unsigned int skipped_sectors = 0U;

    memset(&fat_fs, 0, sizeof(fat_fs));

    // BOOT_LOG_INF("Checking if new firmware image is waiting in FAT partition");

    rc = fs_mount(&mnt);
    if (rc < 0)
    {
        BOOT_LOG_ERR("Failed to open FAT filesystem");
        return -1;
    }

    rc = flash_area_open(upload_slot, &upload_area);
    if (rc != 0)
    {
        BOOT_LOG_ERR("Failed to open upload slot %u", upload_slot);
        fs_unmount(&mnt);
        return -1;
    }

    erased_val = flash_area_erased_val(upload_area);

    snprintf(full_filename, FILENAME_PATH_SIZE, "%s/%s", mnt.mnt_point, FIRMWARE_IMAGE_FILENAME);

    struct fs_file_t fs_file_image;
    fs_file_t_init(&fs_file_image);
    int ret = fs_stat(full_filename, &entry);
    if (ret == 0)
    {
        char size_buf[16];

        if (entry.size > upload_area->fa_size)
        {
            BOOT_LOG_ERR("Firmware image too large: %u > slot size %u",
                         (unsigned int)entry.size,
                         (unsigned int)upload_area->fa_size);
            flash_area_close(upload_area);
            fs_unmount(&mnt);
            return -1;
        }

        fmt_kb_mb(size_buf, sizeof(size_buf), entry.size);
        BOOT_LOG_WRN("New firmware image file \"%s\" found! (%s)", FIRMWARE_IMAGE_FILENAME, size_buf);
        rc = fs_open(&fs_file_image, full_filename, FS_O_READ);
        if (rc < 0)
        {
            BOOT_LOG_ERR("Failed to open firmware image file \"%s\" for reading", full_filename);
            flash_area_close(upload_area);
            fs_unmount(&mnt);
            return -1;
        }
    }
    else
    {
        flash_area_close(upload_area);
        fs_unmount(&mnt);
        return -1;
    }

    while (processed < entry.size)
    {
        struct flash_sector sector;
        bool matches;
        size_t sector_image_end;

        rc = flash_area_get_sector(upload_area, (off_t)processed, &sector);
        if (rc != 0)
        {
            BOOT_LOG_ERR("Failed to get flash sector at offset %u rc=%d", (unsigned int)processed, rc);
            break;
        }

        rc = myFat_sectorMatchesImage(&fs_file_image, upload_area, &sector, entry.size, erased_val, &matches);
        if (rc != 0)
        {
            BOOT_LOG_ERR("Failed to compare sector at offset %u rc=%d", (unsigned int)sector.fs_off, rc);
            break;
        }

        compared_sectors++;

        if (!matches)
        {
            rc = myFat_programSector(&fs_file_image, upload_area, &sector, entry.size, erased_val, &written);
            if (rc != 0)
            {
                BOOT_LOG_ERR("Failed to write changed sector at offset %u rc=%d",
                             (unsigned int)sector.fs_off,
                             rc);
                break;
            }

            written_sectors++;
        }
        else
        {
            sector_image_end = myFat_min_size((size_t)sector.fs_off + sector.fs_size, entry.size);
            skipped += sector_image_end - (size_t)sector.fs_off;
            skipped_sectors++;
        }

        processed = myFat_min_size((size_t)sector.fs_off + sector.fs_size, entry.size);
        log_progress_line((unsigned int)processed, entry.size);
        myFoilLeds_setState(LED_FOIL_TOGGLE_BOTH);
        MCUBOOT_WATCHDOG_FEED();
    }
    printk("\n");

    myFoilLeds_setState(LED_FOIL_OFF);
    BOOT_LOG_INF("Compared %u bytes, written %u bytes, skipped %u bytes",
                 (unsigned int)processed,
                 (unsigned int)written,
                 (unsigned int)skipped);
    BOOT_LOG_INF("Compared %u sectors, wrote %u sectors, skipped %u sectors",
                 compared_sectors,
                 written_sectors,
                 skipped_sectors);

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
    flash_area_close(upload_area);

    if (processed < entry.size)
    {
        return -1;
    }

    if (entry.size < sizeof(struct image_header))
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
        rc = f_mkfs("/NOR", NULL, work, sizeof(work));
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
