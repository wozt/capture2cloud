/* Pure Joy-Con 2 input-map regression test: no Bluetooth hardware required.
 * The Wii U GamePad has TWO independent sticks, so the left Joy-Con 2 must
 * never borrow RX/RY, nor may the right Joy-Con 2 borrow LX/LY. */
#include <stdint.h>
#include <string.h>
#include "../../pcble_backend/src/jc2.c"
#include "test_util.h"

/* The compiled jc2.c also implements key pairing, which is not used in
 * these pure input-mapping tests. Supply link-only placeholders so tests
 * have no cryptographic, radio, or privileged runtime dependencies. */
void pairing_compute_ltk(const uint8_t a1[16], const uint8_t b1[16], uint8_t ltk[16])
{
    (void)a1; (void)b1; memset(ltk, 0, 16);
}
gboolean pairing_compute_b2(const uint8_t *ltk, const uint8_t *a2,
                            uint8_t *b2, GError **error)
{
    (void)ltk; (void)a2; (void)b2; (void)error;
    return FALSE;
}

static void expect_neutral(const char *label, Jc2Side side, Jc2Input input)
{
    Jc2Input neutral = {0};
    jc2_neutral(side, &neutral);
    char name[128];
    snprintf(name, sizeof name, "%s X neutral", label);
    t_eq_int(name, input.stick_x, neutral.stick_x);
    snprintf(name, sizeof name, "%s Y neutral", label);
    t_eq_int(name, input.stick_y, neutral.stick_y);
}

int main(void)
{
    t_begin("Joy-Con 2 upright: independent Wii U GamePad sticks");
    const uint8_t buttons[3] = {0};
    const uint16_t centers[4] = {2159, 1916, 2070, 2013};
    uint16_t sticks[4];
    Jc2Input left = {0}, right = {0}, left_center = {0}, right_center = {0};
    memcpy(sticks, centers, sizeof sticks);
    jc2_from_pro_frame_vertical(JC2_LEFT, buttons, sticks, &left_center);
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &right_center);
    expect_neutral("both neutral left", JC2_LEFT, left_center);
    expect_neutral("both neutral right", JC2_RIGHT, right_center);

    /* Character movement ONLY: the camera (right) must stay centered. */
    sticks[0] = 2759; sticks[1] = 1516;
    jc2_from_pro_frame_vertical(JC2_LEFT, buttons, sticks, &left);
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &right);
    t_ok("left X moves", left.stick_x != left_center.stick_x);
    t_ok("left Y moves", left.stick_y != left_center.stick_y);
    expect_neutral("left-only input right", JC2_RIGHT, right);

    /* Camera ONLY: character movement (left) must stay centered. */
    memcpy(sticks, centers, sizeof sticks);
    sticks[2] = 2570; sticks[3] = 2413;
    jc2_from_pro_frame_vertical(JC2_LEFT, buttons, sticks, &left);
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &right);
    expect_neutral("right-only input left", JC2_LEFT, left);
    t_ok("right X moves", right.stick_x != right_center.stick_x);
    t_ok("right Y moves", right.stick_y != right_center.stick_y);

    /* Nonzero input on both sticks: output must depend only on its side. */
    sticks[0] = 2559; sticks[1] = 2216;
    jc2_from_pro_frame_vertical(JC2_LEFT, buttons, sticks, &left);
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &right);
    Jc2Input left_before = left, right_before = right;
    sticks[2] = 1770; sticks[3] = 1613;
    jc2_from_pro_frame_vertical(JC2_LEFT, buttons, sticks, &left);
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &right);
    t_eq_int("right changes do not alter left X", left.stick_x, left_before.stick_x);
    t_eq_int("right changes do not alter left Y", left.stick_y, left_before.stick_y);
    t_ok("right changes right X", right.stick_x != right_before.stick_x);
    t_ok("right changes right Y", right.stick_y != right_before.stick_y);

    /* Small deflections (within the old 0.01 radius check) still must
     * never borrow the much larger movement of the opposite stick. */
    memcpy(sticks, centers, sizeof sticks);
    sticks[0] = 2659; sticks[2] = 2100;
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &right);
    t_ok("small right input remains independent of large left",
         right.stick_x != right_center.stick_x);
    t_ok("small right input does not borrow left",
         right.stick_x < right_center.stick_x + 120);

    /* Sanity: the legacy horizontal one-Joy-Con transform remains live. */
    memcpy(sticks, centers, sizeof sticks);
    sticks[2] = 2570;
    Jc2Input horizontal = {0};
    jc2_from_pro_frame(JC2_LEFT, buttons, sticks, &horizontal);
    Jc2Input horizontal_neutral = {0};
    jc2_neutral(JC2_LEFT, &horizontal_neutral);
    t_ok("horizontal single-Joy-Con fallback retained",
         horizontal.stick_x != horizontal_neutral.stick_x ||
         horizontal.stick_y != horizontal_neutral.stick_y);
    return t_report();
}
