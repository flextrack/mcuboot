#ifndef _MY_APS823_H_
#define _MY_APS823_H_

#ifdef __cplusplus
extern "C"
{
#endif

    int myAps823_disableWatchdogPulse(void);
    int myAps823_toggleWatchdog(void);
    int myAps823_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _MY_APS823_H_ */
