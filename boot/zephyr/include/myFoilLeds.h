#ifndef _MY_FOIL_LEDS_H_
#define _MY_FOIL_LEDS_H_

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        LED_FOIL_OFF = 0,
        LED_FOIL_SET_GREEN,
        LED_FOIL_SET_BLUE,
        LED_FOIL_SET_BOTH,
        LED_FOIL_TOGGLE_GREEN,
        LED_FOIL_TOGGLE_BLUE,
        LED_FOIL_TOGGLE_BOTH,
    } ledFoilState_t;

    int myFoilLeds_setState(ledFoilState_t state);

#ifdef __cplusplus
}
#endif

#endif /* _MY_FOIL_LEDS_H_ */
