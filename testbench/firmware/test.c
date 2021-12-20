/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org>
*/

#include <stdio.h>
#include <inttypes.h>

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/cpufunc.h>
#include <util/delay.h>

#include "main.h"
#include "test.h"

extern USB_ClassInfo_CDC_Device_t VirtualSerial_CDC_Interface;

/* Resource usage:
 *  SPI  - used to scan out the next address
 *  T/C1 - For timing
 *  ADC  - For verifying supply voltages
 */

// Access bits like variables
struct bits {
  uint8_t b0:1, b1:1, b2:1, b3:1, b4:1, b5:1, b6:1, b7:1;
} __attribute__((__packed__));
#define SBIT_(port,pin) ((*(volatile struct bits*)&port).b##pin)
#define	SBIT(x,y)       SBIT_(x,y)

/* === HW mapping === */

#define LATCH        SBIT(PORTB, 0)
//      SCLK              PORTB, 1
//      MOSI              PORTB, 2
#define WR_EN        SBIT( PINB, 3)
#define VBAT         0x23  // B4 / ADC11
#define V5V          0x24  // B5 / ADC12
#define POWER_ON_B   SBIT(PORTB, 6)
#define WE_B         SBIT(PORTB, 7)
#define CE           SBIT(PORTC, 6)
#define OE_B         SBIT(PORTC, 7)
#define WDATA        PORTD
#define RDATA        PIND
#define DDATA        DDRD
#define BUTTON_B     SBIT( PINE, 2)
#define LED_A16      SBIT(PORTE, 6) // The LED doubles as A16 if we operate in 1Mbit mode

#define BANKSW       PINF
#define BANK_2       (1 << 7)
#define BANK_3       (1 << 5)
#define BANK_4       (1 << 1)
#define BANK_B       (1 << 0)
#define BANK_C       (1 << 4)
#define BANK_D       (1 << 6)

static void test_reset(void);

void test_init(void)
{
	/* Disable JTAG to free PORTF */
	MCUCR = 1 << JTD;
	MCUCR = 1 << JTD;

	/* Set up port directions and states */
	PORTB = (1 << PB6) | (1 << PB7);
	DDRB = (1 << PB0) | (1 << PB1) | (1 << PB2) | (1 << PB6) | (1 << PB7);
	PORTC = 1 << PC7;
	DDRC = (1 << PC6) | (1 << PC7);
	PORTD = 0;
	DDRD = 0;
	PORTE = (1 << PE2);
	DDRE = (1 << PE6);
	PORTF = 0;
	DDRF = 0;

	/* Set up T/C1 to count at 16us period, no interrupts */
	TCCR1A = 0;
	TIMSK1 = 0;
	TCCR1B = 4 << CS10;

	/* Set up SPI: No interrupt, Master, MSB first, idle clock low, 8 MHz */
	SPCR = (1 << SPE) | (0 << DORD) | (1 << MSTR) | (0 << CPOL) | (0 << SPR0);
	//SPSR = (1 << SPI2X);

	/* Disconnect digital inputs from ADC ports */
	DIDR2 = (1 << ADC11D) | (1 << ADC12D);

	test_reset();
}

static int16_t usb(void)
{
	CDC_Device_USBTask(&VirtualSerial_CDC_Interface);
	USB_USBTask();

	return CDC_Device_ReceiveByte(&VirtualSerial_CDC_Interface);
}

#define LOG(msg, args...) do { printf_P(PSTR(msg), ##args); usb(); } while (1==0)
#define FAIL(msg, args...) do { LOG("FAIL: " msg "\n", ##args); goto out; } while (1==0)

static uint8_t button_pressed(void)
{
	static uint8_t last_button_state = 0;
	uint8_t b = !BUTTON_B;
	uint8_t button_press = ((b != last_button_state) && b);
	last_button_state = b;
	return button_press;
}

static inline void timer_set(uint32_t us)
{
	GTCCR = 1 << PSRSYNC;        // Reset prescaler
	TCNT1 = (65536 - (us / 16)); // Set counter start value
	TIFR1 = 1 << TOV1;           // Clear overflow flag
}

#define TIMER_EXPIRED (TIFR1 & (1 << TOV1))

static void wait(uint32_t us)
{
	timer_set(us);
	while (!TIMER_EXPIRED)
		usb();
}

static void set_address(uint16_t address)
{
	SPDR = address >> 8;
	while (!(SPSR & (1 << SPIF)));
	SPDR = address & 0xFF;
	while (!(SPSR & (1 << SPIF)));
	LATCH = 1;
	LATCH = 0;
}

static void write_mode(void)
{
	OE_B = 1;
	WE_B = 1;
	DDATA = 0xFF;
	CE = 1;
}

static void read_mode(void)
{
	WDATA = 0;
	DDATA = 0;
	OE_B = 0;
	WE_B = 1;
	CE = 1;
}

static void disable_mram(void)
{
	OE_B = 1;
	WE_B = 1;
	CE = 0;
	WDATA = 0;
	DDATA = 0;
}

static void write_byte(uint16_t address, uint8_t data)
{
	set_address(address);
	WDATA = data;
//	_delay_us(1);
	WE_B = 0;
//	_delay_us(1);
	WE_B = 1;
//	_delay_us(1);
}

static uint8_t read_byte(uint16_t address)
{
	set_address(address);
//	_delay_us(2);
	return RDATA;
}

static uint8_t read_adc(uint8_t channel)
{
	ADMUX = (1 << ADLAR) | (channel & 0x1F);
	ADCSRB = (((channel >> 5) & 1) << MUX5);
	/* 250 kHz, no int */
	ADCSRA = (1 << ADEN) | (1 << ADSC) | (1 << ADIF) | (6 << ADPS0);
	while (!(ADCSRA & (1 << ADIF)));
	return ADCH;
}

static uint8_t bank_sw_stable_value = 0, bank_sw_new_value = 0, bank_sw_stable_count = 0;

static void reset_bank_sw(void)
{
	bank_sw_stable_value = 0xFF;  // impossible value since not all bits of PINF have corresponding pins
}

static uint8_t get_bank_sw(void)
{
	wait(1000);

	uint8_t b = BANKSW;
	LOG("%02X\b\b", b);
	if (b != bank_sw_stable_value) {
		if (b == bank_sw_new_value) {
			if (bank_sw_stable_count)
				bank_sw_stable_count--;
			else
				bank_sw_stable_value = bank_sw_new_value;
		} else {
			bank_sw_new_value = b;
			bank_sw_stable_count = 20;
		}
		return 0;
	}

	return 1;
}

static uint8_t get_bank_1234(void)
{
	while (!get_bank_sw() && !button_pressed());

	switch (bank_sw_stable_value & (BANK_2 | BANK_3 | BANK_4)) {
	case 0: return 1;
	case BANK_2: return 2;
	case BANK_3: return 3;
	case BANK_4: return 4;
	default:
		LOG("Invalid bank value 0x%02x\n", bank_sw_stable_value);
		return 0;
	}
}

static uint8_t get_bank_ABCD(void)
{
	while (!get_bank_sw() && !button_pressed());

	switch (bank_sw_stable_value & (BANK_B | BANK_C | BANK_D)) {
	case 0: return 1;
	case BANK_B: return 2;
	case BANK_C: return 3;
	case BANK_D: return 4;
	default:
		LOG("Invalid bank value 0x%02x\n", bank_sw_stable_value);
		return 0;
	}
}

static uint8_t wait_bank(uint8_t level, uint8_t which)
{
	uint8_t bank;

	while (1) {
		if (button_pressed())
			return 0;
		bank = level ? get_bank_ABCD() : get_bank_1234();
		if (bank == 0)
			return 0;
		if (bank == which)
			return 1;
	}
}

static uint8_t wait_wr_en(uint8_t value)
{
	uint8_t b;

	while (1) {
		if (button_pressed())
			return 0;
		b = WR_EN;
		_delay_ms(1);
		if ((WR_EN == b) && (b == value))
			return 1;
	}
}

static void test_reset(void)
{
	reset_bank_sw();
	disable_mram();
	set_address(0);
	POWER_ON_B = 1;
}

/*
 * 1. Turn on power
 * 2.2. Check VBAT (rules out shorts 3V3 side)
 * 2.1. Check CST (rules out shorts 5V side)
 * 3. Check that switches are at zero position

 * 8. Check address lines:
 *    1. Write all of first 32K
 *    2. Read back and compare
 *    3. Write second 32K
 *    4. Read first 32K -- if new data, then M-256E, else M-512E
 *    5. If M-512E: Read second 32K and check

 * 3,5. Test write protect switch

 * 4. For bank1 = A..D
 *    1. Wait until switch at matching position
 *    2. Write test data that includes bank ID
 *    3. Indicate "next bank" to user
 * 5. For bank1 = D..A
 *    1. 4.1
 *    2. Check that expected data is still there
 *    3. 4.3
 * 6. For bank2 = 1..4
 *    1. 4.1
 *    2. Write test data including bank ID and address to addresses 0 and 32K
 *    3. 4.3
 * 7. For bank2 = 4..1
 *    1. 4.1
 *    2. Read data back, two valid cases:
 *       M-256E -> 0 and 32K reads equal, all four banks different, reads yield 32K value
 *       M-512E -> 0 and 32K reads differ, banks 1,2 and 3,4 equal
 *
 */

static void test_write(uint16_t address, uint16_t words)
{
	write_mode();
	while (words--) {
		write_byte(address, address >> 8);
		write_byte(address + 1, address & 0xFF);
		address += 2;
	}
	disable_mram();
}

static uint16_t test_read(uint16_t address, uint16_t expect, uint16_t words)
{
	read_mode();
	while (words--) {
		if ((read_byte(address) != (expect >> 8)) || (read_byte(address + 1) != (expect & 0xFF)))
			break;
		address += 2;
		expect += 2;
	}
	disable_mram();
	return address;
}

static void dump(uint16_t address, uint16_t groups, uint8_t binary)
{
	uint8_t i, b[16];

	read_mode();
	while (groups--) {
		for (i = 0; i < 16; i++)
			b[i] = read_byte(address++);
		if (binary)
			fwrite(b, 16, 1, stdout);
		else
			LOG("0x%04x: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					address - 16, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	}
	disable_mram();
}

static void prog(uint16_t address, uint16_t groups)
{
	uint8_t i;
	int16_t data;

	write_mode();
	while (groups--) {
		for (i = 0; i < 16; i++) {
			do {
				data = CDC_Device_ReceiveByte(&VirtualSerial_CDC_Interface);
				if (data < 0) {
					CDC_Device_USBTask(&VirtualSerial_CDC_Interface);
					USB_USBTask();
				}
			} while (data < 0);
			write_byte(address++, data);
		}
	}
	disable_mram();
}

enum { TM_TEST = 0, TM_PROG, TM_DUMP, TM_DIOMON };
enum { TS_128 = 0, TS_256, TS_512, TS_1M };

static const PROGMEM char tm_names[][8] = {
		"Test", "Program", "Dump", "DIO mon"
};

static const PROGMEM char ts_names[][6] = {
		"128Kb", "256Kb", "512Kb", "1Mb"
};

uint8_t test_mode = TM_TEST;
uint8_t test_size = TS_256;

static void do_input(int16_t input) {
	switch (input) {
	case '1':
	case '2':
	case '3':
	case '4':
		test_size = TS_128 + (input - '1');
		break;
	case 't': test_mode = TM_TEST; break;
	case 'p': test_mode = TM_PROG; break;
	case 'd': test_mode = TM_DUMP; break;
	case 'm': test_mode = TM_DIOMON; break;
	}

	LOG("%S mode, size %S\n", tm_names + test_mode, ts_names + test_size);
}

static void do_prog(void)
{
	LOG("Feed me data!\n");
	if (test_size == TS_1M) {
		LED_A16 = 0;
		prog(0, 4096);
		LED_A16 = 1;
		prog(0, 4096);
	} else {
		prog(0, 1024 << test_size);
	}
	LOG("Thank you for your business.\n");
}

static void do_dump(void)
{
	if (test_size == TS_1M) {
		LED_A16 = 0;
		dump(0, 4096, 1);
		LED_A16 = 1;
		dump(0, 4096, 1);
	} else {
		dump(0, 1024 << test_size, 1);
	}
}

void diomon(void)
{
	static uint8_t count = 0;
	while (button_pressed());

	POWER_ON_B = 0;

	while (!button_pressed()) {
		LOG("%02x %02x %02x %02x %02x %c\n", PINB, PINC, PIND, PINF, get_bank_sw(), (count++ & 128) ? '.' : ' ');
		//wait(1000);
	}

	POWER_ON_B = 1;
}

void test(void)
{
	uint8_t b, b2, exp1, exp2, bank;
	uint16_t w;
	static enum { M256, M512 } card_type;
	static uint8_t test_failed = 0;

	int16_t input = usb();
	if (input >= 0)
		do_input(input);

	if (test_failed && TIMER_EXPIRED && test_size != TS_1M) {
		LED_A16 = !LED_A16;
		timer_set(200000);
		test_failed--;
	}

	if (!button_pressed())
		return;

	if (test_mode == TM_DIOMON) {
		diomon();
		return;
	}

	/* Start test */
	test_failed = 20;
	LED_A16 = 0;
	POWER_ON_B = 0;
	wait(20000);

	/* Check voltage rails */
	b = read_adc(V5V);
	if (test_mode == TM_TEST)
		LOG(" 5V rail level: %u.%uV\n", b * 5 / 255, (b * 50 / 255) % 10);
	if (b < (48 * 255 / 50))
		FAIL("5V level too low");

	b = read_adc(VBAT);
	if (test_mode == TM_TEST)
		LOG("3V3 rail level: %u.%uV\n", b * 5 / 255, (b * 50 / 255) % 10);
	if (b < (31 * 255 / 50) || b > (34 * 255 / 50))
		FAIL("3V3 level invalid");

	if (test_mode == TM_DUMP) {
		do_dump();
		goto pass;
	}

	/* Check switches are at zero position */
	get_bank_sw();
	if (!WR_EN)
		FAIL("Write protect engaged");

	if (test_mode == TM_PROG) {
		do_prog();
		goto pass;
	}

	b = get_bank_1234();
	if (b != 1)
		FAIL("1234 bank switch at %d", b);
	b = get_bank_ABCD();
	if (b != 1)
		FAIL("ABCD bank switch at %d", b);

	/* Write and read a full bank to test address, data and control lines */
	LOG("Writing first 32K...\n");
	test_write(0, 0x4000);

	LOG("Reading first 32K...\n");
	w = test_read(0, 0, 0x4000);
	if (w != 0x8000) {
		dump(w & ~0x1f, 4, 0);
		FAIL("Data mismatch at 0x%04x", w);
	}

	LOG("Writing second 32K...\n");
	test_write(0x8000, 0x4000);

	LOG("Reading first 32K again...\n");
	w = test_read(0, 0x8000, 0x4000);
	if (w == 0x8000) {
		LOG("M-256 card detected.\n");
		card_type = M256;
	} else if (w == 0) {
		LOG("M-512 card detected.\n");
		card_type = M512;
		w = test_read(0, 0, 0x4000);
	}

	if (w != 0x8000) {
		dump(w & ~0x1f, 4, 0);
		FAIL("Data mismatch at 0x%04x", w);
	}

	if (card_type == M512) {
		LOG("Reading second 32K...\n");
		w = test_read(0x8000, 0x8000, 0x4000);
		if (w != 0) {
			dump(w & ~0x1f, 4, 0);
			FAIL("Data mismatch at 0x%04x", w);
		}
	}

	if (!((card_type == M256 && test_size == TS_256) || (card_type == M512 && test_size == TS_512))) {
		FAIL("Unexpected size - expected %S", ts_names + test_size);
	}

	/* Test write protect switch */
	LOG("Testing write protect\n");
	LED_A16 = 1;
	wait_wr_en(0);
	LED_A16 = 0;
	wait_wr_en(1);

	/* Test bank switches */
	for (bank = 1; bank <= 4; bank++) {
		LOG("Writing ABCD bank %d\n", bank);
		if (!wait_bank(1, bank))
			FAIL("Aborted");
		write_mode();
		write_byte(0, bank);
		disable_mram();
	}

	for (bank = 4; bank >= 1; bank--) {
		LOG("Reading ABCD bank %d\n", bank);
		if (!wait_bank(1, bank))
			FAIL("Aborted");
		read_mode();
		b = read_byte(0);
		disable_mram();
		if (b != bank)
			FAIL("Expected 0x%02x, read 0x%02x", bank, b);
	}

	for (bank = 1; bank <= 4; bank++) {
		LOG("Writing 1234 bank %d\n", bank);
		if (!wait_bank(0, bank))
			FAIL("Aborted");
		write_mode();
		write_byte(0, bank);
		write_byte(0x8000, bank | 0xA0);
		disable_mram();
	}

	for (bank = 4; bank >= 1; bank--) {
		LOG("Reading 1234 bank %d\n", bank);
		if (!wait_bank(0, bank))
			FAIL("Aborted");
		read_mode();
		b = read_byte(0);
		b2 = read_byte(0x8000);
		disable_mram();

		/*
		 *       M-256E -> 0 and 32K reads equal, all four banks different, reads yield 32K value
		 *       M-512E -> 0 and 32K reads differ, banks 1,2 and 3,4 equal
		 */
		if (card_type == M512) {
			exp1 = (bank + 1) & 14;
			exp2 = exp1 | 0xA0;
		} else {
			exp1 = exp2 = bank | 0xA0;
		}

		if ((b != exp1) || (b2 != exp2))
			FAIL("Expected 0x%02x/0x%02x, read 0x%02x/0x%02x", exp1, exp2, b, b2);
	}

	LED_A16 = 1;
	LOG("PASS \\o/\n");

pass:
	test_failed = 0;

out:
	test_reset();
}
