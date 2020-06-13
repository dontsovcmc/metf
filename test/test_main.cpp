#include <Arduino.h>
#include <unity.h>

#include "utils.h"
#include "base64.h"

// void setUp(void) {
// // set stuff up here
// }

// void tearDown(void) {
// // clean stuff up here
// }

void test_arduino_constants(void) {
    TEST_ASSERT_EQUAL(1, HIGH);
    TEST_ASSERT_EQUAL(0, LOW);

    TEST_ASSERT_EQUAL(0, INPUT);
    TEST_ASSERT_EQUAL(2, INPUT_PULLUP);
    TEST_ASSERT_EQUAL(1, OUTPUT);
}

void test_nodemcu_constants(void) {

    TEST_ASSERT_EQUAL(2, LED_BUILTIN);
    TEST_ASSERT_EQUAL(16, D0);
    TEST_ASSERT_EQUAL(14, D5);
    //TODO add all
}

void test_HexCharToInt(void) {
    TEST_ASSERT_EQUAL(0x0, hexCharToInt('0'));
    TEST_ASSERT_EQUAL(0x1, hexCharToInt('1'));
    TEST_ASSERT_EQUAL(0x9, hexCharToInt('9'));

    TEST_ASSERT_EQUAL(0xA, hexCharToInt('a'));
    TEST_ASSERT_EQUAL(0xD, hexCharToInt('d'));
    TEST_ASSERT_EQUAL(0xF, hexCharToInt('f'));

    TEST_ASSERT_EQUAL(0xA, hexCharToInt('A'));
    TEST_ASSERT_EQUAL(0xD, hexCharToInt('D'));
    TEST_ASSERT_EQUAL(0xF, hexCharToInt('F'));

    //abnormal
    TEST_ASSERT_EQUAL(0, hexCharToInt('M'));
    TEST_ASSERT_EQUAL(0, hexCharToInt('P'));
    TEST_ASSERT_EQUAL(0, hexCharToInt('Y'));
}

void test_IntToHexChar(void) {
    TEST_ASSERT_EQUAL('0', intToHexChar(0));
    TEST_ASSERT_EQUAL('5', intToHexChar(5));
    TEST_ASSERT_EQUAL('9', intToHexChar(9));

    TEST_ASSERT_EQUAL('A', intToHexChar(10));
    TEST_ASSERT_EQUAL('B', intToHexChar(11));
    TEST_ASSERT_EQUAL('F', intToHexChar(15));

    //abnormal
    TEST_ASSERT_EQUAL('0', intToHexChar(20));
    TEST_ASSERT_EQUAL('0', intToHexChar(200));
}

void test_OnlyHexText(void) {
    TEST_ASSERT_TRUE(onlyHexText("F0"));
    TEST_ASSERT_TRUE(onlyHexText("001122"));
    TEST_ASSERT_TRUE(onlyHexText("FF"));
    TEST_ASSERT_TRUE(onlyHexText("ff"));

    TEST_ASSERT_FALSE(onlyHexText("F"));
    TEST_ASSERT_FALSE(onlyHexText("00112"));
    TEST_ASSERT_FALSE(onlyHexText("NN"));
}

void test_HexText2AsciiArray(void) {

    uint8_t arr_out[64];

    {
        String s("");
        TEST_ASSERT_EQUAL(0, hexText2AsciiArray(s, arr_out, 64));
    }

    {
        String str("1");  //incorrect
        TEST_ASSERT_FALSE(onlyHexText(str));
        TEST_ASSERT_EQUAL(0, hexText2AsciiArray(str, arr_out, 64));
    }

    {
        String str("MM");  //incorrect
        TEST_ASSERT_FALSE(onlyHexText(str));
        TEST_ASSERT_EQUAL(0, hexText2AsciiArray(str, arr_out, 64));
    }

    {
        String str("00F"); //incorrect
        TEST_ASSERT_FALSE(onlyHexText(str));
        TEST_ASSERT_EQUAL(0, hexText2AsciiArray(str, arr_out, 2));
    }

    {
        uint8_t arr[] = { 0x00 };
        String str("00");
        TEST_ASSERT_TRUE(onlyHexText(str));
        TEST_ASSERT_EQUAL(1, hexText2AsciiArray(str, arr_out, 1));
        TEST_ASSERT_EQUAL_CHAR_ARRAY(arr, arr_out, 1);
    }

    {
        uint8_t arr[] = { 0x30, 0x31 };
        String str("3031");
        TEST_ASSERT_TRUE(onlyHexText(str));
        TEST_ASSERT_EQUAL(2, hexText2AsciiArray(str, arr_out, 2));
        TEST_ASSERT_EQUAL_CHAR_ARRAY(arr, arr_out, 2);
    }

    {
        uint8_t arr[] = { 0x00, 0xFF, 0xF1, 0xD0 };
        String str("00FFF1D0");
        TEST_ASSERT_TRUE(onlyHexText(str));
        TEST_ASSERT_EQUAL(4, hexText2AsciiArray(str, arr_out, 4));
        TEST_ASSERT_EQUAL_CHAR_ARRAY(arr, arr_out, 4);
    }

    {
        uint8_t arr[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  };
        String str("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
        TEST_ASSERT_TRUE(onlyHexText(str));
        TEST_ASSERT_EQUAL(20, hexText2AsciiArray(str, arr_out, 20));
        TEST_ASSERT_EQUAL_CHAR_ARRAY(arr, arr_out, 20);
    }
}

void setup() {
    delay(2000);

    UNITY_BEGIN();
    
    RUN_TEST(test_arduino_constants);
    RUN_TEST(test_nodemcu_constants);
    RUN_TEST(test_HexCharToInt);
    RUN_TEST(test_IntToHexChar);
    RUN_TEST(test_OnlyHexText);
    RUN_TEST(test_HexText2AsciiArray);

    UNITY_END();
}

void loop() {
}