#include <msp430.h>
#include <intrinsics.h>

#include <stdio.h>
#include "header.h"
#include "drum_utils.h"
#include "fifo_utils.h"
#include "utils.h"

// Variables
static volatile uint32_t virtualClock = 0;
static struct etimer et;
static volatile uint8_t playNow = 0;
static char debugStr[50];

// Functions
void updateClock();
uint32_t getVirtualClock();
void setVirtualClock(uint32_t value);

PROCESS(main_process, "Main Task");
AUTOSTART_PROCESSES(&main_process);

PROCESS_THREAD(main_process, ev, data)
{
	PROCESS_BEGIN();	
	
	// Sets up the watchdog timer to use ACLK input and an interval of 1s
	WDTCTL = WDTPW + WDTSSEL0 + WDTHOLD + WDTIS2; //use WDTID1 + WDTIS0 for 16s
	WDTCTL = (WDTCTL_L&~(WDTHOLD))+ WDTPW; 	// Start the watchdog

	// Turn off LEDs and init as outputs
	G_OFF(); G_OUT();
	R_OFF(); R_OUT();
	Y_OFF(); Y_OUT();

	// Begin status logging
	statusLog("Starting up the system RAD_TEAM");

	// Set up virtual clock timer
	TA1CTL = TASSEL0 + TAIE + MC0; // ACLK @ 32 kHz
	TA1CCR0 = 0x0400; // top value = 1024 -> interrupt at 64 Hz
	TA1CCTL0 = CCIE;

	// Start receiving over radio
	static uint8_t msg[10] = "";
	static uint8_t oldMsgCnt = 0;
	static uint8_t newMsgCnt = 0;
	rf1a_start_rx();

	// Set up motor output
	P1DIR |= LEFT_H;
	P1DIR |= LEFT_R;
	P2DIR = RIGHT_H + RIGHT_R;

	while(1)
	{
		kickWatchdog();

		// Process received wizzimote messages
		getReceivedMessage(msg, &newMsgCnt);
		if(oldMsgCnt != newMsgCnt)
		{
			Y_T();
			oldMsgCnt = newMsgCnt;

			if(msg[0] == CLKREQ) // clock request message, for synchronization
			{	
				char returnMsg[6];
				returnMsg[0] = CLKREQ | ACK;
				returnMsg[1] = MY_ID;
				*((uint32_t*)(&returnMsg[2])) = getVirtualClock();
				unicast_send(returnMsg,6, 0xF);
				//debug info
				sprintf(debugStr,"Sent CLKREQ ACK message %d, %d, %d", returnMsg[0], returnMsg[1], (*((uint32_t*)(&returnMsg[2]))) );
				debugLog(debugStr);
			}
			else if(msg[0] == SETCLK) // set clock message, for synchronization
			{
				uint32_t adjustment = *((uint32_t*)(&msg[2]));
				updateClock(adjustment);
				//debug info
				sprintf(debugStr,"Received SETCLK message %d, %d, %d", 
					msg[0], msg[1], (*((uint32_t*)(&msg[2]))) );
				//debugLog(debugStr);
			}
			else if(msg[0] == SCHDL) // schedule message, for playing drums
			{
				// if this drum's bit is set, add to FIFO queue
				if((msg[1] & MY_ID) != 0x0){
					writeFifo(*((uint32_t*)(&msg[2])));
					sprintf(debugStr,"Added a message to the FIFO queue: %d", (*((uint32_t*)(&msg[2]))) );
					debugLog(debugStr);
				}
				//debug info
				sprintf(debugStr,"Received SCHDL message %d, %d, %d", msg[0], msg[1], (*((uint32_t*)(&msg[2]))) );
				//debugLog(debugStr);
			}
			else if(msg[0] == CANCEL)
			{
				// cancel all scheduled hits by clearing the fifo queue
				clearFifo();
				//debug info
				sprintf(debugStr,"Received CANCEL message %d, %d, %d", msg[0], msg[1], (*((uint32_t*)(&msg[2]))) );
				//debugLog(debugStr);
			}
			else if(msg[0] == 0x0) // hit now message, for playing drums
			{
				//debug info
				sprintf(debugStr,"Received hit message %d, %d, %d", msg[0], msg[1], (*((uint32_t*)(&msg[2]))) );
				debugLog(debugStr);
				// if this drum's bit is set, play now
				if((msg[1] & MY_ID) != 0){
					playNow = 1;
					debugLog("This hit message was for me!");
				}
			}
		}

		// Check for a scheduled hit
		uint32_t clk;
		int failure = peekFifo(&clk);
		if(!failure && (clk == getVirtualClock())){
			playNow = 1;
			readFifo(&clk);
			debugLog("Playing from the FIFO queue.");
		}


		// Retract stick, if applicable
		static uint32_t retractTime = 0;
		static uint8_t stickStatus = READY;
		#ifdef SINGLE_STICK
			if(getVirtualClock() == retractTime)
			{
				if(stickStatus == HIT)
				{
					hitDrum(0);
					stickStatus = RETRACTED;
					retractTime = getVirtualClock() + (COOLDOWN*64);
					debugLog("Retracted the drum stick.");
				}
				else if(stickStatus == RETRACTED)
				{
					stickStatus = READY;
					debugLog("Drum stick is now ready.");
				}
			}
		#else
			if(getVirtualClock() == retractTime)
			{
				if(stickStatus == HIT)
				{
					stickStatus = READY;
					debugLog("Drum stick is now ready.");
				}
			}
		#endif
		// Play drum, if applicable
		if(playNow == 1){
			R_T();
			playNow = 0;
			if(stickStatus == READY){
				hitDrum(1);
				stickStatus = HIT;
				retractTime = getVirtualClock() + (COOLDOWN*64);
				debugLog("Played the drum.");
			}else{
				debugLog("Did not play because not ready.");
			}
		}
	}
	PROCESS_END();
}

void updateClock(uint32_t adjustment){
	uint32_t oldValue = getVirtualClock();
	uint32_t newValue = oldValue + adjustment;

	// if skipping forward, discard any skipped entries from FIFO queue 
	if(newValue > oldValue){
		uint32_t clk;
		int failure = peekFifo(&clk);
		while(!failure && (clk < newValue)){
			playNow = 1;    // if we skip an entry, play now to make up for it
			readFifo(&clk); // discard skipped entry
			failure = peekFifo(&clk); // peek next entry
		}
	}
	setVirtualClock(newValue); // update clock
}

// disable interrupts around virtual clock access
uint32_t getVirtualClock(){
	__bic_status_register(GIE);
	uint32_t ret = virtualClock;
	__bis_status_register(GIE);
	return ret;
}
void setVirtualClock(uint32_t value){
	__bic_status_register(GIE);
	virtualClock = value;
	__bis_status_register(GIE);
	return;
}

#pragma vector = TIMER1_A0_VECTOR
__interrupt void Timer1A0ISR(void)
{
	virtualClock++;
	if(virtualClock % 32 == 0){ G_T(); } // heart beat
}
