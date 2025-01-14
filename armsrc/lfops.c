//-----------------------------------------------------------------------------
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Miscellaneous routines for low frequency tag operations.
// Tags supported here so far are Texas Instruments (TI), HID, EM4x05, EM410x
// Also routines for raw mode reading/simulating of LF waveform
//-----------------------------------------------------------------------------

#include "proxmark3.h"
#include "apps.h"
#include "util.h"
#include "hitag2.h"
#include "crc16.h"
#include "string.h"
#include "lfdemod.h"
#include "lfsampling.h"
#include "protocols.h"
#include "usb_cdc.h" // for usb_poll_validate_length
#include "fpgaloader.h"

/**
 * Function to do a modulation and then get samples.
 * @param delay_off
 * @param period_0
 * @param period_1
 * @param command
 */
void ModThenAcquireRawAdcSamples125k(uint32_t delay_off, uint32_t period_0, uint32_t period_1, uint8_t *command)
{
	// start timer
	StartTicks();

	// use lf config settings
	sample_config *sc = getSamplingConfig();

	// Make sure the tag is reset
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	WaitMS(2500);

	// clear read buffer (after fpga bitstream loaded...)
	BigBuf_Clear_keep_EM();

	// power on
	LFSetupFPGAForADC(sc->divisor, 1);

	// And a little more time for the tag to fully power up
	WaitMS(2000);
	// if delay_off = 0 then just bitbang 1 = antenna on 0 = off for respective periods.
	bool bitbang = delay_off == 0;
	// now modulate the reader field

	if (bitbang) {
		// HACK it appears the loop and if statements take up about 7us so adjust waits accordingly...
		uint8_t hack_cnt = 7;
		if (period_0 < hack_cnt || period_1 < hack_cnt) {
			DbpString("Warning periods cannot be less than 7us in bit bang mode");
			FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
			LED_D_OFF();
			return;
		}

		// hack2 needed---  it appears to take about 8-16us to turn the antenna back on 
		// leading to ~ 1 to 2 125khz samples extra in every off period 
		// so we should test for last 0 before next 1 and reduce period_0 by this extra amount...
		// but is this time different for every antenna or other hw builds???  more testing needed

		// prime cmd_len to save time comparing strings while modulating
		int cmd_len = 0;
		while(command[cmd_len] != '\0' && command[cmd_len] != ' ')
			cmd_len++;

		int counter = 0;
		bool off = false;
		for (counter = 0; counter < cmd_len; counter++) {
			// if cmd = 0 then turn field off
			if (command[counter] == '0') {
				// if field already off leave alone (affects timing otherwise)
				if (off == false) {
					FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
					LED_D_OFF();
					off = true;
				}
				// note we appear to take about 7us to switch over (or run the if statements/loop...)
				WaitUS(period_0-hack_cnt);
			// else if cmd = 1 then turn field on
			} else {
				// if field already on leave alone (affects timing otherwise)
				if (off) {
					FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);
					LED_D_ON();
					off = false;
				}
				// note we appear to take about 7us to switch over (or run the if statements/loop...)
				WaitUS(period_1-hack_cnt);
			}
		}
	} else { // old mode of cmd read using delay as off period
		while(*command != '\0' && *command != ' ') {
			FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
			LED_D_OFF();
			WaitUS(delay_off);
			FpgaSendCommand(FPGA_CMD_SET_DIVISOR, sc->divisor);
			FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);
			LED_D_ON();
			if(*(command++) == '0') {
				WaitUS(period_0);
			} else {
				WaitUS(period_1);
			}
		}
		FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
		LED_D_OFF();
		WaitUS(delay_off);
		FpgaSendCommand(FPGA_CMD_SET_DIVISOR, sc->divisor);
	}

	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);

	// now do the read
	DoAcquisition_config(false, 0);

	// Turn off antenna
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	// tell client we are done
	cmd_send(CMD_ACK,0,0,0,0,0);
}

/* blank r/w tag data stream
...0000000000000000 01111111
1010101010101010101010101010101010101010101010101010101010101010
0011010010100001
01111111
101010101010101[0]000...

[5555fe852c5555555555555555fe0000]
*/
void ReadTItag(void)
{
	// some hardcoded initial params
	// when we read a TI tag we sample the zerocross line at 2Mhz
	// TI tags modulate a 1 as 16 cycles of 123.2Khz
	// TI tags modulate a 0 as 16 cycles of 134.2Khz
 #define FSAMPLE 2000000
 #define FREQLO 123200
 #define FREQHI 134200

	signed char *dest = (signed char *)BigBuf_get_addr();
	uint16_t n = BigBuf_max_traceLen();
	// 128 bit shift register [shift3:shift2:shift1:shift0]
	uint32_t shift3 = 0, shift2 = 0, shift1 = 0, shift0 = 0;

	int i, cycles=0, samples=0;
	// how many sample points fit in 16 cycles of each frequency
	uint32_t sampleslo = (FSAMPLE<<4)/FREQLO, sampleshi = (FSAMPLE<<4)/FREQHI;
	// when to tell if we're close enough to one freq or another
	uint32_t threshold = (sampleslo - sampleshi + 1)>>1;

	// TI tags charge at 134.2Khz
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
	FpgaSendCommand(FPGA_CMD_SET_DIVISOR, 88); //134.8Khz

	// Place FPGA in passthrough mode, in this mode the CROSS_LO line
	// connects to SSP_DIN and the SSP_DOUT logic level controls
	// whether we're modulating the antenna (high)
	// or listening to the antenna (low)
	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_PASSTHRU);

	// get TI tag data into the buffer
	AcquireTiType();

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

	for (i=0; i<n-1; i++) {
		// count cycles by looking for lo to hi zero crossings
		if ( (dest[i]<0) && (dest[i+1]>0) ) {
			cycles++;
			// after 16 cycles, measure the frequency
			if (cycles>15) {
				cycles=0;
				samples=i-samples; // number of samples in these 16 cycles

				// TI bits are coming to us lsb first so shift them
				// right through our 128 bit right shift register
				shift0 = (shift0>>1) | (shift1 << 31);
				shift1 = (shift1>>1) | (shift2 << 31);
				shift2 = (shift2>>1) | (shift3 << 31);
				shift3 >>= 1;

				// check if the cycles fall close to the number
				// expected for either the low or high frequency
				if ( (samples>(sampleslo-threshold)) && (samples<(sampleslo+threshold)) ) {
					// low frequency represents a 1
					shift3 |= (1<<31);
				} else if ( (samples>(sampleshi-threshold)) && (samples<(sampleshi+threshold)) ) {
					// high frequency represents a 0
				} else {
					// probably detected a gay waveform or noise
					// use this as gaydar or discard shift register and start again
					shift3 = shift2 = shift1 = shift0 = 0;
				}
				samples = i;

				// for each bit we receive, test if we've detected a valid tag

				// if we see 17 zeroes followed by 6 ones, we might have a tag
				// remember the bits are backwards
				if ( ((shift0 & 0x7fffff) == 0x7e0000) ) {
					// if start and end bytes match, we have a tag so break out of the loop
					if ( ((shift0>>16)&0xff) == ((shift3>>8)&0xff) ) {
						cycles = 0xF0B; //use this as a flag (ugly but whatever)
						break;
					}
				}
			}
		}
	}

	// if flag is set we have a tag
	if (cycles!=0xF0B) {
		DbpString("Info: No valid tag detected.");
	} else {
		// put 64 bit data into shift1 and shift0
		shift0 = (shift0>>24) | (shift1 << 8);
		shift1 = (shift1>>24) | (shift2 << 8);

		// align 16 bit crc into lower half of shift2
		shift2 = ((shift2>>24) | (shift3 << 8)) & 0x0ffff;

		// if r/w tag, check ident match
		if (shift3 & (1<<15) ) {
			DbpString("Info: TI tag is rewriteable");
			// only 15 bits compare, last bit of ident is not valid
			if (((shift3 >> 16) ^ shift0) & 0x7fff ) {
				DbpString("Error: Ident mismatch!");
			} else {
				DbpString("Info: TI tag ident is valid");
			}
		} else {
			DbpString("Info: TI tag is readonly");
		}

		// WARNING the order of the bytes in which we calc crc below needs checking
		// i'm 99% sure the crc algorithm is correct, but it may need to eat the
		// bytes in reverse or something
		// calculate CRC
		uint32_t crc=0;

		crc = update_crc16(crc, (shift0)&0xff);
		crc = update_crc16(crc, (shift0>>8)&0xff);
		crc = update_crc16(crc, (shift0>>16)&0xff);
		crc = update_crc16(crc, (shift0>>24)&0xff);
		crc = update_crc16(crc, (shift1)&0xff);
		crc = update_crc16(crc, (shift1>>8)&0xff);
		crc = update_crc16(crc, (shift1>>16)&0xff);
		crc = update_crc16(crc, (shift1>>24)&0xff);

		Dbprintf("Info: Tag data: %x%08x, crc=%x",
				 (unsigned int)shift1, (unsigned int)shift0, (unsigned int)shift2 & 0xFFFF);
		if (crc != (shift2&0xffff)) {
			Dbprintf("Error: CRC mismatch, expected %x", (unsigned int)crc);
		} else {
			DbpString("Info: CRC is good");
		}
	}
}

void WriteTIbyte(uint8_t b)
{
	int i = 0;

	// modulate 8 bits out to the antenna
	for (i=0; i<8; i++)
	{
		if (b&(1<<i)) {
			// stop modulating antenna
			LOW(GPIO_SSC_DOUT);
			SpinDelayUs(1000);
			// modulate antenna
			HIGH(GPIO_SSC_DOUT);
			SpinDelayUs(1000);
		} else {
			// stop modulating antenna
			LOW(GPIO_SSC_DOUT);
			SpinDelayUs(300);
			// modulate antenna
			HIGH(GPIO_SSC_DOUT);
			SpinDelayUs(1700);
		}
	}
}

void AcquireTiType(void)
{
	int i, j, n;
	// tag transmission is <20ms, sampling at 2M gives us 40K samples max
	// each sample is 1 bit stuffed into a uint32_t so we need 1250 uint32_t
 #define TIBUFLEN 1250

	// clear buffer
	uint32_t *BigBuf = (uint32_t *)BigBuf_get_addr();
	BigBuf_Clear_ext(false);

	// Set up the synchronous serial port
	AT91C_BASE_PIOA->PIO_PDR = GPIO_SSC_DIN;
	AT91C_BASE_PIOA->PIO_ASR = GPIO_SSC_DIN;

	// steal this pin from the SSP and use it to control the modulation
	AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT;
	AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;

	AT91C_BASE_SSC->SSC_CR = AT91C_SSC_SWRST;
	AT91C_BASE_SSC->SSC_CR = AT91C_SSC_RXEN | AT91C_SSC_TXEN;

	// Sample at 2 Mbit/s, so TI tags are 16.2 vs. 14.9 clocks long
	// 48/2 = 24 MHz clock must be divided by 12
	AT91C_BASE_SSC->SSC_CMR = 12;

	AT91C_BASE_SSC->SSC_RCMR = SSC_CLOCK_MODE_SELECT(0);
	AT91C_BASE_SSC->SSC_RFMR = SSC_FRAME_MODE_BITS_IN_WORD(32) | AT91C_SSC_MSBF;
	AT91C_BASE_SSC->SSC_TCMR = 0;
	AT91C_BASE_SSC->SSC_TFMR = 0;

	LED_D_ON();

	// modulate antenna
	HIGH(GPIO_SSC_DOUT);

	// Charge TI tag for 50ms.
	SpinDelay(50);

	// stop modulating antenna and listen
	LOW(GPIO_SSC_DOUT);

	LED_D_OFF();

	i = 0;
	for(;;) {
		if(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY) {
			BigBuf[i] = AT91C_BASE_SSC->SSC_RHR;	// store 32 bit values in buffer
			i++; if(i >= TIBUFLEN) break;
		}
		WDT_HIT();
	}

	// return stolen pin to SSP
	AT91C_BASE_PIOA->PIO_PDR = GPIO_SSC_DOUT;
	AT91C_BASE_PIOA->PIO_ASR = GPIO_SSC_DIN | GPIO_SSC_DOUT;

	char *dest = (char *)BigBuf_get_addr();
	n = TIBUFLEN*32;
	// unpack buffer
	for (i=TIBUFLEN-1; i>=0; i--) {
		for (j=0; j<32; j++) {
			if(BigBuf[i] & (1 << j)) {
				dest[--n] = 1;
			} else {
				dest[--n] = -1;
			}
		}
	}
}

// arguments: 64bit data split into 32bit idhi:idlo and optional 16bit crc
// if crc provided, it will be written with the data verbatim (even if bogus)
// if not provided a valid crc will be computed from the data and written.
void WriteTItag(uint32_t idhi, uint32_t idlo, uint16_t crc)
{
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
	if(crc == 0) {
		crc = update_crc16(crc, (idlo)&0xff);
		crc = update_crc16(crc, (idlo>>8)&0xff);
		crc = update_crc16(crc, (idlo>>16)&0xff);
		crc = update_crc16(crc, (idlo>>24)&0xff);
		crc = update_crc16(crc, (idhi)&0xff);
		crc = update_crc16(crc, (idhi>>8)&0xff);
		crc = update_crc16(crc, (idhi>>16)&0xff);
		crc = update_crc16(crc, (idhi>>24)&0xff);
	}
	Dbprintf("Writing to tag: %x%08x, crc=%x",
			(unsigned int) idhi, (unsigned int) idlo, crc);

	// TI tags charge at 134.2Khz
	FpgaSendCommand(FPGA_CMD_SET_DIVISOR, 88); //134.8Khz
	// Place FPGA in passthrough mode, in this mode the CROSS_LO line
	// connects to SSP_DIN and the SSP_DOUT logic level controls
	// whether we're modulating the antenna (high)
	// or listening to the antenna (low)
	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_PASSTHRU);
	LED_A_ON();

	// steal this pin from the SSP and use it to control the modulation
	AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT;
	AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;

	// writing algorithm:
	// a high bit consists of a field off for 1ms and field on for 1ms
	// a low bit consists of a field off for 0.3ms and field on for 1.7ms
	// initiate a charge time of 50ms (field on) then immediately start writing bits
	// start by writing 0xBB (keyword) and 0xEB (password)
	// then write 80 bits of data (or 64 bit data + 16 bit crc if you prefer)
	// finally end with 0x0300 (write frame)
	// all data is sent lsb firts
	// finish with 15ms programming time

	// modulate antenna
	HIGH(GPIO_SSC_DOUT);
	SpinDelay(50);	// charge time

	WriteTIbyte(0xbb); // keyword
	WriteTIbyte(0xeb); // password
	WriteTIbyte( (idlo    )&0xff );
	WriteTIbyte( (idlo>>8 )&0xff );
	WriteTIbyte( (idlo>>16)&0xff );
	WriteTIbyte( (idlo>>24)&0xff );
	WriteTIbyte( (idhi    )&0xff );
	WriteTIbyte( (idhi>>8 )&0xff );
	WriteTIbyte( (idhi>>16)&0xff );
	WriteTIbyte( (idhi>>24)&0xff ); // data hi to lo
	WriteTIbyte( (crc     )&0xff ); // crc lo
	WriteTIbyte( (crc>>8  )&0xff ); // crc hi
	WriteTIbyte(0x00); // write frame lo
	WriteTIbyte(0x03); // write frame hi
	HIGH(GPIO_SSC_DOUT);
	SpinDelay(50);	// programming time

	LED_A_OFF();

	// get TI tag data into the buffer
	AcquireTiType();

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	DbpString("Now use `lf ti read` to check");
}

void SimulateTagLowFrequency(int period, int gap, int ledcontrol)
{
	int i;
	uint8_t *tab = BigBuf_get_addr();

	//note FpgaDownloadAndGo destroys the bigbuf so be sure this is called before now...
	//FpgaDownloadAndGo(FPGA_BITSTREAM_LF);  
	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT);

	AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT | GPIO_SSC_CLK;

	AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;
	AT91C_BASE_PIOA->PIO_ODR = GPIO_SSC_CLK;

 #define SHORT_COIL()   LOW(GPIO_SSC_DOUT)
 #define OPEN_COIL()    HIGH(GPIO_SSC_DOUT)

	i = 0;
	for(;;) {
		//wait until SSC_CLK goes HIGH
		int ii = 0;
		while(!(AT91C_BASE_PIOA->PIO_PDSR & GPIO_SSC_CLK)) {
			//only check every 1000th time (usb_poll_validate_length on some systems was too slow)
			if ( ii == 1000 ) {
				if (BUTTON_PRESS() || usb_poll_validate_length() ) {
					FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
					DbpString("Stopped");
					return;
				}
				ii=0;
			}
			WDT_HIT();
			ii++;
		}
		if (ledcontrol)
			LED_D_ON();

		if(tab[i])
			OPEN_COIL();
		else
			SHORT_COIL();

		if (ledcontrol)
			LED_D_OFF();
		ii=0;
		//wait until SSC_CLK goes LOW
		while(AT91C_BASE_PIOA->PIO_PDSR & GPIO_SSC_CLK) {
			//only check every 1000th time (usb_poll_validate_length on some systems was too slow)
			if ( ii == 1000 ) { 
				if (BUTTON_PRESS() || usb_poll_validate_length() ) {
					FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
					DbpString("Stopped");
					return;
				}
				ii=0;
			}
			WDT_HIT();
			ii++;
		}

		i++;
		if(i == period) {

			i = 0;
			if (gap) {
				SHORT_COIL();
				SpinDelayUs(gap);
			}
		}

	}
}

#define DEBUG_FRAME_CONTENTS 1
void SimulateTagLowFrequencyBidir(int divisor, int t0)
{
}

// compose fc/8 fc/10 waveform (FSK2)
static void fc(int c, int *n)
{
	uint8_t *dest = BigBuf_get_addr();
	int idx;

	// for when we want an fc8 pattern every 4 logical bits
	if(c==0) {
		dest[((*n)++)]=1;
		dest[((*n)++)]=1;
		dest[((*n)++)]=1;
		dest[((*n)++)]=1;
		dest[((*n)++)]=0;
		dest[((*n)++)]=0;
		dest[((*n)++)]=0;
		dest[((*n)++)]=0;
	}

	//	an fc/8  encoded bit is a bit pattern of  11110000  x6 = 48 samples
	if(c==8) {
		for (idx=0; idx<6; idx++) {
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
		}
	}

	//	an fc/10 encoded bit is a bit pattern of 1111100000 x5 = 50 samples
	if(c==10) {
		for (idx=0; idx<5; idx++) {
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=1;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
			dest[((*n)++)]=0;
		}
	}
}
// compose fc/X fc/Y waveform (FSKx)
static void fcAll(uint8_t fc, int *n, uint8_t clock, uint16_t *modCnt) 
{
	uint8_t *dest = BigBuf_get_addr();
	uint8_t halfFC = fc/2;
	uint8_t wavesPerClock = clock/fc;
	uint8_t mod = clock % fc;    //modifier
	uint8_t modAdj = fc/mod;     //how often to apply modifier
	bool modAdjOk = !(fc % mod); //if (fc % mod==0) modAdjOk=true;
	// loop through clock - step field clock
	for (uint8_t idx=0; idx < wavesPerClock; idx++){
		// put 1/2 FC length 1's and 1/2 0's per field clock wave (to create the wave)
		memset(dest+(*n), 0, fc-halfFC);  //in case of odd number use extra here
		memset(dest+(*n)+(fc-halfFC), 1, halfFC);
		*n += fc;
	}
	if (mod>0) (*modCnt)++;
	if ((mod>0) && modAdjOk){  //fsk2 
		if ((*modCnt % modAdj) == 0){ //if 4th 8 length wave in a rf/50 add extra 8 length wave
			memset(dest+(*n), 0, fc-halfFC);
			memset(dest+(*n)+(fc-halfFC), 1, halfFC);
			*n += fc;
		}
	}
	if (mod>0 && !modAdjOk){  //fsk1
		memset(dest+(*n), 0, mod-(mod/2));
		memset(dest+(*n)+(mod-(mod/2)), 1, mod/2);
		*n += mod;
	}
}

// prepare a waveform pattern in the buffer based on the ID given then
// simulate a HID tag until the button is pressed
void CmdHIDsimTAG(int hi2, int hi, int lo, int ledcontrol)
{
	int n=0, i=0;
	/*
	 HID tag bitstream format
	 The tag contains a 44bit unique code. This is sent out MSB first in sets of 4 bits
	 A 1 bit is represented as 6 fc8 and 5 fc10 patterns
	 A 0 bit is represented as 5 fc10 and 6 fc8 patterns
	 A fc8 is inserted before every 4 bits
	 A special start of frame pattern is used consisting a0b0 where a and b are neither 0
	 nor 1 bits, they are special patterns (a = set of 12 fc8 and b = set of 10 fc10)
	*/

	if (hi2>0x0FFFFFFF) {
		DbpString("Tags can only have 44 or 84 bits. - USE lf simfsk for larger tags");
		return;
	}
	// set LF so we don't kill the bigbuf we are setting with simulation data.
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);

	fc(0,&n);
	// special start of frame marker containing invalid bit sequences
	fc(8,  &n);	fc(8,  &n); // invalid
	fc(8,  &n);	fc(10, &n); // logical 0
	fc(10, &n);	fc(10, &n); // invalid
	fc(8,  &n);	fc(10, &n); // logical 0

	WDT_HIT();
	if (hi2 > 0 || hi > 0xFFF){
		// manchester encode bits 91 to 64 (91-84 are part of the header)
		for (i=27; i>=0; i--) {
			if ((i%4)==3) fc(0,&n);
			if ((hi2>>i)&1) {
				fc(10, &n); fc(8,  &n);		// low-high transition
			} else {
				fc(8,  &n); fc(10, &n);		// high-low transition
			}
		}
		WDT_HIT();
		// manchester encode bits 63 to 32
		for (i=31; i>=0; i--) {
			if ((i%4)==3) fc(0,&n);
			if ((hi>>i)&1) {
				fc(10, &n); fc(8,  &n);		// low-high transition
			} else {
				fc(8,  &n); fc(10, &n);		// high-low transition
			}
		}
	} else {
		// manchester encode bits 43 to 32
		for (i=11; i>=0; i--) {
			if ((i%4)==3) fc(0,&n);
			if ((hi>>i)&1) {
				fc(10, &n); fc(8,  &n);		// low-high transition
			} else {
				fc(8,  &n); fc(10, &n);		// high-low transition
			}
		}
	}

	WDT_HIT();
	// manchester encode bits 31 to 0
	for (i=31; i>=0; i--) {
		if ((i%4)==3) fc(0,&n);
		if ((lo>>i)&1) {
			fc(10, &n); fc(8,  &n);		// low-high transition
		} else {
			fc(8,  &n); fc(10, &n);		// high-low transition
		}
	}

	if (ledcontrol)
		LED_A_ON();
	SimulateTagLowFrequency(n, 0, ledcontrol);

	if (ledcontrol)
		LED_A_OFF();
}

// prepare a waveform pattern in the buffer based on the ID given then
// simulate a FSK tag until the button is pressed
// arg1 contains fcHigh and fcLow, arg2 contains invert and clock
void CmdFSKsimTAG(uint16_t arg1, uint16_t arg2, size_t size, uint8_t *BitStream)
{
	int ledcontrol=1;
	int n=0, i=0;
	uint8_t fcHigh = arg1 >> 8;
	uint8_t fcLow = arg1 & 0xFF;
	uint16_t modCnt = 0;
	uint8_t clk = arg2 & 0xFF;
	uint8_t invert = (arg2 >> 8) & 1;

	// set LF so we don't kill the bigbuf we are setting with simulation data.
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);

	for (i=0; i<size; i++){
		if (BitStream[i] == invert){
			fcAll(fcLow, &n, clk, &modCnt);
		} else {
			fcAll(fcHigh, &n, clk, &modCnt);
		}
	}
	Dbprintf("Simulating with fcHigh: %d, fcLow: %d, clk: %d, invert: %d, n: %d",fcHigh, fcLow, clk, invert, n);
	/*Dbprintf("DEBUG: First 32:");
	uint8_t *dest = BigBuf_get_addr();
	i=0;
	Dbprintf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", dest[i],dest[i+1],dest[i+2],dest[i+3],dest[i+4],dest[i+5],dest[i+6],dest[i+7],dest[i+8],dest[i+9],dest[i+10],dest[i+11],dest[i+12],dest[i+13],dest[i+14],dest[i+15]);
	i+=16;
	Dbprintf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", dest[i],dest[i+1],dest[i+2],dest[i+3],dest[i+4],dest[i+5],dest[i+6],dest[i+7],dest[i+8],dest[i+9],dest[i+10],dest[i+11],dest[i+12],dest[i+13],dest[i+14],dest[i+15]);
	*/
	if (ledcontrol)
		LED_A_ON();

	SimulateTagLowFrequency(n, 0, ledcontrol);

	if (ledcontrol)
		LED_A_OFF();
}

// compose ask waveform for one bit(ASK)
static void askSimBit(uint8_t c, int *n, uint8_t clock, uint8_t manchester)
{
	uint8_t *dest = BigBuf_get_addr();
	uint8_t halfClk = clock/2;
	// c = current bit 1 or 0
	if (manchester==1){
		memset(dest+(*n), c, halfClk);
		memset(dest+(*n) + halfClk, c^1, halfClk);
	} else {
		memset(dest+(*n), c, clock);
	}
	*n += clock;
}

static void biphaseSimBit(uint8_t c, int *n, uint8_t clock, uint8_t *phase)
{
	uint8_t *dest = BigBuf_get_addr();
	uint8_t halfClk = clock/2;
	if (c){
		memset(dest+(*n), c ^ 1 ^ *phase, halfClk);
		memset(dest+(*n) + halfClk, c ^ *phase, halfClk);
	} else {
		memset(dest+(*n), c ^ *phase, clock);
		*phase ^= 1;
	}
	*n += clock;
}

static void stAskSimBit(int *n, uint8_t clock) {
	uint8_t *dest = BigBuf_get_addr();
	uint8_t halfClk = clock/2;
	//ST = .5 high .5 low 1.5 high .5 low 1 high	
	memset(dest+(*n), 1, halfClk);
	memset(dest+(*n) + halfClk, 0, halfClk);
	memset(dest+(*n) + clock, 1, clock + halfClk);
	memset(dest+(*n) + clock*2 + halfClk, 0, halfClk);
	memset(dest+(*n) + clock*3, 1, clock);
	*n += clock*4;
}

// args clock, ask/man or askraw, invert, transmission separator
void CmdASKsimTag(uint16_t arg1, uint16_t arg2, size_t size, uint8_t *BitStream)
{
	int ledcontrol = 1;
	int n=0, i=0;
	uint8_t clk = (arg1 >> 8) & 0xFF;
	uint8_t encoding = arg1 & 0xFF;
	uint8_t separator = arg2 & 1;
	uint8_t invert = (arg2 >> 8) & 1;

	// set LF so we don't kill the bigbuf we are setting with simulation data.
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);

	if (encoding==2){  //biphase
		uint8_t phase=0;
		for (i=0; i<size; i++){
			biphaseSimBit(BitStream[i]^invert, &n, clk, &phase);
		}
		if (phase==1) { //run a second set inverted to keep phase in check
			for (i=0; i<size; i++){
				biphaseSimBit(BitStream[i]^invert, &n, clk, &phase);
			}
		}
	} else {  // ask/manchester || ask/raw
		for (i=0; i<size; i++){
			askSimBit(BitStream[i]^invert, &n, clk, encoding);
		}
		if (encoding==0 && BitStream[0]==BitStream[size-1]){ //run a second set inverted (for ask/raw || biphase phase)
			for (i=0; i<size; i++){
				askSimBit(BitStream[i]^invert^1, &n, clk, encoding);
			}
		}
	}
	if (separator==1 && encoding == 1)
		stAskSimBit(&n, clk);
	else if (separator==1)
		Dbprintf("sorry but separator option not yet available");

	Dbprintf("Simulating with clk: %d, invert: %d, encoding: %d, separator: %d, n: %d",clk, invert, encoding, separator, n);
	//DEBUG
	//Dbprintf("First 32:");
	//uint8_t *dest = BigBuf_get_addr();
	//i=0;
	//Dbprintf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", dest[i],dest[i+1],dest[i+2],dest[i+3],dest[i+4],dest[i+5],dest[i+6],dest[i+7],dest[i+8],dest[i+9],dest[i+10],dest[i+11],dest[i+12],dest[i+13],dest[i+14],dest[i+15]);
	//i+=16;
	//Dbprintf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", dest[i],dest[i+1],dest[i+2],dest[i+3],dest[i+4],dest[i+5],dest[i+6],dest[i+7],dest[i+8],dest[i+9],dest[i+10],dest[i+11],dest[i+12],dest[i+13],dest[i+14],dest[i+15]);
	
	if (ledcontrol) LED_A_ON();
	SimulateTagLowFrequency(n, 0, ledcontrol);
	if (ledcontrol) LED_A_OFF();
}

//carrier can be 2,4 or 8
static void pskSimBit(uint8_t waveLen, int *n, uint8_t clk, uint8_t *curPhase, bool phaseChg)
{
	uint8_t *dest = BigBuf_get_addr();
	uint8_t halfWave = waveLen/2;
	//uint8_t idx;
	int i = 0;
	if (phaseChg){
		// write phase change
		memset(dest+(*n), *curPhase^1, halfWave);
		memset(dest+(*n) + halfWave, *curPhase, halfWave);
		*n += waveLen;
		*curPhase ^= 1;
		i += waveLen;
	}
	//write each normal clock wave for the clock duration
	for (; i < clk; i+=waveLen){
		memset(dest+(*n), *curPhase, halfWave);
		memset(dest+(*n) + halfWave, *curPhase^1, halfWave);
		*n += waveLen;
	}
}

// args clock, carrier, invert,
void CmdPSKsimTag(uint16_t arg1, uint16_t arg2, size_t size, uint8_t *BitStream)
{
	int ledcontrol=1;
	int n=0, i=0;
	uint8_t clk = arg1 >> 8;
	uint8_t carrier = arg1 & 0xFF;
	uint8_t invert = arg2 & 0xFF;
	uint8_t curPhase = 0;
	// set LF so we don't kill the bigbuf we are setting with simulation data.
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);

	for (i=0; i<size; i++){
		if (BitStream[i] == curPhase){
			pskSimBit(carrier, &n, clk, &curPhase, false);
		} else {
			pskSimBit(carrier, &n, clk, &curPhase, true);
		}
	}
	Dbprintf("Simulating with Carrier: %d, clk: %d, invert: %d, n: %d",carrier, clk, invert, n);
	//Dbprintf("DEBUG: First 32:");
	//uint8_t *dest = BigBuf_get_addr();
	//i=0;
	//Dbprintf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", dest[i],dest[i+1],dest[i+2],dest[i+3],dest[i+4],dest[i+5],dest[i+6],dest[i+7],dest[i+8],dest[i+9],dest[i+10],dest[i+11],dest[i+12],dest[i+13],dest[i+14],dest[i+15]);
	//i+=16;
	//Dbprintf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d", dest[i],dest[i+1],dest[i+2],dest[i+3],dest[i+4],dest[i+5],dest[i+6],dest[i+7],dest[i+8],dest[i+9],dest[i+10],dest[i+11],dest[i+12],dest[i+13],dest[i+14],dest[i+15]);
		   
	if (ledcontrol) LED_A_ON();
	SimulateTagLowFrequency(n, 0, ledcontrol);
	if (ledcontrol) LED_A_OFF();
}

// loop to get raw HID waveform then FSK demodulate the TAG ID from it
void CmdHIDdemodFSK(int findone, int *high2, int *high, int *low, int ledcontrol)
{
	uint8_t *dest = BigBuf_get_addr();
	//const size_t sizeOfBigBuff = BigBuf_max_traceLen();
	size_t size; 
	uint32_t hi2=0, hi=0, lo=0;
	int idx=0;
	int dummyIdx = 0;
	// Configure to go in 125Khz listen mode
	LFSetupFPGAForADC(95, true);

	//clear read buffer
	BigBuf_Clear_keep_EM();

	while(!BUTTON_PRESS() && !usb_poll_validate_length()) {
		WDT_HIT();
		if (ledcontrol) LED_A_ON();

		DoAcquisition_default(-1,true);
		// FSK demodulator
		//size = sizeOfBigBuff;  //variable size will change after demod so re initialize it before use
		size = 50*128*2; //big enough to catch 2 sequences of largest format
		idx = HIDdemodFSK(dest, &size, &hi2, &hi, &lo, &dummyIdx);
		
		if (idx>0 && lo>0 && (size==96 || size==192)){
			uint8_t bitlen = 0;
			uint32_t fc = 0;
			uint32_t cardnum = 0;
			bool decoded = false;

			// go over previously decoded manchester data and decode into usable tag ID
			if ((hi2 & 0x000FFFF) != 0){ //extra large HID tags  88/192 bits
				uint32_t bp = hi2 & 0x000FFFFF;
				bitlen = 63;
				while (bp > 0) {
					bp = bp >> 1;
					bitlen++;
				}
			} else if ((hi >> 6) > 0) {
				uint32_t bp = hi;
				bitlen = 31;
				while (bp > 0) {
					bp = bp >> 1;
					bitlen++;
				}
			} else if (((hi >> 5) & 1) == 0) {
				bitlen = 37;
			} else if ((hi & 0x0000001F) > 0 ) {
				uint32_t bp = (hi & 0x0000001F);
				bitlen = 31;
				while (bp > 0) {
					bp = bp >> 1;
					bitlen++;
				}
			} else {
				uint32_t bp = lo;
				bitlen = 0;
				while (bp > 0) {
					bp = bp >> 1;
					bitlen++;
				}
			}
			switch (bitlen){
				case 26:
					cardnum = (lo>>1)&0xFFFF;
					fc = (lo>>17)&0xFF;
					decoded = true;
					break;
				case 35:
					cardnum = (lo>>1)&0xFFFFF;
					fc = ((hi&1)<<11)|(lo>>21);
					decoded = true;
					break;
			}
				
			if (hi2 != 0) //extra large HID tags  88/192 bits
				Dbprintf("TAG ID: %x%08x%08x (%d)",
					(unsigned int) hi2, (unsigned int) hi, (unsigned int) lo, (unsigned int) (lo>>1) & 0xFFFF);
			else 
				Dbprintf("TAG ID: %x%08x (%d)",
					(unsigned int) hi, (unsigned int) lo, (unsigned int) (lo>>1) & 0xFFFF);
			
			if (decoded)
				Dbprintf("Format Len: %dbits - FC: %d - Card: %d",
					(unsigned int) bitlen, (unsigned int) fc, (unsigned int) cardnum);

			if (findone){
				if (ledcontrol)	LED_A_OFF();
				*high2 = hi2;
				*high = hi;
				*low = lo;
				break;
			}
			// reset
		}
		hi2 = hi = lo = idx = 0;
		WDT_HIT();
	}

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	DbpString("Stopped");
	if (ledcontrol) LED_A_OFF();
}

// loop to get raw HID waveform then FSK demodulate the TAG ID from it
void CmdAWIDdemodFSK(int findone, int *high, int *low, int ledcontrol)
{
	uint8_t *dest = BigBuf_get_addr();
	size_t size; 
	int idx=0, dummyIdx=0;
	//clear read buffer
	BigBuf_Clear_keep_EM();
	// Configure to go in 125Khz listen mode
	LFSetupFPGAForADC(95, true);

	while(!BUTTON_PRESS() && !usb_poll_validate_length()) {

		WDT_HIT();
		if (ledcontrol) LED_A_ON();

		DoAcquisition_default(-1,true);
		// FSK demodulator
		size = 50*128*2; //big enough to catch 2 sequences of largest format
		idx = AWIDdemodFSK(dest, &size, &dummyIdx);
		
		if (idx<=0 || size!=96) continue;
		// Index map
		// 0            10            20            30              40            50              60
		// |            |             |             |               |             |               |
		// 01234567 890 1 234 5 678 9 012 3 456 7 890 1 234 5 678 9 012 3 456 7 890 1 234 5 678 9 012 3 - to 96
		// -----------------------------------------------------------------------------
		// 00000001 000 1 110 1 101 1 011 1 101 1 010 0 000 1 000 1 010 0 001 0 110 1 100 0 000 1 000 1
		// premable bbb o bbb o bbw o fff o fff o ffc o ccc o ccc o ccc o ccc o ccc o wxx o xxx o xxx o - to 96
		//          |---26 bit---|    |-----117----||-------------142-------------|
		// b = format bit len, o = odd parity of last 3 bits
		// f = facility code, c = card number
		// w = wiegand parity
		// (26 bit format shown)

		//get raw ID before removing parities
		uint32_t rawLo = bytebits_to_byte(dest+idx+64,32);
		uint32_t rawHi = bytebits_to_byte(dest+idx+32,32);
		uint32_t rawHi2 = bytebits_to_byte(dest+idx,32);

		size = removeParity(dest, idx+8, 4, 1, 88);
		if (size != 66) continue;
		// ok valid card found!

		// Index map
		// 0           10         20        30          40        50        60
		// |           |          |         |           |         |         |
		// 01234567 8 90123456 7890123456789012 3 456789012345678901234567890123456
		// -----------------------------------------------------------------------------
		// 00011010 1 01110101 0000000010001110 1 000000000000000000000000000000000
		// bbbbbbbb w ffffffff cccccccccccccccc w xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
		// |26 bit|   |-117--| |-----142------|
		// b = format bit len, o = odd parity of last 3 bits
		// f = facility code, c = card number
		// w = wiegand parity
		// (26 bit format shown)

		uint32_t fc = 0;
		uint32_t cardnum = 0;
		uint32_t code1 = 0;
		uint32_t code2 = 0;
		uint8_t fmtLen = bytebits_to_byte(dest,8);
		if (fmtLen==26){
			fc = bytebits_to_byte(dest+9, 8);
			cardnum = bytebits_to_byte(dest+17, 16);
			code1 = bytebits_to_byte(dest+8,fmtLen);
			Dbprintf("AWID Found - BitLength: %d, FC: %d, Card: %d - Wiegand: %x, Raw: %08x%08x%08x", fmtLen, fc, cardnum, code1, rawHi2, rawHi, rawLo);
		} else {
			cardnum = bytebits_to_byte(dest+8+(fmtLen-17), 16);
			if (fmtLen>32){
				code1 = bytebits_to_byte(dest+8,fmtLen-32);
				code2 = bytebits_to_byte(dest+8+(fmtLen-32),32);
				Dbprintf("AWID Found - BitLength: %d -unknown BitLength- (%d) - Wiegand: %x%08x, Raw: %08x%08x%08x", fmtLen, cardnum, code1, code2, rawHi2, rawHi, rawLo);
			} else{
				code1 = bytebits_to_byte(dest+8,fmtLen);
				Dbprintf("AWID Found - BitLength: %d -unknown BitLength- (%d) - Wiegand: %x, Raw: %08x%08x%08x", fmtLen, cardnum, code1, rawHi2, rawHi, rawLo);
			}
		}
		if (findone){
			if (ledcontrol)	LED_A_OFF();
			break;
		}
		// reset
		idx = 0;
		WDT_HIT();
	}
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	DbpString("Stopped");
	if (ledcontrol) LED_A_OFF();
}

void CmdEM410xdemod(int findone, int *high, int *low, int ledcontrol)
{
	uint8_t *dest = BigBuf_get_addr();

	size_t size=0, idx=0;
	int clk=0, invert=0, errCnt=0, maxErr=20;
	uint32_t hi=0;
	uint64_t lo=0;
	//clear read buffer
	BigBuf_Clear_keep_EM();
	// Configure to go in 125Khz listen mode
	LFSetupFPGAForADC(95, true);

	while(!BUTTON_PRESS() && !usb_poll_validate_length()) {

		WDT_HIT();
		if (ledcontrol) LED_A_ON();

		DoAcquisition_default(-1,true);
		size  = BigBuf_max_traceLen();
		//askdemod and manchester decode
		if (size > 16385) size = 16385; //big enough to catch 2 sequences of largest format
		errCnt = askdemod(dest, &size, &clk, &invert, maxErr, 0, 1);
		WDT_HIT();

		if (errCnt<0) continue;
	
		errCnt = Em410xDecode(dest, &size, &idx, &hi, &lo);
		if (errCnt){
			if (size>64){
				Dbprintf("EM XL TAG ID: %06x%08x%08x - (%05d_%03d_%08d)",
				  hi,
				  (uint32_t)(lo>>32),
				  (uint32_t)lo,
				  (uint32_t)(lo&0xFFFF),
				  (uint32_t)((lo>>16LL) & 0xFF),
				  (uint32_t)(lo & 0xFFFFFF));
			} else {
				Dbprintf("EM TAG ID: %02x%08x - (%05d_%03d_%08d)",
				  (uint32_t)(lo>>32),
				  (uint32_t)lo,
				  (uint32_t)(lo&0xFFFF),
				  (uint32_t)((lo>>16LL) & 0xFF),
				  (uint32_t)(lo & 0xFFFFFF));
			}

			if (findone){
				if (ledcontrol) LED_A_OFF();
				*high=lo>>32;
				*low=lo & 0xFFFFFFFF;
				break;
			}
		}
		WDT_HIT();
		hi = lo = size = idx = 0;
		clk = invert = errCnt = 0;
	}
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	DbpString("Stopped");
	if (ledcontrol) LED_A_OFF();
}

void CmdIOdemodFSK(int findone, int *high, int *low, int ledcontrol)
{
	uint8_t *dest = BigBuf_get_addr();
	int idx=0;
	uint32_t code=0, code2=0;
	uint8_t version=0;
	uint8_t facilitycode=0;
	uint16_t number=0;
	int dummyIdx=0;
	//clear read buffer
	BigBuf_Clear_keep_EM();
	// Configure to go in 125Khz listen mode
	LFSetupFPGAForADC(95, true);

	while(!BUTTON_PRESS() && !usb_poll_validate_length()) {
		WDT_HIT();
		if (ledcontrol) LED_A_ON();
		DoAcquisition_default(-1,true);
		//fskdemod and get start index
		WDT_HIT();
		idx = IOdemodFSK(dest, BigBuf_max_traceLen(), &dummyIdx);
		if (idx<0) continue;
		//valid tag found

		//Index map
		//0           10          20          30          40          50          60
		//|           |           |           |           |           |           |
		//01234567 8 90123456 7 89012345 6 78901234 5 67890123 4 56789012 3 45678901 23
		//-----------------------------------------------------------------------------
		//00000000 0 11110000 1 facility 1 version* 1 code*one 1 code*two 1 ???????? 11
		//
		//XSF(version)facility:codeone+codetwo
		//Handle the data
		if(findone){ //only print binary if we are doing one
			Dbprintf("%d%d%d%d%d%d%d%d %d",dest[idx],   dest[idx+1],   dest[idx+2],dest[idx+3],dest[idx+4],dest[idx+5],dest[idx+6],dest[idx+7],dest[idx+8]);
			Dbprintf("%d%d%d%d%d%d%d%d %d",dest[idx+9], dest[idx+10],dest[idx+11],dest[idx+12],dest[idx+13],dest[idx+14],dest[idx+15],dest[idx+16],dest[idx+17]);
			Dbprintf("%d%d%d%d%d%d%d%d %d",dest[idx+18],dest[idx+19],dest[idx+20],dest[idx+21],dest[idx+22],dest[idx+23],dest[idx+24],dest[idx+25],dest[idx+26]);
			Dbprintf("%d%d%d%d%d%d%d%d %d",dest[idx+27],dest[idx+28],dest[idx+29],dest[idx+30],dest[idx+31],dest[idx+32],dest[idx+33],dest[idx+34],dest[idx+35]);
			Dbprintf("%d%d%d%d%d%d%d%d %d",dest[idx+36],dest[idx+37],dest[idx+38],dest[idx+39],dest[idx+40],dest[idx+41],dest[idx+42],dest[idx+43],dest[idx+44]);
			Dbprintf("%d%d%d%d%d%d%d%d %d",dest[idx+45],dest[idx+46],dest[idx+47],dest[idx+48],dest[idx+49],dest[idx+50],dest[idx+51],dest[idx+52],dest[idx+53]);
			Dbprintf("%d%d%d%d%d%d%d%d %d%d",dest[idx+54],dest[idx+55],dest[idx+56],dest[idx+57],dest[idx+58],dest[idx+59],dest[idx+60],dest[idx+61],dest[idx+62],dest[idx+63]);
		}
		code = bytebits_to_byte(dest+idx,32);
		code2 = bytebits_to_byte(dest+idx+32,32);
		version = bytebits_to_byte(dest+idx+27,8); //14,4
		facilitycode = bytebits_to_byte(dest+idx+18,8);
		number = (bytebits_to_byte(dest+idx+36,8)<<8)|(bytebits_to_byte(dest+idx+45,8)); //36,9

		Dbprintf("XSF(%02d)%02x:%05d (%08x%08x)",version,facilitycode,number,code,code2);
		// if we're only looking for one tag
		if (findone){
			if (ledcontrol)	LED_A_OFF();
			//LED_A_OFF();
			*high=code;
			*low=code2;
			break;
		}
		code=code2=0;
		version=facilitycode=0;
		number=0;
		idx=0;

		WDT_HIT();
	}
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	DbpString("Stopped");
	if (ledcontrol) LED_A_OFF();
}

/*------------------------------
 * T5555/T5557/T5567/T5577 routines
 *------------------------------
 * NOTE: T55x7/T5555 configuration register definitions moved to protocols.h
 *
 * Relevant communication times in microsecond
 * To compensate antenna falling times shorten the write times
 * and enlarge the gap ones.
 * Q5 tags seems to have issues when these values changes. 
 */

/* Q5 timing datasheet:
 * Type                  |  MIN   | Typical |  Max   |
 * Start_Gap             |  10*8  |    ?    |  50*8  |
 * Write_Gap Normal mode |   8*8  |   14*8  |  20*8  | 
 * Write_Gap Fast Mode   |   8*8  |    ?    |  20*8  |
 * Write_0   Normal mode |  16*8  |   24*8  |  32*8  |
 * Write_1   Normal mode |  48*8  |   56*8  |  64*8  |
 * Write_0   Fast Mode   |   8*8  |   12*8  |  16*8  |
 * Write_1   Fast Mode   |  24*8  |   28*8  |  32*8  |
*/

/* T5557 timing datasheet:
 * Type                  |  MIN   | Typical |  Max   |
 * Start_Gap             |  10*8  |    ?    |  50*8  |
 * Write_Gap Normal mode |   8*8  |50-150us |  30*8  | 
 * Write_Gap Fast Mode   |   8*8  |    ?    |  20*8  |
 * Write_0   Normal mode |  16*8  |   24*8  |  31*8  | 
 * Write_1   Normal mode |  48*8  |   54*8  |  63*8  | 
 * Write_0   Fast Mode   |   8*8  |   12*8  |  15*8  |
 * Write_1   Fast Mode   |  24*8  |   28*8  |  31*8  |
*/

/* T5577C timing datasheet for Fixed-Bit-Length protocol (defualt):
 * Type                  |  MIN   | Typical |  Max   |
 * Start_Gap             |   8*8  |   15*8  |  50*8  |
 * Write_Gap Normal mode |   8*8  |   10*8  |  20*8  | 
 * Write_Gap Fast Mode   |   8*8  |   10*8  |  20*8  |
 * Write_0   Normal mode |  16*8  |   24*8  |  32*8  | 
 * Write_1   Normal mode |  48*8  |   56*8  |  64*8  | 
 * Write_0   Fast Mode   |   8*8  |   12*8  |  16*8  |
 * Write_1   Fast Mode   |  24*8  |   28*8  |  32*8  |
*/

//note startgap must be sent after tag has been powered up for more than 3ms (per T5557 ds)
#define START_GAP 31*8 //31*8 // was 250 // SPEC:  1*8 to 50*8 - typ 15*8 (or 15fc) - T5557: 10*8 to 50*8 
#define WRITE_GAP 20*8 //20*8 // was 160 // SPEC:  1*8 to 20*8 - typ 10*8 (or 10fc) - T5557:  8*8 to 30*8 typ 50-150us
#define WRITE_0   18*8 //18*8 // was 144 // SPEC: 16*8 to 32*8 - typ 24*8 (or 24fc) - T5557: 16*8 to 31*8 typ 24*8
#define WRITE_1   50*8 //50*8 // was 400 // SPEC: 48*8 to 64*8 - typ 56*8 (or 56fc) - T5557: 48*8 to 63*8 typ 54*8       432 for T55x7; 448 for E5550
#define READ_GAP  15*8 

void TurnReadLFOn(int delay) {
	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);
	// Give it a bit of time for the resonant antenna to settle.
	WaitUS(delay); //155*8 //50*8
}

// Write one bit to card
void T55xxWriteBit(int bit) {
	if (!bit)
		TurnReadLFOn(WRITE_0);
	else
		TurnReadLFOn(WRITE_1);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	WaitUS(WRITE_GAP);
}

// Send T5577 reset command then read stream (see if we can identify the start of the stream)
void T55xxResetRead(void) {
	LED_A_ON();
	//clear buffer now so it does not interfere with timing later
	BigBuf_Clear_keep_EM();

	// Set up FPGA, 125kHz
	LFSetupFPGAForADC(95, true);
	StartTicks();
	// make sure tag is fully powered up...
	WaitMS(5);
	
	// Trigger T55x7 in mode.
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	WaitUS(START_GAP);

	// reset tag - op code 00
	T55xxWriteBit(0);
	T55xxWriteBit(0);

	TurnReadLFOn(READ_GAP);

	// Acquisition
	DoPartialAcquisition(0, true, BigBuf_max_traceLen(), 0);

	// Turn the field off
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	cmd_send(CMD_ACK,0,0,0,0,0);    
	LED_A_OFF();
}

// Write one card block in page 0, no lock
void T55xxWriteBlockExt(uint32_t Data, uint32_t Block, uint32_t Pwd, uint8_t arg) {
	LED_A_ON();
	bool PwdMode = arg & 0x1;
	uint8_t Page = (arg & 0x2)>>1;
	bool testMode = arg & 0x4;
	uint32_t i = 0;

	// Set up FPGA, 125kHz
	LFSetupFPGAForADC(95, true);
	StartTicks();
	// make sure tag is fully powered up...
	WaitMS(5);
	// Trigger T55x7 in mode.
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	WaitUS(START_GAP);

	if (testMode) Dbprintf("TestMODE");
	// Std Opcode 10
	T55xxWriteBit(testMode ? 0 : 1);
	T55xxWriteBit(testMode ? 1 : Page); //Page 0

	if (PwdMode) {
		// Send Pwd
		for (i = 0x80000000; i != 0; i >>= 1)
			T55xxWriteBit(Pwd & i);
	}
	// Send Lock bit
	T55xxWriteBit(0);

	// Send Data
	for (i = 0x80000000; i != 0; i >>= 1)
		T55xxWriteBit(Data & i);

	// Send Block number
	for (i = 0x04; i != 0; i >>= 1)
		T55xxWriteBit(Block & i);

	// Perform write (nominal is 5.6 ms for T55x7 and 18ms for E5550,
	// so wait a little more)

	// "there is a clock delay before programming" 
	//  - programming takes ~5.6ms for t5577 ~18ms for E5550 or t5567
	//  so we should wait 1 clock + 5.6ms then read response? 
	//  but we need to know we are dealing with t5577 vs t5567 vs e5550 (or q5) marshmellow...
	if (testMode) {
		//TESTMODE TIMING TESTS: 
		// <566us does nothing 
		// 566-568 switches between wiping to 0s and doing nothing
		// 5184 wipes and allows 1 block to be programmed.
		// indefinite power on wipes and then programs all blocks with bitshifted data sent.
		TurnReadLFOn(5184); 

	} else {
		TurnReadLFOn(20 * 1000);
		//could attempt to do a read to confirm write took
		// as the tag should repeat back the new block 
		// until it is reset, but to confirm it we would 
		// need to know the current block 0 config mode for
		// modulation clock an other details to demod the response...
		// response should be (for t55x7) a 0 bit then (ST if on) 
		// block data written in on repeat until reset. 

		//DoPartialAcquisition(20, true, 12000);
	}

	// turn field off
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_A_OFF();
}

// Write one card block in page 0, no lock
void T55xxWriteBlock(uint32_t Data, uint32_t Block, uint32_t Pwd, uint8_t arg) {
	T55xxWriteBlockExt(Data, Block, Pwd, arg);
	cmd_send(CMD_ACK,0,0,0,0,0);
}

// Read one card block in page [page]
void T55xxReadBlock(uint16_t arg0, uint8_t Block, uint32_t Pwd) {
	LED_A_ON();
	bool PwdMode = arg0 & 0x1;
	uint8_t Page = (arg0 & 0x2) >> 1;
	uint32_t i = 0;
	bool RegReadMode = (Block == 0xFF);//regular read mode

	//clear buffer now so it does not interfere with timing later
	BigBuf_Clear_ext(false);

	//make sure block is at max 7
	Block &= 0x7;

	// Set up FPGA, 125kHz to power up the tag
	LFSetupFPGAForADC(95, true);
	StartTicks();
	// make sure tag is fully powered up...
	WaitMS(5);
	// Trigger T55x7 Direct Access Mode with start gap
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	WaitUS(START_GAP);

	// Opcode 1[page]
	T55xxWriteBit(1);
	T55xxWriteBit(Page); //Page 0

	if (PwdMode) {
		// Send Pwd
		for (i = 0x80000000; i != 0; i >>= 1)
			T55xxWriteBit(Pwd & i);
	}
	// Send a zero bit separation
	T55xxWriteBit(0);

	// Send Block number (if direct access mode)
	if (!RegReadMode)
		for (i = 0x04; i != 0; i >>= 1)
			T55xxWriteBit(Block & i);		

	// Turn field on to read the response
	// 137*8 seems to get to the start of data pretty well... 
	//  but we want to go past the start and let the repeating data settle in...
	TurnReadLFOn(210*8); 

	// Acquisition
	// Now do the acquisition
	DoPartialAcquisition(0, true, 12000, 0);

	// Turn the field off
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	cmd_send(CMD_ACK,0,0,0,0,0);    
	LED_A_OFF();
}

void T55xxWakeUp(uint32_t Pwd){
	LED_B_ON();
	uint32_t i = 0;
	
	// Set up FPGA, 125kHz
	LFSetupFPGAForADC(95, true);
	StartTicks();
	// make sure tag is fully powered up...
	WaitMS(5);
	
	// Trigger T55x7 Direct Access Mode
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	WaitUS(START_GAP);
	
	// Opcode 10
	T55xxWriteBit(1);
	T55xxWriteBit(0); //Page 0

	// Send Pwd
	for (i = 0x80000000; i != 0; i >>= 1)
		T55xxWriteBit(Pwd & i);

	// Turn and leave field on to let the begin repeating transmission
	TurnReadLFOn(20*1000);
}

/*-------------- Cloning routines -----------*/

void WriteT55xx(uint32_t *blockdata, uint8_t startblock, uint8_t numblocks) {
	// write last block first and config block last (if included)
	for (uint8_t i = numblocks+startblock; i > startblock; i--) {
		T55xxWriteBlockExt(blockdata[i-1],i-1,0,0);
	}
}

// Copy a HID-like card (e.g. HID Proximity, Paradox) to a T55x7 compatible card
void CopyHIDtoT55x7(uint32_t hi2, uint32_t hi, uint32_t lo, uint8_t longFMT, uint8_t preamble) {
	uint32_t data[] = {0,0,0,0,0,0,0};
	uint8_t last_block = 0;

	if (longFMT) {
		// Ensure no more than 84 bits supplied
		if (hi2>0xFFFFF) {
			DbpString("Tags can only have 84 bits.");
			return;
		}
		// Build the 6 data blocks for supplied 84bit ID
		last_block = 6;
		// load preamble & long format identifier (9E manchester encoded)
		data[1] = (preamble << 24) | 0x96A900 | (manchesterEncode2Bytes((hi2 >> 16) & 0xF) & 0xFF);
		// load raw id from hi2, hi, lo to data blocks (manchester encoded)
		data[2] = manchesterEncode2Bytes(hi2 & 0xFFFF);
		data[3] = manchesterEncode2Bytes(hi >> 16);
		data[4] = manchesterEncode2Bytes(hi & 0xFFFF);
		data[5] = manchesterEncode2Bytes(lo >> 16);
		data[6] = manchesterEncode2Bytes(lo & 0xFFFF);
	} else {
		// Ensure no more than 44 bits supplied
		if (hi>0xFFF) {
			DbpString("Tags can only have 44 bits.");
			return;
		}
		// Build the 3 data blocks for supplied 44bit ID
		last_block = 3;
		// load preamble
		data[1] = (preamble << 24) | (manchesterEncode2Bytes(hi) & 0xFFFFFF);
		data[2] = manchesterEncode2Bytes(lo >> 16);
		data[3] = manchesterEncode2Bytes(lo & 0xFFFF);
	}
	// load chip config block
	data[0] = T55x7_BITRATE_RF_50 | T55x7_MODULATION_FSK2a | last_block << T55x7_MAXBLOCK_SHIFT;

	//TODO add selection of chip for Q5 or T55x7
	// data[0] = (((50-2)/2)<<T5555_BITRATE_SHIFT) | T5555_MODULATION_FSK2 | T5555_INVERT_OUTPUT | last_block << T5555_MAXBLOCK_SHIFT;

	LED_D_ON();
	// Program the data blocks for supplied ID
	// and the block 0 for HID format
	WriteT55xx(data, 0, last_block+1);

	LED_D_OFF();

	DbpString("DONE!");
}

void CopyIOtoT55x7(uint32_t hi, uint32_t lo) {
	uint32_t data[] = {T55x7_BITRATE_RF_64 | T55x7_MODULATION_FSK2a | (2 << T55x7_MAXBLOCK_SHIFT), hi, lo};
	//TODO add selection of chip for Q5 or T55x7
	// data[0] = (((64-2)/2)<<T5555_BITRATE_SHIFT) | T5555_MODULATION_FSK2 | T5555_INVERT_OUTPUT | 2 << T5555_MAXBLOCK_SHIFT;

	LED_D_ON();
	// Program the data blocks for supplied ID
	// and the block 0 config
	WriteT55xx(data, 0, 3);

	LED_D_OFF();

	DbpString("DONE!");
}

// Clone Indala 64-bit tag by UID to T55x7
void CopyIndala64toT55x7(uint32_t hi, uint32_t lo) {
	//Program the 2 data blocks for supplied 64bit UID
	// and the Config for Indala 64 format (RF/32;PSK1 with RF/2;Maxblock=2)
	uint32_t data[] = { T55x7_BITRATE_RF_32 | T55x7_MODULATION_PSK1 | (2 << T55x7_MAXBLOCK_SHIFT), hi, lo};
	//TODO add selection of chip for Q5 or T55x7
	// data[0] = (((32-2)/2)<<T5555_BITRATE_SHIFT) | T5555_MODULATION_PSK1 | 2 << T5555_MAXBLOCK_SHIFT;

	WriteT55xx(data, 0, 3);
	//Alternative config for Indala (Extended mode;RF/32;PSK1 with RF/2;Maxblock=2;Inverse data)
	//	T5567WriteBlock(0x603E1042,0);
	DbpString("DONE!");
}
// Clone Indala 224-bit tag by UID to T55x7
void CopyIndala224toT55x7(uint32_t uid1, uint32_t uid2, uint32_t uid3, uint32_t uid4, uint32_t uid5, uint32_t uid6, uint32_t uid7) {
	//Program the 7 data blocks for supplied 224bit UID
	uint32_t data[] = {0, uid1, uid2, uid3, uid4, uid5, uid6, uid7};
	// and the block 0 for Indala224 format	
	//Config for Indala (RF/32;PSK2 with RF/2;Maxblock=7)
	data[0] = T55x7_BITRATE_RF_32 | T55x7_MODULATION_PSK2 | (7 << T55x7_MAXBLOCK_SHIFT);
	//TODO add selection of chip for Q5 or T55x7
	// data[0] = (((32-2)>>1)<<T5555_BITRATE_SHIFT) | T5555_MODULATION_PSK2 | 7 << T5555_MAXBLOCK_SHIFT;
	WriteT55xx(data, 0, 8);
	//Alternative config for Indala (Extended mode;RF/32;PSK1 with RF/2;Maxblock=7;Inverse data)
	//	T5567WriteBlock(0x603E10E2,0);
	DbpString("DONE!");
}
// clone viking tag to T55xx
void CopyVikingtoT55xx(uint32_t block1, uint32_t block2, uint8_t Q5) {
	uint32_t data[] = {T55x7_BITRATE_RF_32 | T55x7_MODULATION_MANCHESTER | (2 << T55x7_MAXBLOCK_SHIFT), block1, block2};
	if (Q5) data[0] = T5555_SET_BITRATE(32) | T5555_MODULATION_MANCHESTER | 2 << T5555_MAXBLOCK_SHIFT;
	// Program the data blocks for supplied ID and the block 0 config
	WriteT55xx(data, 0, 3);
	LED_D_OFF();
	cmd_send(CMD_ACK,0,0,0,0,0);
}

// Define 9bit header for EM410x tags
#define EM410X_HEADER		  0x1FF
#define EM410X_ID_LENGTH	40

void WriteEM410x(uint32_t card, uint32_t id_hi, uint32_t id_lo) {
	int i, id_bit;
	uint64_t id = EM410X_HEADER;
	uint64_t rev_id = 0;	// reversed ID
	int c_parity[4];	// column parity
	int r_parity = 0;	// row parity
	uint32_t clock = 0;

	// Reverse ID bits given as parameter (for simpler operations)
	for (i = 0; i < EM410X_ID_LENGTH; ++i) {
		if (i < 32) {
			rev_id = (rev_id << 1) | (id_lo & 1);
			id_lo >>= 1;
		} else {
			rev_id = (rev_id << 1) | (id_hi & 1);
			id_hi >>= 1;
		}
	}

	for (i = 0; i < EM410X_ID_LENGTH; ++i) {
		id_bit = rev_id & 1;

		if (i % 4 == 0) {
			// Don't write row parity bit at start of parsing
			if (i)
				id = (id << 1) | r_parity;
			// Start counting parity for new row
			r_parity = id_bit;
		} else {
			// Count row parity
			r_parity ^= id_bit;
		}

		// First elements in column?
		if (i < 4)
			// Fill out first elements
			c_parity[i] = id_bit;
		else
			// Count column parity
			c_parity[i % 4] ^= id_bit;

		// Insert ID bit
		id = (id << 1) | id_bit;
		rev_id >>= 1;
	}

	// Insert parity bit of last row
	id = (id << 1) | r_parity;

	// Fill out column parity at the end of tag
	for (i = 0; i < 4; ++i)
		id = (id << 1) | c_parity[i];

	// Add stop bit
	id <<= 1;

	Dbprintf("Started writing %s tag ...", card ? "T55x7":"T5555");
	LED_D_ON();

	// Write EM410x ID
	uint32_t data[] = {0, (uint32_t)(id>>32), (uint32_t)(id & 0xFFFFFFFF)};

	clock = (card & 0xFF00) >> 8;
	clock = (clock == 0) ? 64 : clock;
	Dbprintf("Clock rate: %d", clock);
	if (card & 0xFF) { //t55x7
		clock = GetT55xxClockBit(clock);			
		if (clock == 0) {
			Dbprintf("Invalid clock rate: %d", clock);
			return;
		}
		data[0] = clock | T55x7_MODULATION_MANCHESTER | (2 << T55x7_MAXBLOCK_SHIFT);
	} else { //t5555 (Q5)
		data[0] = T5555_SET_BITRATE(clock) | T5555_MODULATION_MANCHESTER | (2 << T5555_MAXBLOCK_SHIFT);
	}

	WriteT55xx(data, 0, 3);

	LED_D_OFF();
	Dbprintf("Tag %s written with 0x%08x%08x\n", card ? "T55x7":"T5555",
			 (uint32_t)(id >> 32), (uint32_t)id);
}

//-----------------------------------
// EM4469 / EM4305 routines
//-----------------------------------
#define FWD_CMD_LOGIN 0xC //including the even parity, binary mirrored
#define FWD_CMD_WRITE 0xA
#define FWD_CMD_READ 0x9
#define FWD_CMD_DISABLE 0x5
#define FWD_CMD_PROTECT 0x3

uint8_t forwardLink_data[64]; //array of forwarded bits
uint8_t * forward_ptr; //ptr for forward message preparation
uint8_t fwd_bit_sz; //forwardlink bit counter
uint8_t * fwd_write_ptr; //forwardlink bit pointer

//====================================================================
// prepares command bits
// see EM4469 spec
//====================================================================
//--------------------------------------------------------------------
//  VALUES TAKEN FROM EM4x function: SendForward
//  START_GAP = 440;       (55*8) cycles at 125Khz (8us = 1cycle)
//  WRITE_GAP = 128;       (16*8)
//  WRITE_1   = 256 32*8;  (32*8) 

//  These timings work for 4469/4269/4305 (with the 55*8 above)
//  WRITE_0 = 23*8 , 9*8  SpinDelayUs(23*8); 

uint8_t Prepare_Cmd( uint8_t cmd ) {

	*forward_ptr++ = 0; //start bit
	*forward_ptr++ = 0; //second pause for 4050 code

	*forward_ptr++ = cmd;
	cmd >>= 1;
	*forward_ptr++ = cmd;
	cmd >>= 1;
	*forward_ptr++ = cmd;
	cmd >>= 1;
	*forward_ptr++ = cmd;

	return 6; //return number of emited bits
}

//====================================================================
// prepares address bits
// see EM4469 spec
//====================================================================
uint8_t Prepare_Addr( uint8_t addr ) {

	register uint8_t line_parity;

	uint8_t i;
	line_parity = 0;
	for(i=0;i<6;i++) {
		*forward_ptr++ = addr;
		line_parity ^= addr;
		addr >>= 1;
	}

	*forward_ptr++ = (line_parity & 1);

	return 7; //return number of emited bits
}

//====================================================================
// prepares data bits intreleaved with parity bits
// see EM4469 spec
//====================================================================
uint8_t Prepare_Data( uint16_t data_low, uint16_t data_hi) {

	register uint8_t line_parity;
	register uint8_t column_parity;
	register uint8_t i, j;
	register uint16_t data;

	data = data_low;
	column_parity = 0;

	for(i=0; i<4; i++) {
		line_parity = 0;
		for(j=0; j<8; j++) {
			line_parity ^= data;
			column_parity ^= (data & 1) << j;
			*forward_ptr++ = data;
			data >>= 1;
		}
		*forward_ptr++ = line_parity;
		if(i == 1)
			data = data_hi;
	}

	for(j=0; j<8; j++) {
		*forward_ptr++ = column_parity;
		column_parity >>= 1;
	}
	*forward_ptr = 0;

	return 45; //return number of emited bits
}

//====================================================================
// Forward Link send function
// Requires: forwarLink_data filled with valid bits (1 bit per byte)
// fwd_bit_count set with number of bits to be sent
//====================================================================
void SendForward(uint8_t fwd_bit_count) {

	fwd_write_ptr = forwardLink_data;
	fwd_bit_sz = fwd_bit_count;

	// Set up FPGA, 125kHz or 95 divisor
	LFSetupFPGAForADC(95, true);

	// force 1st mod pulse (start gap must be longer for 4305)
	fwd_bit_sz--; //prepare next bit modulation
	fwd_write_ptr++;
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	WaitUS(55*8); //55 cycles off (8us each)for 4305  //another reader has 37 here...
	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);//field on
	WaitUS(18*8); //18 cycles on (8us each)

	// now start writting
	while(fwd_bit_sz-- > 0) { //prepare next bit modulation
		if(((*fwd_write_ptr++) & 1) == 1)
			WaitUS(32*8); //32 cycles at 125Khz (8us each)
		else {
			//These timings work for 4469/4269/4305 (with the 55*8 above)
			FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
			WaitUS(23*8); //23 cycles off (8us each)
			FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);//field on
			WaitUS(18*8); //18 cycles on (8us each)
		}
	}
}

void EM4xLogin(uint32_t Password) {

	uint8_t fwd_bit_count;

	forward_ptr = forwardLink_data;
	fwd_bit_count = Prepare_Cmd( FWD_CMD_LOGIN );
	fwd_bit_count += Prepare_Data( Password&0xFFFF, Password>>16 );

	SendForward(fwd_bit_count);

	//Wait for command to complete
	SpinDelay(20);
}

void EM4xReadWord(uint8_t Address, uint32_t Pwd, uint8_t PwdMode) {

	uint8_t fwd_bit_count;

	// Clear destination buffer before sending the command
	BigBuf_Clear_ext(false);

	LED_A_ON();
	StartTicks();
	//If password mode do login
	if (PwdMode == 1) EM4xLogin(Pwd);

	forward_ptr = forwardLink_data;
	fwd_bit_count = Prepare_Cmd( FWD_CMD_READ );
	fwd_bit_count += Prepare_Addr( Address );

	SendForward(fwd_bit_count);
	WaitUS(400);
	// Now do the acquisition
	DoPartialAcquisition(20, true, 6000, 1000);
	
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	LED_A_OFF();
	cmd_send(CMD_ACK,0,0,0,0,0);
}

void EM4xWriteWord(uint32_t flag, uint32_t Data, uint32_t Pwd) {
	
	bool PwdMode = (flag & 0x1);
	uint8_t Address = (flag >> 8) & 0xFF;
	uint8_t fwd_bit_count;

	//clear buffer now so it does not interfere with timing later
	BigBuf_Clear_ext(false);

	LED_A_ON();
	StartTicks();
	//If password mode do login
	if (PwdMode) EM4xLogin(Pwd);

	forward_ptr = forwardLink_data;
	fwd_bit_count = Prepare_Cmd( FWD_CMD_WRITE );
	fwd_bit_count += Prepare_Addr( Address );
	fwd_bit_count += Prepare_Data( Data&0xFFFF, Data>>16 );

	SendForward(fwd_bit_count);

	//Wait for write to complete
	//SpinDelay(10);

	WaitUS(6500);
	//Capture response if one exists
	DoPartialAcquisition(20, true, 6000, 1000);

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	LED_A_OFF();
	cmd_send(CMD_ACK,0,0,0,0,0);
}

void EM4xProtect(uint32_t flag, uint32_t Data, uint32_t Pwd) {
	
	bool PwdMode = (flag & 0x1);
	uint8_t fwd_bit_count;

	//clear buffer now so it does not interfere with timing later
	BigBuf_Clear_ext(false);

	LED_A_ON();
	StartTicks();
	//If password mode do login
	if (PwdMode) EM4xLogin(Pwd);

	forward_ptr = forwardLink_data;
	fwd_bit_count = Prepare_Cmd( FWD_CMD_PROTECT );

	//unsure if this needs the full packet config...
	fwd_bit_count += Prepare_Data( Data&0xFFFF, Data>>16 );

	SendForward(fwd_bit_count);

	//Wait for write to complete
	//SpinDelay(10);

	WaitUS(6500);
	//Capture response if one exists
	DoPartialAcquisition(20, true, 6000, 1000);

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	LED_A_OFF();
	cmd_send(CMD_ACK,0,0,0,0,0);
}
/*
Reading a COTAG.

COTAG needs the reader to send a startsequence and the card has an extreme slow datarate.
because of this, we can "sample" the data signal but we interpreate it to Manchester direct.

READER START SEQUENCE:
burst 800 us,    gap   2.2 msecs
burst 3.6 msecs  gap   2.2 msecs
burst 800 us     gap   2.2 msecs
pulse 3.6 msecs

This triggers a COTAG tag to response
*/
void Cotag(uint32_t arg0) {

#define OFF     { FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); WaitUS(2035); }
#define ON(x)   { FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD); WaitUS((x)); }

	uint8_t rawsignal = arg0 & 0xF;

	LED_A_ON();

	// Switching to LF image on FPGA. This might empty BigBuff
	FpgaDownloadAndGo(FPGA_BITSTREAM_LF);

	//clear buffer now so it does not interfere with timing later
	BigBuf_Clear_ext(false);

	// Set up FPGA, 132kHz to power up the tag
	FpgaSendCommand(FPGA_CMD_SET_DIVISOR, 89);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);

	// Connect the A/D to the peak-detected low-frequency path.
	SetAdcMuxFor(GPIO_MUXSEL_LOPKD);

	// Now set up the SSC to get the ADC samples that are now streaming at us.
	FpgaSetupSsc(FPGA_MAJOR_MODE_LF_ADC);

	// start clock - 1.5ticks is 1us
	StartTicks();

	//send COTAG start pulse
	ON(740)  OFF
	ON(3330) OFF
	ON(740)  OFF
	ON(1000)

	switch(rawsignal) {
		case 0: doCotagAcquisition(50000); break;
		case 1: doCotagAcquisitionManchester(); break;
		case 2: DoAcquisition_config(true, 0); break;
	}

	// Turn the field off
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF); // field off
	cmd_send(CMD_ACK,0,0,0,0,0);
	LED_A_OFF();
}
