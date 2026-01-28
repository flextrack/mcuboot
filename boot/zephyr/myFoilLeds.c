#include <zephyr/drivers/gpio.h>

#include "myFoilLeds.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(myFoilLeds, LOG_LEVEL_INF);

#define LED_FOIL_GREEN_NODE DT_NODELABEL(led_foil_green)
static const struct gpio_dt_spec led_foil_green_out = GPIO_DT_SPEC_GET(LED_FOIL_GREEN_NODE, gpios);

#define LED_FOIL_BLUE_NODE DT_NODELABEL(led_foil_blue)
static const struct gpio_dt_spec led_foil_blue_out = GPIO_DT_SPEC_GET(LED_FOIL_BLUE_NODE, gpios);

static bool foil_leds_initialized = false;

static int foil_leds_init(void)
{
    int ret = gpio_pin_configure_dt(&led_foil_green_out, GPIO_OUTPUT);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
        return -1;
    }

    ret = gpio_pin_configure_dt(&led_foil_blue_out, GPIO_OUTPUT);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
        return -1;
    }

    foil_leds_initialized = true;
    return 0;
}

int myFoilLeds_setState(ledFoilState_t state)
{
    int ret;

    if (!foil_leds_initialized)
    {
        ret = foil_leds_init();
        if (ret == 0)
        {
            foil_leds_initialized = true;
        }
        else
        {
            LOG_ERR("Failed to initialize foil LEDs");
            return -1;
        }
    }

    switch (state)
    {
    case LED_FOIL_OFF: // off
        ret = gpio_pin_set_dt(&led_foil_green_out, 0);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
            return -1;
        }

        ret = gpio_pin_set_dt(&led_foil_blue_out, 0);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
            return -1;
        }
        break;

    case LED_FOIL_SET_GREEN: // green
        ret = gpio_pin_set_dt(&led_foil_green_out, 1);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
            return -1;
        }
        ret = gpio_pin_set_dt(&led_foil_blue_out, 0);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
            return -1;
        }
        break;

    case LED_FOIL_SET_BLUE: // blue
        ret = gpio_pin_set_dt(&led_foil_green_out, 0);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
            return -1;
        }
        ret = gpio_pin_set_dt(&led_foil_blue_out, 1);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
            return -1;
        }
        break;

    case LED_FOIL_SET_BOTH: // both
        ret = gpio_pin_set_dt(&led_foil_green_out, 1);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
            return -1;
        }
        ret = gpio_pin_set_dt(&led_foil_blue_out, 1);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to set %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
            return -1;
        }
        break;

    case LED_FOIL_TOGGLE_GREEN: // toggle green
        ret = gpio_pin_toggle_dt(&led_foil_green_out);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to toggle %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
            return -1;
        }
        break;

    case LED_FOIL_TOGGLE_BLUE: // toggle blue
        ret = gpio_pin_toggle_dt(&led_foil_blue_out);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to toggle %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
            return -1;
        }
        break;

    case LED_FOIL_TOGGLE_BOTH: // toggle both
        ret = gpio_pin_toggle_dt(&led_foil_green_out);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to toggle %s pin %d", ret, led_foil_green_out.port->name, led_foil_green_out.pin);
            return -1;
        }
        ret = gpio_pin_toggle_dt(&led_foil_blue_out);
        if (ret != 0)
        {
            LOG_ERR("Error %d: failed to toggle %s pin %d", ret, led_foil_blue_out.port->name, led_foil_blue_out.pin);
            return -1;
        }
        break;

    default:
        LOG_ERR("Invalid foil LED state: %d", state);
        return -1;
    }

    return 0;
}