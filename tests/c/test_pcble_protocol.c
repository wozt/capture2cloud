/*
 * The Switch interprets raw 12-bit stick reports using calibration data
 * read from the controller's emulated SPI flash.
 *
 * If those two disagree, a perfectly valid ControllerState=100 can show
 * up as only ~75% in Nintendo's own stick calibration screen.
 */

#include <stdint.h>

#include "../../pcble_backend/poc/protocol.c"
#include "test_util.h"


static void unpack_pair(
    const uint8_t data[3],
    uint16_t *x,
    uint16_t *y)
{
    *x =
        (uint16_t)data[0] |
        ((uint16_t)(data[1] & 0x0f) << 8);

    *y =
        ((uint16_t)data[1] >> 4) |
        ((uint16_t)data[2] << 4);
}


static void read_pair(
    ProState *state,
    uint32_t address,
    uint16_t *x,
    uint16_t *y)
{
    uint8_t data[3] = {
        spi(state, address),
        spi(state, address + 1),
        spi(state, address + 2),
    };

    unpack_pair(data, x, y);
}


static void test_pro_stick_spi(void)
{
    t_begin("Pro Controller SPI stick calibration");

    uint8_t address[6] = {0};

    ProState state;
    controller_init(
        &state,
        CONTROLLER_PRO,
        address);

    uint16_t x;
    uint16_t y;

    /* Left: positive excursion. */
    read_pair(&state, 0x603d, &x, &y);
    t_eq_int("left +X excursion", x, 1466);
    t_eq_int("left +Y excursion", y, 1583);

    /* Left: center. */
    read_pair(&state, 0x6040, &x, &y);
    t_eq_int("left X center", x, 2159);
    t_eq_int("left Y center", y, 1916);

    /* Left: negative excursion. */
    read_pair(&state, 0x6043, &x, &y);
    t_eq_int("left -X excursion", x, 1517);
    t_eq_int("left -Y excursion", y, 1465);

    /* Right: center. */
    read_pair(&state, 0x6046, &x, &y);
    t_eq_int("right X center", x, 2070);
    t_eq_int("right Y center", y, 2013);

    /* Right: negative excursion. */
    read_pair(&state, 0x6049, &x, &y);
    t_eq_int("right -X excursion", x, 1522);
    t_eq_int("right -Y excursion", y, 1531);

    /* Right: positive excursion. */
    read_pair(&state, 0x604c, &x, &y);
    t_eq_int("right +X excursion", x, 1414);
    t_eq_int("right +Y excursion", y, 1510);

    /*
     * controller_init() uses those same centers for reports. This is
     * the part that the old synthetic 2032 calibration violated.
     */
    t_eq_int(
        "runtime left X center matches SPI",
        state.sticks[0],
        2159);

    t_eq_int(
        "runtime left Y center matches SPI",
        state.sticks[1],
        1916);

    t_eq_int(
        "runtime right X center matches SPI",
        state.sticks[2],
        2070);

    t_eq_int(
        "runtime right Y center matches SPI",
        state.sticks[3],
        2013);

    t_eq_int(
        "0x604F factory byte remains present",
        spi(&state, 0x604f),
        0x0f);
}


int main(void)
{
    test_pro_stick_spi();
    return t_report();
}
