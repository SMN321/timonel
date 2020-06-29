/*
  timonel-twim-ss.h
  ==================
  Timonel library test header (Single Slave) v1.5
  ----------------------------------------------------------------------------
  2020-06-22 Gustavo Casanova
  ----------------------------------------------------------------------------
*/

#ifndef TIMONEL_TWIM_SS_H
#define TIMONEL_TWIM_SS_H

#include <NbMicro.h>
#include <TimonelTwiM.h>
#include <TwiBus.h>

// Prototypes
void setup(void);
void loop(void);
void ListTwiDevices(uint8_t sda = 0, uint8_t scl = 0);
void PrintStatus(Timonel tml);
void ReadChar(void);
uint16_t ReadWord(void);
void ShowHeader(void);
void ShowMenu(void);
void ClrScr(void);
void PrintLogo(void);
void RotaryDelay(void);

#endif  // TIMONEL_TWIM_SS_H