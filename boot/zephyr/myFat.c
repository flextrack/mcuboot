#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <ff.h>

#include "myFoilLeds.h"

#include "bootutil/bootutil_log.h"
#include "bootutil/image.h"
#include "flash_map_backend/flash_map_backend.h"

BOOT_LOG_MODULE_REGISTER(myFat);

#define DISK_LABEL "PARROT"
#define DISK_ACCESS_NAME "NOR"

#define FIRMWARE_IMAGE_FILENAME "fw.bin"
#define MKFS_TRIGGER_FILENAME "mkfs.now"

#define EMD_LZ4_MAGIC "PRL4"
#define EMD_LZ4_HEADER_SIZE 16U
#define EMD_LZ4_MAX_CHUNK_SIZE (4U * 1024U)
#define EMD_LZ4_COMPRESSBOUND(size) ((size) + ((size) / 255U) + 16U)
#define EMD_LZ4_MAX_COMPRESSED_CHUNK_SIZE EMD_LZ4_COMPRESSBOUND(EMD_LZ4_MAX_CHUNK_SIZE)

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
static uint8_t lz4_compressed_buf[EMD_LZ4_MAX_COMPRESSED_CHUNK_SIZE];
static uint8_t lz4_decompressed_buf[EMD_LZ4_MAX_CHUNK_SIZE];

static bool fat_file_read_error_detected;

static const char *myFat_typeName(BYTE type)
{
    switch (type)
    {
    case FS_FAT12:
        return "FAT12";
    case FS_FAT16:
        return "FAT16";
    case FS_FAT32:
        return "FAT32";
    case FS_EXFAT:
        return "exFAT";
    default:
        return "unknown";
    }
}

static void myFat_logFilesystemGeometry(void)
{
    BOOT_LOG_INF("FAT %s: sector=%u, cluster=%u sectors (%u bytes), entries=%u",
                 myFat_typeName(fat_fs.fs_type),
                 (unsigned int)fat_fs.ssize,
                 (unsigned int)fat_fs.csize,
                 (unsigned int)(fat_fs.csize * fat_fs.ssize),
                 (unsigned int)fat_fs.n_fatent);
}

static int myFat_syncDiskCache(void)
{
    int rc;

    rc = disk_access_ioctl(DISK_ACCESS_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
    if (rc != 0)
    {
        BOOT_LOG_ERR("Failed to sync FAT disk cache (%d)", rc);
        return rc;
    }

    return 0;
}

static void myFat_noteFileReadError(void)
{
    fat_file_read_error_detected = true;
}

static int myFat_formatAndRemount(const char *reason)
{
    FRESULT fr;
    int rc;

    BOOT_LOG_WRN("Formatting FAT filesystem: %s", reason);
    (void)fs_unmount(&mnt);
    memset(&fat_fs, 0, sizeof(fat_fs));

    fr = f_mkfs("/NOR", NULL, work, sizeof(work));
    if (fr != FR_OK)
    {
        BOOT_LOG_ERR("Failed to create new FAT filesystem (%d)", fr);
        return -1;
    }

    BOOT_LOG_INF("FAT filesystem formatted");
    rc = fs_mount(&mnt);
    if (rc == 0)
    {
        myFat_logFilesystemGeometry();
    }
    else
    {
        BOOT_LOG_ERR("Failed to mount newly formatted filesystem (%d)", rc);
        return -1;
    }

    return 0;
}

static int myFat_formatAfterFileReadError(void)
{
    int rc;

    rc = myFat_formatAndRemount("firmware file read error");
    if (rc == 0)
    {
        fs_unmount(&mnt);
    }

    return rc;
}

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

static int myFat_readExact(struct fs_file_t *file, void *buffer, size_t size)
{
    uint8_t *dst = buffer;
    size_t total = 0U;

    while (total < size)
    {
        int read_len = fs_read(file, dst + total, size - total);
        if (read_len < 0)
        {
            myFat_noteFileReadError();
            return read_len;
        }
        if (read_len == 0)
        {
            myFat_noteFileReadError();
            return -EIO;
        }

        total += (size_t)read_len;
    }

    return 0;
}

static int myFat_lz4DecompressBlock(const uint8_t *src,
                                    size_t src_size,
                                    uint8_t *dst,
                                    size_t dst_capacity)
{
    size_t ip = 0U;
    size_t op = 0U;

    while (ip < src_size)
    {
        uint8_t token = src[ip++];
        size_t literal_len = token >> 4;
        size_t match_len = token & 0x0FU;
        uint16_t offset;
        size_t match_pos;

        if (literal_len == 15U)
        {
            uint8_t value;
            do
            {
                if (ip >= src_size)
                {
                    return -EINVAL;
                }
                value = src[ip++];
                literal_len += value;
            } while (value == 255U);
        }

        if ((literal_len > (src_size - ip)) || (literal_len > (dst_capacity - op)))
        {
            return -EINVAL;
        }

        memcpy(&dst[op], &src[ip], literal_len);
        ip += literal_len;
        op += literal_len;

        if (ip == src_size)
        {
            return (int)op;
        }

        if ((src_size - ip) < 2U)
        {
            return -EINVAL;
        }

        offset = (uint16_t)src[ip] | ((uint16_t)src[ip + 1U] << 8);
        ip += 2U;
        if ((offset == 0U) || (offset > op))
        {
            return -EINVAL;
        }

        if (match_len == 15U)
        {
            uint8_t value;
            do
            {
                if (ip >= src_size)
                {
                    return -EINVAL;
                }
                value = src[ip++];
                match_len += value;
            } while (value == 255U);
        }
        match_len += 4U;

        if (match_len > (dst_capacity - op))
        {
            return -EINVAL;
        }

        match_pos = op - offset;
        for (size_t i = 0; i < match_len; i++)
        {
            dst[op++] = dst[match_pos + i];
        }
    }

    return (int)op;
}

static int myFat_validateInstalledHeader(const struct flash_area *fa, size_t image_size)
{
    struct image_header image_hdr;
    int rc;

    if (image_size < sizeof(image_hdr))
    {
        BOOT_LOG_ERR("Invalid image: image size < image header!");
        return -EINVAL;
    }

    rc = flash_area_read(fa, 0, &image_hdr, sizeof(image_hdr));
    if (rc != 0)
    {
        BOOT_LOG_ERR("Failed to read installed image header (%d)", rc);
        return rc;
    }

    if (image_hdr.ih_magic != IMAGE_MAGIC)
    {
        BOOT_LOG_ERR("Invalid image magic: 0x%08x", image_hdr.ih_magic);
        return -EINVAL;
    }

    if (((size_t)image_hdr.ih_hdr_size + image_hdr.ih_img_size) > image_size)
    {
        BOOT_LOG_ERR("Invalid image size in header: hdr=%u img=%u file=%u",
                     image_hdr.ih_hdr_size,
                     image_hdr.ih_img_size,
                     (unsigned int)image_size);
        return -EINVAL;
    }

    return 0;
}

static int myFat_writeAligned(const struct flash_area *fa,
                              off_t offset,
                              const uint8_t *buffer,
                              size_t size,
                              uint8_t erased_val)
{
    uint32_t align = flash_area_align(fa);
    size_t written = 0U;

    if (align == 0U)
    {
        align = 1U;
    }

    if ((offset < 0) || ((size_t)offset > fa->fa_size) || (size > (fa->fa_size - (size_t)offset)))
    {
        BOOT_LOG_ERR("Refusing flash write outside slot: off=%u size=%u slot=%u",
                     (unsigned int)offset,
                     (unsigned int)size,
                     (unsigned int)fa->fa_size);
        return -EINVAL;
    }

    if ((sizeof(firmware_buf) % align) != 0U)
    {
        BOOT_LOG_ERR("Firmware scratch buffer is not aligned to flash write size");
        return -EINVAL;
    }

    while (written < size)
    {
        size_t payload_size = myFat_min_size(sizeof(firmware_buf), size - written);
        size_t write_size = payload_size;
        int rc;

        if ((write_size % align) != 0U)
        {
            write_size += align - (write_size % align);
        }

        if (write_size > sizeof(firmware_buf))
        {
            return -EINVAL;
        }

        memset(firmware_buf, erased_val, write_size);
        memcpy(firmware_buf, buffer + written, payload_size);

        rc = flash_area_write(fa, offset + (off_t)written, firmware_buf, write_size);
        if (rc != 0)
        {
            return rc;
        }

        MCUBOOT_WATCHDOG_FEED();
        written += payload_size;
    }

    return 0;
}

static int myFat_flashMatchesAligned(const struct flash_area *fa,
                                     off_t offset,
                                     const uint8_t *buffer,
                                     size_t size,
                                     uint8_t erased_val,
                                     bool *matches)
{
    uint32_t align = flash_area_align(fa);
    size_t checked = 0U;

    if (matches == NULL)
    {
        return -EINVAL;
    }

    *matches = false;

    if (align == 0U)
    {
        align = 1U;
    }

    if ((offset < 0) || ((size_t)offset > fa->fa_size) || (size > (fa->fa_size - (size_t)offset)))
    {
        BOOT_LOG_ERR("Refusing flash compare outside slot: off=%u size=%u slot=%u",
                     (unsigned int)offset,
                     (unsigned int)size,
                     (unsigned int)fa->fa_size);
        return -EINVAL;
    }

    if ((sizeof(firmware_buf) % align) != 0U)
    {
        BOOT_LOG_ERR("Firmware scratch buffer is not aligned to flash write size");
        return -EINVAL;
    }

    while (checked < size)
    {
        size_t payload_size = myFat_min_size(sizeof(firmware_buf), size - checked);
        size_t compare_size = payload_size;
        int rc;

        if ((compare_size % align) != 0U)
        {
            compare_size += align - (compare_size % align);
        }

        if (compare_size > sizeof(firmware_buf))
        {
            return -EINVAL;
        }

        if (((size_t)offset + checked + compare_size) > fa->fa_size)
        {
            BOOT_LOG_ERR("Refusing aligned flash compare outside slot");
            return -EINVAL;
        }

        memset(firmware_buf, erased_val, compare_size);
        memcpy(firmware_buf, buffer + checked, payload_size);

        rc = flash_area_read(fa, offset + (off_t)checked, flash_buf, compare_size);
        if (rc != 0)
        {
            return rc;
        }

        if (memcmp(firmware_buf, flash_buf, compare_size) != 0)
        {
            return 0;
        }

        MCUBOOT_WATCHDOG_FEED();
        checked += payload_size;
    }

    *matches = true;
    return 0;
}

static int myFat_ensureErasedForWrite(const struct flash_area *fa,
                                      off_t offset,
                                      size_t size,
                                      size_t *erased_until)
{
    uint32_t align = flash_area_align(fa);
    size_t erase_needed_until;

    if (!flash_area_erase_required(fa))
    {
        return 0;
    }

    if ((erased_until == NULL) || (offset < 0))
    {
        return -EINVAL;
    }

    if (align == 0U)
    {
        align = 1U;
    }

    if (((size_t)offset > fa->fa_size) || (size > (fa->fa_size - (size_t)offset)))
    {
        BOOT_LOG_ERR("Refusing erase outside slot: off=%u size=%u slot=%u",
                     (unsigned int)offset,
                     (unsigned int)size,
                     (unsigned int)fa->fa_size);
        return -EINVAL;
    }

    erase_needed_until = (size_t)offset + size;
    if ((erase_needed_until % align) != 0U)
    {
        erase_needed_until += align - (erase_needed_until % align);
    }

    if (erase_needed_until > fa->fa_size)
    {
        BOOT_LOG_ERR("Refusing aligned erase outside slot: end=%u slot=%u",
                     (unsigned int)erase_needed_until,
                     (unsigned int)fa->fa_size);
        return -EINVAL;
    }

    while (*erased_until < erase_needed_until)
    {
        struct flash_sector sector;
        int rc;

        rc = flash_area_get_sector(fa, (off_t)*erased_until, &sector);
        if (rc != 0)
        {
            BOOT_LOG_ERR("Failed to get erase sector at offset %u (%d)",
                         (unsigned int)*erased_until,
                         rc);
            return rc;
        }

        rc = flash_area_erase(fa, sector.fs_off, sector.fs_size);
        if (rc != 0)
        {
            BOOT_LOG_ERR("Failed to erase sector at offset %u size %u (%d)",
                         (unsigned int)sector.fs_off,
                         (unsigned int)sector.fs_size,
                         rc);
            return rc;
        }

        *erased_until = (size_t)sector.fs_off + sector.fs_size;
        myFoilLeds_setState(LED_FOIL_TOGGLE_BOTH);
        MCUBOOT_WATCHDOG_FEED();
    }

    return 0;
}

static int myFat_installLz4Package(struct fs_file_t *file,
                                   const struct fs_dirent *entry,
                                   const struct flash_area *upload_area,
                                   uint8_t erased_val,
                                   const uint8_t header[EMD_LZ4_HEADER_SIZE])
{
    uint32_t original_size = sys_get_le32(&header[4]);
    uint32_t chunk_size = sys_get_le32(&header[8]);
    uint32_t chunk_count = sys_get_le32(&header[12]);
    uint32_t expected_chunks;
    size_t file_offset = EMD_LZ4_HEADER_SIZE;
    size_t written = 0U;
    size_t programmed = 0U;
    size_t skipped = 0U;
    size_t erased_until = 0U;
    int rc;

    if (original_size > upload_area->fa_size)
    {
        BOOT_LOG_ERR("Decompressed image too large: %u > slot size %u",
                     original_size,
                     (unsigned int)upload_area->fa_size);
        return -EINVAL;
    }

    if ((chunk_size == 0U) || (chunk_size > EMD_LZ4_MAX_CHUNK_SIZE))
    {
        BOOT_LOG_ERR("Unsupported LZ4 chunk size: %u", chunk_size);
        return -EINVAL;
    }

    expected_chunks = (original_size + chunk_size - 1U) / chunk_size;
    if ((chunk_count == 0U) || (chunk_count != expected_chunks))
    {
        BOOT_LOG_ERR("Invalid LZ4 chunk count: %u expected %u", chunk_count, expected_chunks);
        return -EINVAL;
    }

    BOOT_LOG_WRN("LZ4 firmware package detected: %u bytes, %u chunks x %u bytes",
                 original_size,
                 chunk_count,
                 chunk_size);

    BOOT_LOG_INF("Erasing inactive slot as LZ4 chunks are written");

    rc = fs_seek(file, EMD_LZ4_HEADER_SIZE, FS_SEEK_SET);
    if (rc < 0)
    {
        myFat_noteFileReadError();
        return rc;
    }

    for (uint32_t chunk_index = 0; chunk_index < chunk_count; chunk_index++)
    {
        uint8_t size_buf[4];
        uint32_t compressed_size;
        uint32_t expected_size;
        bool matches;
        int decoded_size;

        rc = myFat_readExact(file, size_buf, sizeof(size_buf));
        if (rc != 0)
        {
            return rc;
        }
        file_offset += sizeof(size_buf);

        compressed_size = sys_get_le32(size_buf);
        if ((compressed_size == 0U) || (compressed_size > EMD_LZ4_MAX_COMPRESSED_CHUNK_SIZE))
        {
            BOOT_LOG_ERR("Invalid compressed chunk %u size: %u", chunk_index, compressed_size);
            return -EINVAL;
        }

        if ((file_offset + compressed_size) > (size_t)entry->size)
        {
            BOOT_LOG_ERR("Compressed chunk %u exceeds package size", chunk_index);
            return -EINVAL;
        }

        rc = myFat_readExact(file, lz4_compressed_buf, compressed_size);
        if (rc != 0)
        {
            return rc;
        }
        file_offset += compressed_size;

        expected_size = myFat_min_size(chunk_size, original_size - written);
        if (expected_size > (upload_area->fa_size - written))
        {
            BOOT_LOG_ERR("Refusing decompressed chunk outside slot: written=%u chunk=%u slot=%u",
                         (unsigned int)written,
                         expected_size,
                         (unsigned int)upload_area->fa_size);
            return -EINVAL;
        }

        decoded_size = myFat_lz4DecompressBlock(lz4_compressed_buf,
                                                compressed_size,
                                                lz4_decompressed_buf,
                                                expected_size);
        if (decoded_size != (int)expected_size)
        {
            BOOT_LOG_ERR("Failed to decompress chunk %u (%d, expected %u)",
                         chunk_index,
                         decoded_size,
                         expected_size);
            return -EINVAL;
        }

        rc = myFat_flashMatchesAligned(upload_area,
                                       (off_t)written,
                                       lz4_decompressed_buf,
                                       expected_size,
                                       erased_val,
                                       &matches);
        if (rc != 0)
        {
            BOOT_LOG_ERR("Failed to compare decompressed chunk %u at offset %u (%d)",
                         chunk_index,
                         (unsigned int)written,
                         rc);
            return rc;
        }

        if (matches)
        {
            skipped += expected_size;
            written += expected_size;
            log_progress_line((unsigned int)written, original_size);
            myFoilLeds_setState(LED_FOIL_TOGGLE_BOTH);
            MCUBOOT_WATCHDOG_FEED();
            continue;
        }

        rc = myFat_ensureErasedForWrite(upload_area,
                                        (off_t)written,
                                        expected_size,
                                        &erased_until);
        if (rc != 0)
        {
            return rc;
        }

        rc = myFat_writeAligned(upload_area,
                                (off_t)written,
                                lz4_decompressed_buf,
                                expected_size,
                                erased_val);
        if (rc != 0)
        {
            BOOT_LOG_ERR("Failed to write decompressed chunk %u at offset %u (%d)",
                         chunk_index,
                         (unsigned int)written,
                         rc);
            return rc;
        }

        programmed += expected_size;
        written += expected_size;
        log_progress_line((unsigned int)written, original_size);
        myFoilLeds_setState(LED_FOIL_TOGGLE_BOTH);
        MCUBOOT_WATCHDOG_FEED();
    }
    printk("\n");

    if (file_offset != (size_t)entry->size)
    {
        BOOT_LOG_ERR("Unexpected trailing data in LZ4 package: %u bytes",
                     (unsigned int)((size_t)entry->size - file_offset));
        return -EINVAL;
    }

    BOOT_LOG_INF("Decompressed %u bytes from LZ4 package, wrote %u bytes, skipped %u bytes",
                 (unsigned int)written,
                 (unsigned int)programmed,
                 (unsigned int)skipped);
    return myFat_validateInstalledHeader(upload_area, original_size);
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
            myFat_noteFileReadError();
            return rc;
        }

        read_len = fs_read(file, buffer, to_read);
        if (read_len < 0)
        {
            myFat_noteFileReadError();
            return read_len;
        }
        if ((size_t)read_len != to_read)
        {
            myFat_noteFileReadError();
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
    uint8_t file_header[EMD_LZ4_HEADER_SIZE];
    bool lz4_package = false;
    uint8_t erased_val;
    size_t processed = 0U;
    size_t written = 0U;
    size_t skipped = 0U;
    unsigned int compared_sectors = 0U;
    unsigned int written_sectors = 0U;
    unsigned int skipped_sectors = 0U;

    fat_file_read_error_detected = false;

    // BOOT_LOG_INF("Checking if new firmware image is waiting in FAT partition");

    memset(&fat_fs, 0, sizeof(fat_fs));

    rc = myFat_syncDiskCache();
    if (rc != 0)
    {
        return -1;
    }

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

        if (entry.size >= EMD_LZ4_HEADER_SIZE)
        {
            rc = myFat_readExact(&fs_file_image, file_header, sizeof(file_header));
            if (rc != 0)
            {
                BOOT_LOG_ERR("Failed to read firmware package header (%d)", rc);
                fs_close(&fs_file_image);
                flash_area_close(upload_area);
                fs_unmount(&mnt);
                return -1;
            }

            lz4_package = (memcmp(file_header, EMD_LZ4_MAGIC, strlen(EMD_LZ4_MAGIC)) == 0);
            rc = fs_seek(&fs_file_image, 0, FS_SEEK_SET);
            if (rc < 0)
            {
                BOOT_LOG_ERR("Failed to rewind firmware file (%d)", rc);
                fs_close(&fs_file_image);
                flash_area_close(upload_area);
                fs_unmount(&mnt);
                return -1;
            }
        }

        if (!lz4_package && (entry.size > upload_area->fa_size))
        {
            BOOT_LOG_ERR("Firmware image too large: %u > slot size %u",
                         (unsigned int)entry.size,
                         (unsigned int)upload_area->fa_size);
            fs_close(&fs_file_image);
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

    if (lz4_package)
    {
        rc = myFat_installLz4Package(&fs_file_image, &entry, upload_area, erased_val, file_header);
        myFoilLeds_setState(LED_FOIL_OFF);
        fs_close(&fs_file_image);

        if ((rc != 0) || fat_file_read_error_detected)
        {
            if (fat_file_read_error_detected)
            {
                (void)myFat_formatAfterFileReadError();
            }
            flash_area_close(upload_area);
            fs_unmount(&mnt);
            return -1;
        }

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
        return 0;
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
            if (fat_file_read_error_detected)
            {
                BOOT_LOG_ERR("FAT file read error detected while comparing firmware image");
                goto file_read_error;
            }
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
                if (fat_file_read_error_detected)
                {
                    BOOT_LOG_ERR("FAT file read error detected while programming firmware image");
                    goto file_read_error;
                }
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

    if (processed < entry.size)
    {
        fs_unmount(&mnt);
        flash_area_close(upload_area);
        return -1;
    }

    if (fat_file_read_error_detected)
    {
        (void)myFat_formatAfterFileReadError();
        flash_area_close(upload_area);
        return -1;
    }

    rc = myFat_validateInstalledHeader(upload_area, entry.size);
    if (rc != 0)
    {
        fs_unmount(&mnt);
        flash_area_close(upload_area);
        return -1;
    }

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

    return 0;

file_read_error:
    printk("\n");
    myFoilLeds_setState(LED_FOIL_OFF);
    fs_close(&fs_file_image);
    (void)myFat_formatAfterFileReadError();
    flash_area_close(upload_area);
    return -1;
}

int myFat_setupUsbMscDisk(void)
{
    int rc;
    struct fs_dirent entry;
    char label[35];

    memset(&fat_fs, 0, sizeof(fat_fs));

    rc = fs_mount(&mnt);
    if (rc < 0)
    {
        BOOT_LOG_WRN("Failed to mount, creating new FAT filesystem");
        rc = myFat_formatAndRemount("mount failed while setting up USB MSC disk");
        if (rc != 0)
        {
            return -1;
        }
    }
    else
    {
        myFat_logFilesystemGeometry();
    }

    snprintf(full_filename, FILENAME_PATH_SIZE, "%s/%s", mnt.mnt_point, MKFS_TRIGGER_FILENAME);
    rc = fs_stat(full_filename, &entry);
    if (rc == 0)
    {
        BOOT_LOG_WRN("Format trigger file \"%s\" found", MKFS_TRIGGER_FILENAME);
        rc = myFat_formatAndRemount("format trigger file found");
        if (rc != 0)
        {
            return -1;
        }
    }
    else if (rc != -ENOENT)
    {
        BOOT_LOG_ERR("Failed to stat format trigger file \"%s\" (%d)", full_filename, rc);
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
