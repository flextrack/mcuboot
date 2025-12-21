#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(myAps823, LOG_LEVEL_INF);

#define APX823_NODE DT_NODELABEL(apx823)
PINCTRL_DT_DEFINE(APX823_NODE);
#define PINCTRL_STATE_DEFAULT 0U

static struct gpio_dt_spec watchdog_dis_out = GPIO_DT_SPEC_GET(APX823_NODE, watchdog_dis_gpios); /* gpio1 24 */
static struct gpio_dt_spec watchdog_int_out = GPIO_DT_SPEC_GET(APX823_NODE, watchdog_int_gpios); /* gpio1 25 */

static bool aps823_initialized = false;

static int apx823_apply_pinctrl(void)
{
    const struct pinctrl_dev_config *cfg = PINCTRL_DT_DEV_CONFIG_GET(APX823_NODE);

    return pinctrl_apply_state(cfg, PINCTRL_STATE_DEFAULT);
}

int myAps823_toggleWatchdog(void)
{
    int ret;

    if (!aps823_initialized)
    {
        LOG_ERR("APX823 not initialized!");
        return -1;
    }

    ret = gpio_pin_toggle_dt(&watchdog_int_out);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to set %s pin %d", ret, watchdog_int_out.port->name, watchdog_int_out.pin);
        return ret;
    }

    // LOG_INF("Toggle ex-watchdog!");

    return 0;
}

int myAps823_disableWatchdogPulse(void)
{
    int ret;

    if (!aps823_initialized)
    {
        LOG_ERR("APX823 not initialized!");
        return -1;
    }

    ret = gpio_pin_set_dt(&watchdog_dis_out, 1);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to set %s pin %d", ret, watchdog_dis_out.port->name, watchdog_dis_out.pin);
        return -1;
    }

    k_busy_wait(1000);

    ret = gpio_pin_set_dt(&watchdog_dis_out, 0);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to set %s pin %d", ret, watchdog_dis_out.port->name, watchdog_dis_out.pin);
        return -1;
    }

    // LOG_INF("Disable ex-watchdog for few seconds!");

    return 0;
}

int myAps823_init(void)
{
    int ret;

    ret = apx823_apply_pinctrl();
    if (ret != 0)
    {
        LOG_ERR("Failed to apply pinctrl for APX823");
        return -1;
    }

    ret = gpio_pin_configure_dt(&watchdog_dis_out, GPIO_OUTPUT);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d", ret, watchdog_dis_out.port->name, watchdog_dis_out.pin);
        return -1;
    }

    ret = gpio_pin_configure_dt(&watchdog_int_out, GPIO_OUTPUT);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d", ret, watchdog_int_out.port->name, watchdog_int_out.pin);
        return -1;
    }

    aps823_initialized = true;

    LOG_INF("Ex-watchdog initialized");

    return 0;
}