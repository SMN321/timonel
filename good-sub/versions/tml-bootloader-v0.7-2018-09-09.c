/*           _                         _
 *       _  (_)                       | |
 *     _| |_ _ ____   ___  ____  _____| |
 *    (_   _) |    \ / _ \|  _ \| ___ | |
 *      | |_| | | | | |_| | | | | ____| |
 *       \__)_|_|_|_|\___/|_| |_|_____)\_)
 *
 *   Timonel - I2C Bootloader for ATiiny85 MCUs
 *	 Author: Gustavo Casanova
 *	 ...........................................
 *	 Version: 0.7 / 2018-09-07
 *	 gustavo.casanova@nicebots.com
 */

/* Includes */
#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stddef.h>
#include <stdbool.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include "nb-libs/nb-usitwisl-if.h"
#include "nb-libs/nb-i2c-cmd.h"

// Definitions
#define PAGE_SIZE	64		/* SPM Flash memory page size */
#define RESET_PAGE	0		/* Interrupt vector table address start location */
#define MAXBUFFERTXLN	5		/* Maximum page buffer TX size */

#ifndef __AVR_ATtiny85__
	#define __AVR_ATtiny85__
	#pragma message "   >>>   Run, Timonel, run!   <<<   "
#endif

#if TIMONEL_START % PAGE_SIZE != 0
	#error "TIMONEL_START in makefile must be a multiple of chip's pagesize"
#endif

#if PAGE_SIZE > 256
	#error "Timonel only supports pagesizes up to 256 bytes"
#endif

#define CMD_READPAGE 	false	/* This is used mostly for debugging, it takes ~126 bytes of memory. */
														/* Change TIMONEL_START in Makefile.inc to 1900 or lower to compile. */
#ifndef F_CPU
	#define F_CPU 8000000UL	/* Default CPU speed for delay.h */
#endif

#define I2C_ADDR	0x0A	/* Timonel I2C Address: 0xA = 10 */

#define TIMONEL_VER_MJR	0	/* Timonel version major number */
#define TIMONEL_VER_MNR	7	/* Timonel version major number */

#define LED_UI_PIN		PB1			/* >>> Use PB1 to monitor activity. <<< */
#define LED_UI_DDR		DDRB		/* >>> WARNING! This is not for use <<< */
#define LED_UI_PORT		PORTB		/* >>> in production!               <<< */
#define TOGGLETIME		0xFFFF	/* LED toggle delay before initialization */
#define I2CDLYTIME		0x7FFF	/* Main loop times to allow the I2C responses to finish */
#define RXDATASIZE		8				/* RX data size for WRITBUFF command */
#define CYCLESTOEXIT		80			/* Main loop cycles before exit to app if not initialized */

#if (RXDATASIZE > 8)
	#pragma GCC warning "Do not set transmission data too high to avoid hurting I2C reliability!"
#endif

#if ((CYCLESTOEXIT > 0) && (CYCLESTOEXIT < 80))
	#pragma GCC warning "Do not set CYCLESTOEXIT too low, it could make difficult for I2C master to initialize on time!"
#endif

#define SR_INIT_1	0			/* Status Register Bit 1 (1)  : Initialized 1 */
#define SR_INIT_2	1			/* Status Register Bit 2 (2)  : Initialized 2 */
#define SR_DEL_FLASH	2			/* Status Register Bit 3 (4)  : Delete flash  */
#define SR_APP_READY	3			/* Status Register Bit 4 (8)  : Application flased OK, ready to run */
#define SR_EXIT_TML	4			/* Status Register Bit 5 (16) : Exit Timonel & Run Application */
#define SR_BIT_6	5			/* Status Register Bit 6 (32) : Not used */
#define SR_BIT_7	6			/* Status Register Bit 7 (64) : Not used */
#define SR_BIT_8	7			/* Status Register Bit 8 (128): Not used */

// Type definitions
typedef uint8_t byte;
typedef uint16_t word;
typedef void (*fptr_t)(void);

// Global variables
byte command[(RXDATASIZE * 2) + 2] = { 0 };	/* Command received from I2C master */
byte commandLength = 0;  			/* Command number of bytes */
word ledToggleTimer = 0;			/* Pre-init led toggle timer */
byte statusRegister = 0;			/* Bit: 8,7,6,5: Not used, 4: exit, 3: delete flash, 2, 1: initialized */
word i2cDly = I2CDLYTIME;			/* Delay to allow I2C execution before jumping to application */
byte exitDly = CYCLESTOEXIT;			/* Delay to exit Timonel and run the application if not initialized */
byte pageBuffer[PAGE_SIZE];			/* Flash memory page buffer */
word flashPageAddr = 0x0000;			/* Flash memory page address */
byte pageIX = 0;				/* Flash memory page index */
byte tplJumpLowByte = 0;			/* Trampoline jump address LSB */
byte tplJumpHighByte = 0;			/* Trampoline jump address MSB */

// Jump to trampoline
fptr_t RunApplication = (fptr_t)((TIMONEL_START - 2) / 2);
//const uint8_t rstVector[2]  __attribute__ ((section (".appvector"))) = { 0xaa, 0xbb };

// Function prototypes
void SetCPUSpeed8MHz (void);
void ReceiveEvent(byte commandBytes);
void RequestEvent(void);
void Reset(void);
void DisableWatchDog(void);
void DeleteFlash(void);
void __attribute__ ((noinline)) ClearPageBuffer();
void FixResetVector();
void FlashRaw(word pageAddress);
void FlashPage(word pageAddress);
void CreateTrampoline(void);
void CalculateTrampoline(byte applJumpLowByte, byte applJumpHighByte);

// Function Main
int main() {
	// ###################
	// ### Setup Block ###
	// ###################
	DisableWatchDog();					/* Disable watchdog to avoid continuous loop after reset */
	LED_UI_DDR |= (1 << LED_UI_PIN);			/* Set led pin Data Direction Register for output */
	SetCPUSpeed8MHz();					/* Set the CPU prescaler for 8 MHz */
	UsiTwiSlaveInit(I2C_ADDR);				/* Initialize I2C */
	Usi_onReceiverPtr = ReceiveEvent;			/* I2C Receive Event declaration */
	Usi_onRequestPtr = RequestEvent;			/* I2C Request Event declaration */
	statusRegister = (1 << SR_APP_READY);			/* In principle, we assume that there is a valid app in memory */
	//TCCR1 = 0;	    					/* Set Timer1 off */
	cli();							/* Disable Interrupts */
	// ###################
	// ###  Main Loop  ###
	// ###################
	for (;;) {
		// Initialization check
		if ((statusRegister & ((1 << SR_INIT_1) + (1 << SR_INIT_2))) != \
		((1 << SR_INIT_1) + (1 << SR_INIT_2))) {
			// ============================================
			// = Blink led until is initialized by master =
			// ============================================
			if (ledToggleTimer++ >= TOGGLETIME) {
				LED_UI_PORT ^= (1 << LED_UI_PIN);	/* Blinks on each main loop pass at TOGGLETIME intervals */
				ledToggleTimer = 0;
				if (exitDly-- == 0) {
					RunApplication();
				}
			}
		}
		else {
			if (i2cDly-- <= 0) {				/* Decrease i2cDly on each main loop pass until it    */
				//					/* reaches 0 before running code to allow I2C replies */
				//LED_UI_PORT &= ~(1 << LED_UI_PIN);	/* Turn led off to indicate correct initialization ... */
				//
				// =======================================
				// = Exit bootloader and run application =
				// =======================================
				if ((statusRegister & ((1 << SR_EXIT_TML) + (1 << SR_APP_READY))) == \
				((1 << SR_EXIT_TML) + (1 << SR_APP_READY))) {
					RunApplication();
				}
				if ((statusRegister & ((1 << SR_EXIT_TML) + (1 << SR_APP_READY))) == \
				(1 << SR_EXIT_TML)) {
					DeleteFlash();
				}
				// ===========================================================================
				// = Delete application from flash memory and point reset to this bootloader =
				// ===========================================================================
				if ((statusRegister & (1 << SR_DEL_FLASH)) == (1 << SR_DEL_FLASH)) {
					LED_UI_PORT |= (1 << LED_UI_PIN);	/* Turn led on to indicate erasing ... */
					DeleteFlash();
					RunApplication();			/* Since there is no app anymore, this resets to the bootloader */
				}
				// ========================================================================
				// = Write received page to flash memory and prepare to receive a new one =
				// ========================================================================
				if ((pageIX == PAGE_SIZE) /* & (flashPageAddr != 0xFFFF)*/) {
					LED_UI_PORT ^= (1 << LED_UI_PIN);	/* Turn led on and off to indicate writing ... */
					FlashPage(flashPageAddr);
					flashPageAddr += PAGE_SIZE;
					pageIX = 0;
				}
				i2cDly = I2CDLYTIME;
			}
		}
		// ==================================================
		// = I2C Interrupt Emulation ********************** =
		// = Check the USI Status Register to verify        =
		// = whether a USI start handler should be launched =
		// ==================================================
		if (USISR & (1 << USISIF)) {
			UsiStartHandler();				/* If so, run the USI start handler ... */
			USISR |= (1 << USISIF);				/* Reset the USI start flag in USISR register to prepare for new ints */
		}
		// =====================================================
		// = I2C Interrupt Emulation ************************* =
		// = Check the USI Status Register to verify           =
		// = whether a USI counter overflow should be launched =
		// =====================================================
		if (USISR & (1 << USIOIF)) {
			UsiOverflowHandler();				/* If so, run the USI overflow handler ... */
			USISR |= (1 << USIOIF);				/* Reset the USI overflow flag in USISR register to prepare for new ints */
		}
	}
	// Return
	return 0;
}

// I2C Receive Event
void ReceiveEvent(byte commandBytes) {
	commandLength = commandBytes; 				/* Save the number of bytes sent by the I2C master */
	for (byte i = 0; i < commandLength; i++) {
		command[i] = UsiTwiReceiveByte();		/* Store the data sent by the I2C master in the data buffer */
	}
}

// I2C Request Event
void RequestEvent(void) {
	byte opCodeAck = ~command[0];				/* Command Operation Code reply => Command Bitwise "Not" */
	switch (command[0]) {
		// ******************
		// * GETTMNLV Reply *
		// ******************
		case GETTMNLV: {
			#define GETTMNLV_RPLYLN 8
			byte reply[GETTMNLV_RPLYLN] = { 0 };
			reply[0] = opCodeAck;
			reply[1] = 78;						/* N */
			reply[2] = 66;						/* B */
			reply[3] = 84;						/* T */
			reply[4] = TIMONEL_VER_MJR;				/* Timonel Major version number	*/
			reply[5] = TIMONEL_VER_MNR;				/* Timonel Minor version number */
			reply[6] = ((TIMONEL_START & 0xFF00) >> 8);		/* Timonel Base Address MSB		*/
			reply[7] = (TIMONEL_START & 0xFF);			/* Timonel Base Address LSB		*/
			statusRegister |= (1 << SR_INIT_2);			/* Two-step init step 2: receive GETTMNLV command */
			for (byte i = 0; i < GETTMNLV_RPLYLN; i++) {
				UsiTwiTransmitByte(reply[i]);
			}
			break;
		}
		// ******************
		// * EXITTMNL Reply *
		// ******************
		case EXITTMNL: {
			#define EXITTMNL_RPLYLN 1
			byte reply[EXITTMNL_RPLYLN] = { 0 };
			reply[0] = opCodeAck;
			for (byte i = 0; i < EXITTMNL_RPLYLN; i++) {
				UsiTwiTransmitByte(reply[i]);
			}
			statusRegister |= (1 << SR_EXIT_TML);
			break;
		}
		// ******************
		// * DELFLASH Reply *
		// ******************
		case DELFLASH: {
			#define DELFLASH_RPLYLN 1
			byte reply[DELFLASH_RPLYLN] = { 0 };
			reply[0] = opCodeAck;
			for (byte i = 0; i < DELFLASH_RPLYLN; i++) {
				UsiTwiTransmitByte(reply[i]);
			}
			statusRegister |= (1 << SR_DEL_FLASH);
			break;
		}
		// ******************
		// * STPGADDR Reply *
		// ******************
		case STPGADDR: {
			#define STPGADDR_RPLYLN 2
			byte reply[STPGADDR_RPLYLN] = { 0 };
			flashPageAddr = ((command[1] << 8) + command[2]); /* Sets the flash memory page base address */
			reply[0] = opCodeAck;
			reply[1] = (byte)(command[1] + command[2]);	  /* Returns the sum of MSB and LSB of the page address */
			for (byte i = 0; i < STPGADDR_RPLYLN; i++) {
				UsiTwiTransmitByte(reply[i]);
			}
			break;
		}
		// ******************
		// * WRITPAGE Reply *
		// ******************
		case WRITPAGE: {
			#define WRITPAGE_RPLYLN 2
			uint8_t reply[WRITPAGE_RPLYLN] = { 0 };
			reply[0] = opCodeAck;
			for (uint8_t i = 1; i < (RXDATASIZE + 1); i++) {
				pageBuffer[pageIX] = (command[i]);
				reply[1] += (uint8_t)(command[i]);
				pageIX++;
			}
			if (reply[1] != command[RXDATASIZE + 1]) {
				statusRegister &= ~(1 << SR_APP_READY);		/* Payload received with errors, don't run it !!! */
				statusRegister |= (1 << SR_DEL_FLASH);		/* Delete Payload !!!*/
			}
			for (uint8_t i = 0; i < WRITPAGE_RPLYLN; i++) {
				UsiTwiTransmitByte(reply[i]);
			}
			break;
		}
#if CMD_READPAGE
		// ******************
		// * READPAGE Reply *
		// ******************
		case READPAGE: {
			// command[0] : OpCode
			// command[1] : Start Position
			// command[2] : Requested Bytes
			uint8_t ix = command[1];			/* Second byte received determines start of reply data */
			const uint8_t ackLng = (command[2] + 2);	/* Third byte received determines the size of reply data */
			uint8_t reply[ackLng];
			reply[ackLng - 1] = 0;				/* Checksum initialization */
			reply[0] = opCodeAck;
			if ((ix > 0) & (ix <= PAGE_SIZE) & (command[2] >= 1) & (command[2] <= MAXBUFFERTXLN * 2)) {
				uint8_t j = 1;
				reply[ackLng - 1] = 0;
				for (uint8_t i = 1; i < command[2] + 1; i++) {
					reply[j] = pageBuffer[ix + i - 2];	/* Data bytes in reply */
					reply[ackLng - 1] += reply[j];		/* Checksum accumulator to be sent in the last byte of the reply */
					j++;
				}
				//reply[ackLng - 1] = CalculateCRC(reply, ackLng - 1);	/* Prepare CRC for Reply */
				for (uint8_t i = 0; i < ackLng; i++) {
					UsiTwiTransmitByte(reply[i]);
				}
			}
			else {
				UsiTwiTransmitByte(UNKNOWNC);		/* Incorrect operand value received */
			}
			break;
		}
#endif
		// ******************
		// * INITTINY Reply *
		// ******************
		case INITTINY: {
			#define INITTINY_RPLYLN 1
			byte reply[INITTINY_RPLYLN] = { 0 };
			reply[0] = opCodeAck;
			//PORTB |= (1 << LED_UI_PIN);  // turn LED_UI_PIN on, it will be turned off by next toggle
			//PORTB &= ~(1 << PB1); // turn PB1 off (led pin)
			//statusRegister |= (1 << (SR_INIT_1 - 1)) + (1 << (SR_INIT_2 - 1)); /* Single-step init */
			statusRegister |= (1 << SR_INIT_1);		/* Two-step init step 1: receive INITTINY command */
			for (byte i = 0; i < INITTINY_RPLYLN; i++) {
				UsiTwiTransmitByte(reply[i]);
			}
			break;
		}
		default: {
			for (byte i = 0; i < commandLength; i++) {
				UsiTwiTransmitByte(UNKNOWNC);
			}
			break;
		}
	}
}

// Function DisableWatchDog
void DisableWatchDog(void) {
    MCUSR = 0;
    WDTCR = ((1 << WDCE) | (1 << WDE));
    WDTCR = ((1 << WDP2) | (1 << WDP1) | (1 << WDP0));
}

// Function SetCPUSpeed8MHz
void SetCPUSpeed8MHz(void) {
	cli();								/* Disable interrupts */
	CLKPR = (1 << CLKPCE);						/* Mandatory for setting CPU prescaler */
	CLKPR = (0x00);							/* Set CPU prescaler 1 (System clock / 1) */
}

// Function DeleteFlash
void DeleteFlash(void) {
	ClearPageBuffer();						/* Fill page buffer with 0xff */
    FixResetVector();							/* Preserve Timonel reset vector */
    boot_spm_busy_wait();
    boot_page_erase(0); 						/* Erase the first page or else it will not write correctly */
    FlashRaw(0);							/* Restore just the initial vector */
    word address = PAGE_SIZE;
    FlashPage(address);

    while (address < TIMONEL_START) {
        boot_spm_busy_wait();
        boot_page_erase(address);
        address += PAGE_SIZE;
    }
}

// Function ClearPageBuffer
void __attribute__ ((noinline)) ClearPageBuffer() {
    for (byte i = 0; i < PAGE_SIZE; i++) {
        pageBuffer[i] = 0xff;
    }
}

// Function FixResetVector
void FixResetVector() {
    // Replace page buffer payload reset vector with bootloader reset vector
	pageBuffer[0] = (byte)((0xC000 + (TIMONEL_START / 2) - 1) & 0xff);
	pageBuffer[1] = (byte)((0xC000 + (TIMONEL_START / 2) - 1) >> 8);
	// This only preserves the current reset vector, it doesn't force the bootloader address ...
    //for (byte i = 2; i != 0; i--) {
    //    pageBuffer[i - 1] = pgm_read_byte(RESET_PAGE + i - 1);
    //}
}

// Function FlashPage
void FlashPage(word pageAddress) {
    // Remove page internal addresses, keep only base addresses
    pageAddress &= ~(PAGE_SIZE - 1);
    // If this is page 0, modify the reset vector to point to this
	// bootloader and calculate the application jump trampoline bytes ...
    if (pageAddress == RESET_PAGE) {
        // Calculate Trampoline Reset Vector
		CalculateTrampoline(pageBuffer[0], pageBuffer[1]);
		FixResetVector();
    }
	// If the application uses the trampoline page, fix again trampoline bytes ...
	if (pageAddress == (TIMONEL_START - PAGE_SIZE)) {
		pageBuffer[62] = tplJumpLowByte;
		pageBuffer[63] = tplJumpHighByte;
	}
    // Discard attempts to flash bootloader section
    if (pageAddress >= TIMONEL_START) {
        return;
    }
    FlashRaw(pageAddress);						/* Call flashRaw to make the actual writing */
	if (pageAddress == RESET_PAGE) {
		CreateTrampoline();
		flashPageAddr = RESET_PAGE;
	}
}

// Function FlashRaw
void FlashRaw(word pageAddress) {
    for (byte i = 0; i < PAGE_SIZE; i += 2) {
        word tempWord = ((pageBuffer[i + 1] << 8) | pageBuffer[i]);
        boot_spm_busy_wait();
        boot_page_fill(pageAddress + i, tempWord); 			/* Fill the temporary buffer with the given data */
    }
    // Write page from temporary buffer to the given location in flash memory
    boot_spm_busy_wait();
    boot_page_write(pageAddress);
}

// Function CreateTrampoline
void CreateTrampoline(void) {
	if (flashPageAddr < (TIMONEL_START - PAGE_SIZE)) {
		flashPageAddr = TIMONEL_START - PAGE_SIZE;
		for (int i = 0; i < PAGE_SIZE - 2; i++) {
			pageBuffer[i] = 0xff;
		}
		pageBuffer[62] = tplJumpLowByte;
		pageBuffer[63] = tplJumpHighByte;
		FlashRaw(flashPageAddr);
	}
}

// Function CalculateTrampoline
void CalculateTrampoline(byte applJumpLowByte, byte applJumpHighByte) {
	word jumpOffset = (~(TIMONEL_START - (((((applJumpHighByte << 8) + applJumpLowByte) + 1) & 0x0FFF) << 1)) >> 1);
	jumpOffset++;
	tplJumpLowByte = (jumpOffset & 0xFF);
	tplJumpHighByte = (((jumpOffset & 0xF00) >> 8) + 0xC0);
	//
	// Reference: https://drive.google.com/open?id=1CbJnCkh9XVg-G4aWQpTDD5isyDQ4WarV
	//
}
