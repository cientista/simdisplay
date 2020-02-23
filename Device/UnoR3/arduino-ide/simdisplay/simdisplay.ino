/*
  simdisplay - A simracing dashboard created using Arduino to show shared memory
             telemetry from Assetto Corsa Competizione.

  Copyright (C) 2020  Filippo Erik Negroni

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <inttypes.h>

#include <LiquidCrystal.h>

#include "SimDisplayProtocol.h"

// Pinouts for the four groups of outputs: Redline, Gear, Damage and Lcd.

// Redline, Gear and Damage use a 74HC595N IC to drive groups of LEDs.
// The three groups are update sequentially, so they can all share
// the same CLOCK, DATA, CLEAR and ENABLE pins, but each one has its own LATCH pin.
// We do not need complicate the code to write to all of them in parallel
// because Redline changes very frequently, while Gear and especially Damage
// only change occasionally.
// We use the CLEAR pin to fast clear them all together.
// We use the ENABLE pin so we can disable them all together.

// Redline pinout.
enum RedlinePins {
	RL_LATCH_PIN = 2,
	RL_CLOCK_PIN = 5,
	RL_DATA_PIN = 6,
};

// Gear indicator pinout.
enum GearPins {
	GEAR_LATCH_PIN = 3,
	GEAR_CLOCK_PIN = 5,
	GEAR_DATA_PIN = 6,
};

// Damage pinout.
enum DamagePins {
	DMG_LATCH_PIN = 4,
	DMG_CLOCK_PIN = 5,
	DMG_DATA_PIN = 6,
};

// Enable and clear all leds.
enum AllLedPins {
	ALL_CLEAR_PIN = 8,
	ALL_ENABLE_PIN = 9,
};

// LCD display pinout.
enum LcdPins {
	LCD_RS_PIN = 18, // A4
	LCD_ENABLE_PIN = 19, // A5
	LCD_D4_PIN = 14, // A0
	LCD_D5_PIN = 15, // A1
	LCD_D6_PIN = 16, // A2
	LCD_D7_PIN = 17  // A3
};

// ------------------------------  0123456789012345
static char LCD_MASK_ROW0[] = "-   --.-%   -/- ";
static char LCD_MASK_ROW1[] = "R--  M-   --/--C";

#define LCD_ABS_COLROW      0,  0
#define LCD_REMLAPS_COLROW  1,  1
#define LCD_TC_COLROW       11, 0
#define LCD_TCC_COLROW      14, 0
#define LCD_BB_COLROW       4,  0
#define LCD_MAP_COLROW      6,  1
#define LCD_AIRT_COLROW     10, 1
#define LCD_ROADT_COLROW    13, 1

LiquidCrystal lcd(LCD_RS_PIN, LCD_ENABLE_PIN, LCD_D4_PIN, LCD_D5_PIN, LCD_D6_PIN, LCD_D7_PIN);

static struct SimDisplayPacket packet[2];
static struct SimDisplayPacket *newPacket = &packet[0];
static struct SimDisplayPacket *oldPacket = &packet[1];

static void printLcd(char *str, int col, int row)
{
	lcd.setCursor(col, row);
	lcd.print(str);
}

static void printLcdMask(void)
{
	printLcd(LCD_MASK_ROW0, 0, 0);
	printLcd(LCD_MASK_ROW1, 0, 1);
}

static void printLcdField(int newval, int oldval, const char *zerostr, const char *fmtstr, int col, int row)
{
	static char strbuffer[17];
	
	if (newval != oldval) {
		if (0 == newval) {
			strcpy(strbuffer, zerostr);
		} else {
			sprintf(strbuffer, fmtstr, newval);
		}
		printLcd(strbuffer, col, row);
	}
}

static void printLcdFieldBB(int newval, int oldval, int col, int row)
{
static char strbuffer[17];

	if (newval != oldval) {
		if (0 == newval) {
			strcpy(strbuffer, "--.-");
		} else {
			sprintf(strbuffer, "%d.%d", newval / 10, newval % 10);
		}
		printLcd(strbuffer, col, row);
	}
}

static void printLcdFields(void)
{
	printLcdField(newPacket->abs, oldPacket->abs, "- ", "%-2d", LCD_ABS_COLROW);
	printLcdField(newPacket->remlaps, oldPacket->remlaps, "--", "%2d", LCD_REMLAPS_COLROW);
	printLcdField(newPacket->tc, oldPacket->tc, " -", "%2d", LCD_TC_COLROW);
	printLcdField(newPacket->tcc, oldPacket->tcc, "- ", "%-2d", LCD_TCC_COLROW);
	printLcdFieldBB(newPacket->bb, oldPacket->bb, LCD_BB_COLROW);
	printLcdField(newPacket->map, oldPacket->map, "- ", "%-2d", LCD_MAP_COLROW);
	printLcdField(newPacket->airt, oldPacket->airt, "--", "%2d", LCD_AIRT_COLROW);
	printLcdField(newPacket->roadt, oldPacket->roadt, "--", "%2d", LCD_ROADT_COLROW);
}

static void writeLeds(uint8_t latchpin, uint8_t datapin, uint8_t clockpin, uint8_t pattern)
{
	digitalWrite(latchpin, LOW);
	shiftOut(datapin, clockpin, MSBFIRST, pattern);
	digitalWrite(latchpin, HIGH);
}

static void writeRedline(uint8_t pattern)
{
	writeLeds(RL_LATCH_PIN, RL_DATA_PIN, RL_CLOCK_PIN, pattern);
}

static void clearRedline(void)
{
	writeRedline(B00000000);
}

// clearAllLeds clears all led groups, useful at reset.
static void clearAllLeds(void)
{
	digitalWrite(RL_LATCH_PIN, LOW);
	digitalWrite(GEAR_LATCH_PIN, LOW);
	digitalWrite(DMG_LATCH_PIN, LOW);
	digitalWrite(ALL_CLEAR_PIN, LOW);
	digitalWrite(RL_LATCH_PIN, HIGH);
	digitalWrite(GEAR_LATCH_PIN, HIGH);
	digitalWrite(DMG_LATCH_PIN, HIGH);
	digitalWrite(ALL_CLEAR_PIN, HIGH);
}

// enableAllLeds enables or disables all led groups, useful on pause.
// parameter enable can be 1 to enable or 0 to disable.
static void enableAllLeds(int enable)
{
	digitalWrite(ALL_ENABLE_PIN, enable ? LOW : HIGH);
}

static void printRedline(void)
{
	static unsigned long bktm = 0;
	static uint8_t bksta = B00000000;
	static const unsigned long bkint = 100;

	if (newPacket->rpm > newPacket->shftrpm) {
		if (bktm > 0 && millis() - bktm < bkint) {
			return;
		}
		writeRedline(bksta = ~bksta);
		bktm = millis();
		return;
	}
	
	bktm = 0;
	bksta = B00000000;
	
	if (newPacket->rpm > newPacket->optrpm) {
		static const int RLSTAGES = 8;
		static uint8_t patterns[RLSTAGES] = {
			B10000000,
			B11000000,
			B11100000,
			B11110000,
			B11111000,
			B11111100,
			B11111110,
			B11111111,
		};
	
		int stage = (newPacket->rpm - newPacket->optrpm) / ((newPacket->shftrpm - newPacket->optrpm) / RLSTAGES);
		writeRedline(patterns[stage]);
		return;
	}
	
	clearRedline();
}

static void writeGear(uint8_t pattern)
{
	writeLeds(GEAR_LATCH_PIN, GEAR_DATA_PIN, GEAR_CLOCK_PIN, pattern);
}

static void printGear(void)
{
	enum Patterns {
		GEAR_R = B00000001,
		GEAR_N = B00000010,
		GEAR_1 = B00000100,
		GEAR_2 = B00001000,
		GEAR_3 = B00010000,
		GEAR_4 = B00100000,
		GEAR_5 = B01000000,
		GEAR_6 = B10000000,
		GEAR_7 = B11111111,
	};

	static uint8_t gearPatterns[] = {
		GEAR_R, GEAR_N, GEAR_1, GEAR_2, GEAR_3, GEAR_4, GEAR_5, GEAR_6
	};

	if (newPacket->gear != oldPacket->gear) {
		writeGear(gearPatterns[newPacket->gear]);
	}
}

void setup()
{
	pinMode(LED_BUILTIN, OUTPUT);
	lcd.begin(16, 2);
	printLcdMask();
	
	pinMode(RL_LATCH_PIN, OUTPUT);
	pinMode(RL_CLOCK_PIN, OUTPUT);
	pinMode(RL_DATA_PIN, OUTPUT);
	
	pinMode(GEAR_LATCH_PIN, OUTPUT);
	pinMode(GEAR_CLOCK_PIN, OUTPUT);
	pinMode(GEAR_DATA_PIN, OUTPUT);
	
	pinMode(DMG_LATCH_PIN, OUTPUT);
	pinMode(DMG_CLOCK_PIN, OUTPUT);
	pinMode(DMG_DATA_PIN, OUTPUT);
	
	pinMode(ALL_CLEAR_PIN, OUTPUT);
	pinMode(ALL_ENABLE_PIN, OUTPUT);
	
	clearAllLeds();
	enableAllLeds(1);

	Serial.begin(9600);
}

void loop()
{
	static const int packetsize = sizeof(struct SimDisplayPacket);
	while (Serial.available() >= packetsize) {
		unsigned long time = micros();

		if (packetsize > Serial.readBytes((byte *)newPacket, packetsize)) {
			// something went wrong, we reset ourselves.
			Serial.end();
			delay(2000);
			printLcdMask();
			clearAllLeds();
			enableAllLeds(true);
			Serial.begin(9600);
			break;
		}
	
		printRedline();
		printGear();
		printLcdFields();
		struct SimDisplayPacket *tmp = oldPacket;
		oldPacket = newPacket;
		newPacket = tmp;
	
		time = micros() - time;
	
		if (time > 20000) {
			digitalWrite(LED_BUILTIN, HIGH);
		}
	}
}
