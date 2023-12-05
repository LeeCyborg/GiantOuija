/*
Giant Ouija Board project for Arcana 
Kyle Chisholm + lee wilkins, example code for Make Magazine
*/
#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <FastLED.h>

// Color preference persists after restarts
#define PREFERENCES_NAME "ouija"
#define PREFERENCES_COLOR "color"

// LED strip configuration
#define LED_CHIPSET WS2812B
#define LED_PIN 11
#define LED_ORDER EOrder::GRB
#define LED_NUM_LEDS 141
#define LED_BRIGHTNESS 255
#define LED_MILLIAMPS 4000
#define LED_VOLTS 5

// max number of letters to iterate through during Ouija animation
#define OUIJA_MESSAGE_MAX_LENGTH 100
// number of letters in ouija board (a-z, 0-9, Yes and No)
#define OUIJA_NUM_LETTERS 39

// MQTT server with io.adafruit
#define MQTT_SERVER "io.adafruit.com"
#define MQTT_PORT 8883
#define MQTT_USER "adafruitIO_Username"
#define MQTT_PASS "adafruitIO_ApiKey"
#define MQTT_CLIENT "ouija-board";
// Names from feeds setup with Adafruit IO
#define MQTT_TOPIC_PHRASE MQTT_USER "/feeds/ouija.phrase"
#define MQTT_TOPIC_COLOR MQTT_USER "/feeds/ouija.color"

// Message data to pass from MQTT and send to LED animation
struct OuijaPhrase
{
    // Mutex variable to control access from multiple threads
    SemaphoreHandle_t mutex = nullptr;
    // Message received from MQTT
    String message = {};
    // Set to true when new data is received
    bool new_data = false;
};

// Color data to pass from MQTT and send to LED animation
struct OuijaColor
{
    SemaphoreHandle_t mutex = nullptr;
    String color = {};
    bool new_data = false;
};

// The running LED animation data, including the current phrase being displayed
struct OuijaState
{
    // letters appearing in sequence
    int letters[OUIJA_MESSAGE_MAX_LENGTH] = {};
    // number of letters in message
    int letters_length = 0;
    // current letter index being animated
    int letters_index = 0;
    // Color of active letter
    CRGB color = CRGB::White;
    // Color of inactive letters
    CRGB color_inactive = CRGB::Black;
    // Start time of current letter animation
    uint32_t start_time = 0;
};

// global variables

Preferences g_preferences;

WiFiClientSecure g_client;
PubSubClient g_mqtt(g_client);

CRGB g_strip[LED_NUM_LEDS];
TaskHandle_t g_led_run_task = nullptr;

OuijaPhrase g_phrase_input;
OuijaColor g_color_input;
OuijaState g_state;

void mqtt_receive_callback(char* topic_received, uint8_t* payload_received, unsigned int length)
{
    const String topic = String(topic_received);
    const String payload = String(reinterpret_cast<const char*>(payload_received), length);

    if (topic.equals(MQTT_TOPIC_PHRASE))
    {
        // Phrase input
        TickType_t wait_time = 20 / portTICK_PERIOD_MS;
        if (xSemaphoreTake(g_phrase_input.mutex, wait_time) == pdTRUE) {
            g_phrase_input.message = payload;
            g_phrase_input.new_data = true;
            xSemaphoreGive(g_phrase_input.mutex);
        }
    }
    else if (topic.equals(MQTT_TOPIC_COLOR))
    {
        // Phrase input
        TickType_t wait_time = 20 / portTICK_PERIOD_MS;
        if (xSemaphoreTake(g_color_input.mutex, wait_time) == pdTRUE) {
            g_color_input.color = payload;
            g_color_input.new_data = true;
            xSemaphoreGive(g_color_input.mutex);
        }
    }
}

// letters array with start and end index of LED strip
int g_letters[OUIJA_NUM_LETTERS][2] = {
    { 124, 126 }, // 0 is a
    { 120, 122 }, // 1 is b
    { 116, 118 }, // 2 is c
    { 112, 114 }, // 3 is d
    { 108, 110 }, // 4 is e
    { 105, 107 }, // 5 is f
    { 101, 103 }, // 6 is g
    { 97, 99 }, // 7 is h
    { 95, 96 }, // 8 is i
    { 92, 93 }, // 9 is j
    { 88, 90 }, // 10 is k
    { 85, 87 }, // 11 is l
    { 80, 83 }, // 12 is m
    { 76, 78 }, // 13 is n
    { 72, 74 }, // 14 is o
    { 35, 37 }, // 54 is p
    { 38, 40 }, // 16 is q
    { 41, 43 }, // 17 is r
    { 44, 46 }, // 18 is s
    { 48, 50 }, // 19 is t
    { 51, 53 }, // 20 is u
    { 54, 56 }, // 21 is v
    { 57, 61 }, // 22 is w
    { 62, 64 }, // 23 is x
    { 65, 68 }, // 24 is y
    { 69, 71 }, // 25 is z
    { 3, 5 }, // 26 is 0
    { 32, 34 }, // 27 is 1
    { 29, 31 }, // 28 is 2
    { 26, 28 }, // 29 is 3
    { 22, 24 }, // 30 is 4
    { 19, 21 }, // 31 is 5
    { 16, 18 }, // 32 is 6
    { 13, 15 }, // 33 is 7
    { 10, 12 }, // 34 is 8
    { 7, 8 }, // 35 is 9
    { 129, 134 }, // 36 is yes
    { 136, 140 }, // 37 is no
    { 0, 0 } // 38 is space
};

// convert ascii character to index of g_letters
int ascii_to_letter_array_index(const char ascii_character)
{
    if (ascii_character >= 97 && ascii_character <= 122) {
        // numbers 97 to 122 are a to z
        return (ascii_character - 97);
    } else if (ascii_character >= 48 && ascii_character <= 57) {
        // numbers 48 to 57 are 0 to 9
        return (ascii_character - 22);
    } else if (ascii_character == 89) {
        // 89 is Y
        return 36;
    } else if (ascii_character == 78) {
        // 78 is N
        return 37;
    } else if (ascii_character >= 65 && ascii_character <= 90) {
        // numbers 65 to 90 are A to Z
        return (ascii_character - 65);
    } else if ((ascii_character == 32) || (ascii_character == 46)) {
        // space or period
        return 38;
    }
    else
    {
        // invalid
        return -1;
    }
}

// convert characters to letters array corresponding to index of g_letters
void convert_phrase_to_letters(const String &phrase, int letters[], int &letters_length)
{
    size_t index = 0;
    for (size_t k = 0; k < phrase.length(); k++) {
        const int letter = ascii_to_letter_array_index(phrase[k]);
        if (letter >= 0) {
            letters[index] = letter;
            index++;
        }
    }
    letters_length = index;
}

// set color for a letter on the LED strip by changing all LEDs in range given at index of g_letters
void set_ouija_letter(const CRGB& color, int letter_index)
{
    if (letter_index >= 0 && letter_index < OUIJA_NUM_LETTERS) {
        for (size_t i = g_letters[letter_index][0]; i <= g_letters[letter_index][1]; i++) {
            g_strip[i] = color;
        }
    }
}

// Get strip color from string
bool convert_color_string(const String &color_hex, CRGB &color)
{
    bool valid_string = false;
    // Color should be string in format "#000000"
    // Only parse if size is correct
    const int color_hex_size = 7;
    if (color_hex.length() == color_hex_size) {
        // parse string to get integer value
        const char *color_string = color_hex.c_str();
        char *end_ptr = nullptr;
        const int base_hex = 16;
        uint32_t data = static_cast<uint32_t>(strtol(&color_string[1], &end_ptr, base_hex));
        if ((end_ptr - color_string) == color_hex_size)
        {
            bool valid_string = true;
            // strtol success! Get RGB from data
            uint8_t color_r = ((data >> 16) & 0xFF);
            uint8_t color_g = ((data >> 8) & 0xFF);
            uint8_t color_b = ((data >> 0) & 0xFF);
            // assign output color
            color = CRGB(color_r, color_g, color_b);
        }
    }
    return valid_string;
}

// Main LED animation loop
void led_animation_task(void *)
{
    // duration to fade in milliseconds
    const int fade_duration = 600;
    // duration to stay active in milliseconds
    const int active_duration = 1800;

    // start with color saved to preferences (default white)
    String color_preference = g_preferences.getString(PREFERENCES_COLOR, "#ffffff");
    convert_color_string(color_preference, g_state.color);

    // Get number of "ticks" for 20ms intervals (50 times per second)
    uint32_t timer_interval_millis = 20;
    const TickType_t timer_interval_ticks = timer_interval_millis / portTICK_PERIOD_MS;
    // initialize timer
    TickType_t start_tick_time = xTaskGetTickCount();
    TickType_t step_tick_time = start_tick_time;
    for (;;)
    {
        // wait for next step with timer
        xTaskDelayUntil(&step_tick_time, timer_interval_ticks);
        uint32_t time_elapsed = portTICK_PERIOD_MS * (step_tick_time - start_tick_time);

        // Retrieve Inputs
        // ---------------

        // Wait for 5ms to get shared data from MQTT thread
        const TickType_t wait_time = 5 / portTICK_PERIOD_MS;
        // get input phrase data
        if (xSemaphoreTake(g_phrase_input.mutex, wait_time) == pdTRUE) {
            if (g_phrase_input.new_data) {
                // convert to letters
                convert_phrase_to_letters(g_phrase_input.message, g_state.letters, g_state.letters_length);
                g_phrase_input.new_data = false;
                // reset animation with new phrase
                g_state.letters_index = 0;
                g_state.start_time = time_elapsed;
            }
            xSemaphoreGive(g_phrase_input.mutex);
        }
        // get input color data
        if (xSemaphoreTake(g_color_input.mutex, wait_time) == pdTRUE) {
            if (g_color_input.new_data) {
                // set new color
                if (convert_color_string(g_color_input.color, g_state.color))
                {
                    // save color to preferences
                    g_preferences.putString(PREFERENCES_COLOR, g_color_input.color);
                }
                g_color_input.new_data = false;
            }
            xSemaphoreGive(g_color_input.mutex);
        }

        // Animate LEDs
        // ------------

        // clear all LEDs with inactive color
        for (int i = 0; i < LED_NUM_LEDS; i++) {
            g_strip[i] = CRGB::Black;
        }

        // get time in milliseconds
        const uint16_t elapsed_ms = time_elapsed - g_state.start_time;
        if (elapsed_ms <= fade_duration) {
            // fade in letter
            const uint16_t fade_value = (elapsed_ms * 255U) / fade_duration;
            const uint8_t value = static_cast<uint8_t>((fade_value < 255U) ? fade_value : 255U);
            // set color
            CRGB color = g_state.color;
            color.nscale8(value);
            set_ouija_letter(color, g_state.letters[g_state.letters_index]);
        } else if (elapsed_ms <= (active_duration - fade_duration)) {
            // hold letter on
            set_ouija_letter(g_state.color, g_state.letters[g_state.letters_index]);
        } else if (elapsed_ms <= active_duration) {
            // fade out letter
            const uint16_t fade_time = active_duration - elapsed_ms;
            const uint16_t fade_value = (fade_time * 255) / fade_duration;
            const uint8_t value = (fade_value < 255) ? fade_value : 255;
            // set color
            CRGB color = g_state.color;
            color.nscale8(value);
            set_ouija_letter(color, g_state.letters[g_state.letters_index]);
        } else {
            // ensure previous letter is off when done fading out
            set_ouija_letter(CRGB::Black, g_state.letters[g_state.letters_index]);
            // advance to next letter
            g_state.letters_index++;
            if (g_state.letters_index >= g_state.letters_length) {
                // done all letters, restart at index 0 to repeat phrase
                g_state.letters_index = 0;
            }
            // new start time for next letter
            g_state.start_time = time_elapsed;
        }
    }
}

void setup()
{
    // Initialize preferences
    g_preferences.begin(PREFERENCES_NAME, false);

    // Initialize shared data
    g_phrase_input.mutex = xSemaphoreCreateMutex();
    if (g_phrase_input.mutex == nullptr) {
        Serial.print("Failed to create phrase mutex");
    }
    g_color_input.mutex = xSemaphoreCreateMutex();
    if (g_color_input.mutex == nullptr) {
        Serial.print("Failed to create color mutex");
    }

    // start LED animation thread
    // choose core opposite to arduino setup and loop
    const BaseType_t led_run_core = (1 == xPortGetCoreID() ? 0 : 1);
    xTaskCreatePinnedToCore(led_animation_task, /* Task function. */
        "ledRunTask", /* name of task. */
        10000, /* Stack size of task */
        nullptr, /* parameter of the task */
        1, /* priority of the task */
        &g_led_run_task, /* Task handle to keep track of created task */
        led_run_core); /* pin task to core */

    CLEDController& controller = FastLED.addLeds<LED_CHIPSET, LED_PIN, LED_ORDER>(g_strip, LED_NUM_LEDS);
    FastLED.setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(LED_BRIGHTNESS);
    FastLED.setMaxPowerInVoltsAndMilliamps(LED_VOLTS, LED_MILLIAMPS);

    // Set callback to receive data from MQTT and send to LED animation thread
    g_mqtt.setCallback(mqtt_receive_callback);

    // Create client id
    randomSeed(micros());
    String clientId = MQTT_CLIENT;
    clientId += String(random(0xffff), HEX);
    // Configure server
    g_mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    g_client.setCACert("-----BEGIN CERTIFICATE-----\n"
                       "MIIEjTCCA3WgAwIBAgIQDQd4KhM/xvmlcpbhMf/ReTANBgkqhkiG9w0BAQsFADBh\n"
                       // Insert rest of Adafruit CA certificate here
                       "-----END CERTIFICATE-----\n");
    // Connect to MQTT server
    if (!g_mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS))
    {
        Serial.println("Failed to connect MQTT to mqtt server. Restarting in 5 seconds");
    }
    // subscribe to topics
    g_mqtt.subscribe(MQTT_TOPIC_PHRASE);
    g_mqtt.subscribe(MQTT_TOPIC_COLOR);
}

void loop()
{
    // Main mqtt loop
    // --------------

    g_mqtt.loop();
}
