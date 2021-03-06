#include <stdint.h>
#include <stdbool.h>

#include "stm32f0xx.h"
#include "platform.h"
#include "rc_receiver.h"
#include "persistent_storage.h"
#include "rf.h"



#define PAYLOAD_SIZE 10
#define ADDRESS_WIDTH 5
#define NUMBER_OF_HOP_CHANNELS 20
#define MAX_HOP_WITHOUT_PACKET 15
#define FIRST_HOP_TIME_IN_US 2500
#define HOP_TIME_IN_US 5000

#define FAILSAFE_TIMEOUT (640 / __SYSTICK_IN_MS)
#define BIND_TIMEOUT (5000 / __SYSTICK_IN_MS)
#define ISP_TIMEOUT (3000 / __SYSTICK_IN_MS)
#define BLINK_TIME_FAILSAFE (320 / __SYSTICK_IN_MS)
#define BLINK_TIME_BINDING (50 / __SYSTICK_IN_MS)

#define LED_STATE_IDLE 0
#define LED_STATE_RECEIVING 1
#define LED_STATE_FAILSAFE 2
#define LED_STATE_BINDING 3

#define BUTTON_PRESSED 0
#define BUTTON_RELEASED 1


extern bool systick;


uint16_t channels[NUMBER_OF_CHANNELS];
//uint16_t raw_data[2];
bool successful_stick_data = false;


static bool rf_int_fired = false;
static unsigned int led_state;
static unsigned int blink_timer;
static unsigned int bind_button_timer;

static uint8_t payload[PAYLOAD_SIZE];

static uint8_t failsafe_enabled;
static uint16_t failsafe[NUMBER_OF_CHANNELS];
static unsigned int failsafe_timer;

static uint8_t model_address[ADDRESS_WIDTH];
static bool perform_hop_requested = false;
static unsigned int hops_without_packet;
static unsigned int hop_index;
static uint8_t hop_data[NUMBER_OF_HOP_CHANNELS];

static bool binding_requested = false;
static bool binding = false;
static unsigned int bind_timer;
static const uint8_t BIND_CHANNEL = 0x51;
static const uint8_t BIND_ADDRESS[ADDRESS_WIDTH] = {0x12, 0x23, 0x23, 0x45, 0x78};
static uint8_t bind_storage_area[ADDRESS_WIDTH + NUMBER_OF_HOP_CHANNELS] __attribute__ ((aligned (4)));




// ****************************************************************************
static void initialize_failsafe(void) {
    int i;

    failsafe_enabled = false;
    failsafe_timer = FAILSAFE_TIMEOUT;
    for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
        failsafe[i] = SERVO_PULSE_CENTER;
    }
}


// ****************************************************************************
static void output_pulses(void)
{
    TIM14->CCR1 = 2 * channels[0];
    TIM1->CCR3  = 2 * channels[1];
    TIM1->CCR2  = 2 * channels[2];
}


// ****************************************************************************
static uint16_t stickdata2ms(uint16_t stickdata)
{
    uint32_t ms;

    // ms = (0xffff - stickdata) * 3 / 4;
    ms = (0xffff - stickdata);
    ms = ((2100 - 900)*(ms)/(2750 - 1210) + 900) - (2100 - 900)*(1210)/(2750 - 1210);
    
    return ms & 0xffff;
}


// ****************************************************************************
// This code undos the value scaling that the transmitter nRF module does
// when receiving a 12 bit channel value via the UART, while forming the
// transmit packet.
//
// The transmitter calculates
//
//    value = (uart_data * 14 / 10) + 0xf200
//
// As such the input range via the UART can be 0x000 .. 0x9ff
// ****************************************************************************
/*
static uint16_t stickdata2txdata(uint16_t stickdata)
{
    uint32_t txdata;

    txdata = (stickdata - 0xf200) * 10 / 14;
    return txdata & 0xffff;
}
*/

// ****************************************************************************
static void stop_hop_timer(void)
{
    // Stop the Timer
    TIM3->CR1 &= ~(TIM_CR1_CEN);
    perform_hop_requested = false;
}


// ****************************************************************************
static void restart_hop_timer(void)
{
    
    TIM3->ARR = (HOP_TIME_IN_US - 1);
    TIM3->CNT = (FIRST_HOP_TIME_IN_US - 1);
    TIM3->CR1 |=  TIM_CR1_CEN;
    
    hops_without_packet = 0;
    perform_hop_requested = false;
}


// ****************************************************************************
static void restart_packet_receiving(void)
{
    stop_hop_timer();

    rf_clear_ce();
    hop_index = 0;
    hops_without_packet = 0;
    perform_hop_requested = false;
    rf_set_rx_address(DATA_PIPE_0, ADDRESS_WIDTH, model_address);
    rf_set_channel(hop_data[0]);
    rf_flush_rx_fifo();
    rf_clear_irq(RX_RD);
    rf_int_fired = false;
    rf_set_ce();
}


// ****************************************************************************
static void parse_bind_data(void)
{
    int i;

    for (i = 0; i < ADDRESS_WIDTH; i++) {
        model_address[i] = bind_storage_area[i];
    }

    for (i = 0; i < NUMBER_OF_HOP_CHANNELS; i++) {
        hop_data[i] = bind_storage_area[ADDRESS_WIDTH + i];
    }
}


// ****************************************************************************
static void binding_done(void)
{
    led_state = LED_STATE_IDLE;
    failsafe_timer = FAILSAFE_TIMEOUT;
    binding = false;
    binding_requested = false;

    restart_packet_receiving();
}


// ****************************************************************************
// The bind process works as follows:
//
// The transmitter regularly sends data on the fixed channel 51h, with address
// 12:23:23:45:78.
// This data is sent at a lower power. All transmitters do that all the time.
//
// A bind data packet is sent every 5ms.
//
// The transmitter cycles through 4 packets:
// ff aa 55 a1 a2 a3 a4 a5 .. ..
// cc cc 00 ha hb hc hd he hf hg
// cc cc 01 hh hi hj hk hl hm hn
// cc cc 02 ho hp hq hr hs ht ..
//
// ff aa 55     Special marker for the first packet
// a[1-5]       The 5 address bytes
// cc cc        A 16 byte checksum of bytes a1..a5
// h[a-t]       20 channels for frequency hopping
// ..           Not used
//
// ****************************************************************************
static void process_binding(void)
{
    static unsigned int bind_state;
    static uint16_t checksum;
    int i;

    // ================================
    if (!binding) {
        if (!binding_requested) {
            return;
        }

        binding_requested = false;
        led_state = LED_STATE_BINDING;
        binding = true;
        bind_state = 0;
        bind_timer = BIND_TIMEOUT;

        rf_clear_ce();
        // Set special address 12h 23h 23h 45h 78h
        rf_set_rx_address(0, ADDRESS_WIDTH, BIND_ADDRESS);
        // Set special channel 0x51
        rf_set_channel(BIND_CHANNEL);
        rf_set_ce();
        return;
    }


    // ================================
    if (bind_timer == 0) {
        binding_done();
        return;
    }


    // ================================
    if (!rf_int_fired) {
        return;
    }
    rf_int_fired = false;

    while (!rf_is_rx_fifo_emtpy()) {
        rf_read_fifo(payload, PAYLOAD_SIZE);
    }
    rf_clear_irq(RX_RD);

    switch (bind_state) {
        case 0:
            if (payload[0] == 0xff) {
                if (payload[1] == 0xaa) {
                    if (payload[2] == 0x55) {
                        checksum = 0;
                        for (i = 0; i < 5; i++) {
                            uint8_t payload_byte;

                            payload_byte = payload[3 + i];
                            bind_storage_area[i] = payload_byte;
                            checksum += payload_byte;
                        }
                        bind_state = 1;
                    }
                }
            }
            break;

        case 1:
            if (payload[0] == (checksum & 0xff)) {
                if (payload[1] == (checksum >> 8)) {
                    if (payload[2] == 0) {
                        for (i = 0; i < 7; i++) {
                            bind_storage_area[5 + i] = payload[3 + i];
                        }
                        bind_state = 2;
                    }
                }
            }
            break;

        case 2:
            if (payload[0] == (checksum & 0xff)) {
                if (payload[1] == (checksum >> 8)) {
                    if (payload[2] == 1) {
                        for (i = 0; i < 7; i++) {
                            bind_storage_area[12 + i] = payload[3 + i];
                        }
                        bind_state = 3;
                    }
                }
            }
            break;

        case 3:
            if (payload[0] == (checksum & 0xff)) {
                if (payload[1] == (checksum >> 8)) {
                    if (payload[2] == 2) {
                        for (i = 0; i < 6; i++) {
                            bind_storage_area[19 + i] = payload[3 + i];
                        }

                        save_persistent_storage(bind_storage_area);
                        parse_bind_data();
                        binding_done();
                        return;
                    }
                }
            }
            break;

        default:
            bind_state = 0;
            break;
    }
}


// ****************************************************************************
static void process_receiving(void)
{
    // ================================
    if (binding) {
        return;
    }

    // ================================
    // Process failsafe only if we ever got a successsful stick data payload
    // after reset.
    //
    // This way the servo outputs stay off until we got successful stick
    // data, so the servos do not got to the failsafe point after power up
    // in case the transmitter is not on yet.
    if (successful_stick_data) {
        if (failsafe_timer == 0) {
            int i;

            for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
                channels[i] = failsafe[i];
            }
            output_pulses();

            led_state = LED_STATE_FAILSAFE;
        }
    }


    // ================================
    if (perform_hop_requested) {
        perform_hop_requested = false;
        ++hops_without_packet;


        if (hops_without_packet > MAX_HOP_WITHOUT_PACKET) {
            restart_packet_receiving();
        }
        else {
            rf_clear_ce();
            hop_index = (hop_index + 1) % NUMBER_OF_HOP_CHANNELS;
            rf_set_channel(hop_data[hop_index]);
            rf_set_ce();
        }
    }


    // ================================
    if (!rf_int_fired) {
        return;
    }
    rf_int_fired = false;

    while (!rf_is_rx_fifo_emtpy()) {
        rf_read_fifo(payload, PAYLOAD_SIZE);
    }
    rf_clear_irq(RX_RD);

    restart_hop_timer();


    // ================================
    // payload[7] is 0x55 for stick data
    if (payload[7] == 0x55) {   // Stick data
        channels[0] = stickdata2ms((payload[1] << 8) + payload[0]);
        channels[1] = stickdata2ms((payload[3] << 8) + payload[2]);
        channels[2] = stickdata2ms((payload[5] << 8) + payload[4]);
        output_pulses();

        // Save raw received data for the pre-processor to output, so someone
        // can build custom extension based on hijacking channel 3 and using
        // the unused payload bytes 6 and 9.
        // Note:
        //   - See hk310-expansion project for hijacking channel 3
        //   - Custom nRF module firmware required in the transmitter to utilize
        //     payload 6 + 9
//        raw_data[0] = stickdata2txdata((payload[5] << 8) + payload[4]);
//        raw_data[1] = (payload[6] << 8) + payload[9];


        if (!successful_stick_data) {
            TIM3->CR1 |=  TIM_CR1_CEN;      // Start the Timer
        }
        successful_stick_data = true;

        failsafe_timer = FAILSAFE_TIMEOUT;
        led_state = LED_STATE_RECEIVING;
    }
    // ================================
    // payload[7] is 0xaa for failsafe data
    else if (payload[7] == 0xaa) {
        // payload[8]: 0x5a if enabled, 0x5b if disabled
        if (payload[8] == 0x5a) {
            failsafe_enabled = true;
            failsafe[0] = stickdata2ms((payload[1] << 8) + payload[0]);
            failsafe[1] = stickdata2ms((payload[3] << 8) + payload[2]);
            failsafe[2] = stickdata2ms((payload[5] << 8) + payload[4]);
        }
        else {
            // If failsafe is disabled use default values of 1500ms, just
            // like the HKR3000 and XR3100 do.
            initialize_failsafe();
        }
    }
}


// ****************************************************************************
static void process_systick(void)
{
    if (!systick) {
        return;
    }

    if (failsafe_timer) {
        --failsafe_timer;
    }

    if (bind_timer) {
        --bind_timer;
    }

    if (bind_button_timer) {
        --bind_button_timer;
    }

    if (blink_timer) {
        --blink_timer;
    }
}


// ****************************************************************************
static void process_bind_button(void)
{
    static bool isp_timeout_active;
    static bool old_button_state = BUTTON_RELEASED;
    bool new_button_state;


    if (!systick) {
        return;
    }

    new_button_state = GPIO_BIND;

    if (isp_timeout_active && (bind_button_timer == 0)) {
        GPIO_LED_OFF();
        // invoke_ISP();
        // We should never return here...
    }

    if (new_button_state == old_button_state) {
        return;
    }
    old_button_state = new_button_state;

    if (new_button_state == BUTTON_PRESSED) {
        bind_button_timer = ISP_TIMEOUT;
        isp_timeout_active = true;
    }

    if (new_button_state == BUTTON_RELEASED) {
        isp_timeout_active = false;
        binding_requested = true;
    }
}


// ****************************************************************************
static void process_led(void)
{
    static unsigned int old_led_state = 0xffffffff;
    static bool blinking;
    static unsigned int blink_timer_reload_value;


    if (blinking) {
        if (blink_timer == 0) {
            blink_timer = blink_timer_reload_value;
            GPIO_LED_TOGGLE();
        }
    }

    if (led_state == old_led_state) {
        return;
    }
    old_led_state = led_state;

    GPIO_LED_OFF();

    switch (led_state) {
        case LED_STATE_RECEIVING:
            GPIO_LED_ON();
            blinking = false;
            break;

        case LED_STATE_BINDING:
            blink_timer_reload_value = BLINK_TIME_BINDING;
            blinking = true;
            break;

        case LED_STATE_IDLE:
        case LED_STATE_FAILSAFE:
        default:
            blink_timer_reload_value = BLINK_TIME_FAILSAFE;
            blinking = true;
            break;
    }
}


// ****************************************************************************
void init_receiver(void)
{
    load_persistent_storage(bind_storage_area);
    parse_bind_data();
    initialize_failsafe();

    rf_enable_clock();
    rf_clear_ce();
    rf_enable_receiver();

    rf_set_crc(CRC_2_BYTES);
    rf_set_irq_source(RX_RD);
    rf_set_data_rate(DATA_RATE_250K);
    rf_set_data_pipes(DATA_PIPE_0, NO_AUTO_ACKNOWLEDGE);
    rf_set_address_width(ADDRESS_WIDTH);
    rf_set_payload_size(DATA_PIPE_0, PAYLOAD_SIZE);

    restart_packet_receiving();

    led_state = LED_STATE_IDLE;
}


// ****************************************************************************
void process_receiver(void)
{
    process_systick();
    process_bind_button();
    process_binding();
    process_receiving();
    process_led();
}


// ****************************************************************************
void rf_interrupt_handler(void)
{
    rf_int_fired = true;
}


// ****************************************************************************
void hop_timer_handler(void)
{
    perform_hop_requested = true;
}
