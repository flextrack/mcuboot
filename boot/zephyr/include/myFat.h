#ifndef _MY_FAT_H_
#define _MY_FAT_H_

#ifdef __cplusplus
extern "C"
{
#endif

    int myFat_installFirmwareFromFatFile(uint8_t upload_slot);
    int myFat_setupUsbMscDisk(void);

#ifdef __cplusplus
}
#endif

#endif /* _MY_FAT_H_ */
