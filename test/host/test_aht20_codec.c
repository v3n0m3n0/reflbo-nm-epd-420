#include "aht20_codec.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_busy_bit_rejected(void)
{
    uint8_t raw[6] = { 0x80, 0x80, 0x00, 0x06, 0x00, 0x00 };
    int t = 0, h = 0;
    TEST_ASSERT_FALSE(aht20_parse(raw, &t, &h));
}

static void test_null_guards(void)
{
    uint8_t raw[6] = { 0x1C, 0x80, 0x00, 0x06, 0x00, 0x00 };
    int t = 0, h = 0;
    TEST_ASSERT_FALSE(aht20_parse(NULL, &t, &h));
    TEST_ASSERT_FALSE(aht20_parse(raw, NULL, &h));
    TEST_ASSERT_FALSE(aht20_parse(raw, &t, NULL));
}

static void test_valid_midpoint(void)
{
    /* hum_raw = 0x80000 (50.00%), temp_raw = 0x60000 (3/8 * 200 - 50 = 25.00 C) */
    uint8_t raw[6] = { 0x1C, 0x80, 0x00, 0x06, 0x00, 0x00 };
    int t = 0, h = 0;
    TEST_ASSERT_TRUE(aht20_parse(raw, &t, &h));
    TEST_ASSERT_EQUAL_INT(2500, t);
    TEST_ASSERT_EQUAL_INT(5000, h);
}

static void test_extremes(void)
{
    int t = 0, h = 0;
    /* All zeros -> 0% RH, -50.00 C */
    uint8_t min_raw[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    TEST_ASSERT_TRUE(aht20_parse(min_raw, &t, &h));
    TEST_ASSERT_EQUAL_INT(-5000, t);
    TEST_ASSERT_EQUAL_INT(0, h);

    /* All 0xFF (status 0x1C) -> ~100% RH, ~+150 C */
    uint8_t max_raw[6] = { 0x1C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_TRUE(aht20_parse(max_raw, &t, &h));
    TEST_ASSERT_INT_WITHIN(2, 15000, t);
    TEST_ASSERT_INT_WITHIN(2, 10000, h);
}

static void test_offset_humidity(void)
{
    TEST_ASSERT_EQUAL_INT(5500, aht20_offset_humidity(5000, 500));
    TEST_ASSERT_EQUAL_INT(0, aht20_offset_humidity(200, -500));
    TEST_ASSERT_EQUAL_INT(10000, aht20_offset_humidity(9800, 500));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_busy_bit_rejected);
    RUN_TEST(test_null_guards);
    RUN_TEST(test_valid_midpoint);
    RUN_TEST(test_extremes);
    RUN_TEST(test_offset_humidity);
    return UNITY_END();
}

