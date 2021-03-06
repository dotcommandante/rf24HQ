/*-
 * Copyright (c) 2012 Darran Hunt (darran [at] hunt dot net dot nz)
 * All rights reserved.
 * Some parts copyright (c) 2012 Eric Brundick (spirilis [at] linux dot com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Release history
 *
 * Version  Date         Description
 * 0.1      22-Apr-2012  First release.
 *
 */

#include <avr/pgmspace.h>
#include <SPI.h>
#include "rf24HQ.h"

/* Work around a bug with PROGMEM and PSTR where the compiler always
 * generates warnings.
 */
#undef PROGMEM 
#define PROGMEM __attribute__(( section(".progmem.data") )) 
#undef PSTR 
#define PSTR(s) (__extension__({static prog_char __c[] PROGMEM = (s); &__c[0];})) 


/* Save 1466 bytes by not using SNPRINTF */
#undef USE_SNPRINTF

rf24::rf24(uint8_t cePinSet, uint8_t csnPinSet, uint8_t channelSet, uint8_t size)
{
    acked = false;
    sending = false;
    autoAck = false;
    channel = channelSet;
    packetSize = size;
    cePin = cePinSet;
    csnPin = csnPinSet;
    rfpower = RF24_POWER_MINUS6DBM;
    rfspeed = RF24_SPEED_1MBPS;
    txByteCount = 0;
    txCount = 0;
    txEnabled = false;
    rxEnabled = false;
    maxChan = 83;	// NZ, US, 2.483 GHz max allowed channel

    /* Default to interrupts masked, CRC16 enabled, powered down */
    config = 1<<MASK_RX_DR | 1<<MASK_TX_DS | 1<<MASK_MAX_RT | 1<<EN_CRC | 1<<CRCO;
}

void rf24::chipDisable()
{
    digitalWrite(cePin,LOW);
}

void rf24::chipEnable()
{
    digitalWrite(cePin,HIGH);
    delayMicroseconds(10);  // 10us minimum Thce
}

void rf24::chipSelect()
{
    digitalWrite(csnPin,LOW);
}

void rf24::chipDeselect()
{
    digitalWrite(csnPin,HIGH);
}

/**
* Initialize the nRF24L01+.
* @param dataRate the data rate to use. 250000, 1000000, or 2000000.
* @param debugPrint optional debug print stream.
*/
boolean rf24::begin(uint32_t dataRate, Print *debugPrint)
{
    debug.begin(debugPrint);

    pinMode(cePin,OUTPUT);
    pinMode(csnPin,OUTPUT);

    chipDisable();
    chipDeselect();

    SPI.begin();
    SPI.setDataMode(SPI_MODE0);
    SPI.setClockDivider(SPI_CLOCK_DIV2);

    acked = false;
    sending = false;

    delay(100);		// 100ms power on reset
    powerDown();

    writeReg(CONFIG, config);
    setChannel(channel);
    setPacketSize(packetSize);
    setSpeed(dataRate);
    setPowerReg(rfpower);

    /* Make sure the module is working */
    if ((getChannel() != channel) || (readReg(RX_PW_P0) != packetSize)) {
	debug.println(F("rf24: failed to initialise"));
	return false;
    }

    writeReg(STATUS, (1 << RX_DR) | (1 << TX_DS) | (1 << MAX_RT)); 

    // Start receiver 
    enableRx();

    // clear out any old packets 
    flushRx();
    flushTx();

    return true;
}

/**
* Initialize the nRF24L01+.
*/
boolean rf24::begin(Print *debugPrint)
{
    return begin(1000000, debugPrint);
}

uint8_t rf24::getPacketSize()
{
    return packetSize;
}

void rf24::setPacketSize(uint8_t size)
{
    packetSize = size;
    writeReg(RX_PW_P0, packetSize);
    writeReg(RX_PW_P1, packetSize);
    writeReg(RX_PW_P2, packetSize);
    writeReg(RX_PW_P3, packetSize);
    writeReg(RX_PW_P4, packetSize);
    writeReg(RX_PW_P5, packetSize);
}

void rf24::setCRC(uint8_t value)
{
    config = (config & ((1 << CRCO) | (1 << EN_CRC))) | (value & ((1 << CRCO) | (1 << EN_CRC)));
    writeReg(CONFIG, config);
}

void rf24::setCRC8()
{
    setCRC(1 << EN_CRC);
}

void rf24::setCRC16()
{
    setCRC(1 << EN_CRC | 1 << CRCO);
}

void rf24::setCRCOn()
{
    setCRC(1 << EN_CRC);
}

void rf24::setCRCOff()
{
    setCRC(0);
}

uint8_t rf24::_convertSpeedToReg(uint32_t rfspd)
{
    if (rfspd >= 2000000UL)
        return RF24_SPEED_2MBPS;
    if (rfspd >= 1000000UL)
        return RF24_SPEED_1MBPS;
    return RF24_SPEED_250KBPS;
}

uint32_t rf24::_convertRegToSpeed(uint8_t rfspdreg)
{
    switch (rfspdreg) {
        case RF24_SPEED_2MBPS:
            return(2000000UL);
        case RF24_SPEED_1MBPS:
            return(1000000UL);
        case RF24_SPEED_250KBPS:
            return(250000UL);
        default:
            return(0);  // Unknown register value
    }
}

void rf24::setSpeed(uint32_t rfspd)
{
    setSpeedReg(_convertSpeedToReg(rfspd));
}

void rf24::setSpeedReg(uint8_t setting)
{
    uint8_t rfset;

    rfset = readReg(RF_SETUP);
    if (setting > 2) {  // Erroneous value, assume the user means maximum speed?
        rfspeed = RF24_SPEED_2MBPS;
    } else {
        rfspeed = setting;
    }
    rfset &= ~( (1<<RF_DR_HIGH) | (1<<RF_DR_LOW) );
    if (rfspeed == RF24_SPEED_250KBPS) {
        rfset |= 1 << RF_DR_LOW;
    } else {
        rfset |= (rfspeed & 0x01) << RF_DR_HIGH;
    }

    writeReg(RF_SETUP, rfset);
}

uint32_t rf24::getSpeed()
{
    return (_convertRegToSpeed(getSpeedReg()));
}

uint8_t rf24::getSpeedReg()
{
    uint8_t rfset;

    rfset = readReg(RF_SETUP);
    rfspeed = ((rfset >> (RF_DR_LOW-1)) & 0x02) | ((rfset >> RF_DR_HIGH) & 0x01);
    return(rfspeed);
}

char* rf24::getSpeedString(char *buf)
{
    getSpeedReg();  // Sets the 'rfspeed' variable as a side-effect
    switch(rfspeed) {
      case RF24_SPEED_250KBPS:
          strcpy_P(buf, PSTR("250Kbps"));
          break;

      case RF24_SPEED_1MBPS:
          strcpy_P(buf, PSTR("1Mbps"));
          break;

      case RF24_SPEED_2MBPS:
          strcpy_P(buf, PSTR("2Mbps"));
          break;

      default:
          strcpy_P(buf, PSTR("Unknown"));
    }
    return(buf);
}

void rf24::setPowerReg(uint8_t setting)
{
    uint8_t rfset;

    rfset = readReg(RF_SETUP);
    if (setting > 0x03) {  // User not providing a #defined constant like they should...
        rfpower = RF24_POWER_MAX;
    } else {
        rfpower = setting;
    }
    rfset &= ~(0x06);  // Clear bit 2, 1
    rfset |= ((rfpower & 0x03) << 1);  // Copy rfpower in place.

    writeReg(RF_SETUP, rfset);
}

uint8_t rf24::getPowerReg()
{
    uint8_t rfset;

    rfset = readReg(RF_SETUP);
    rfset &= 0x06;
    rfset >>= 1;
    rfpower = rfset;
    return(rfpower);
}

char* rf24::getPowerString(char *buf)
{
    getPowerReg();  // Sets rfpower as a side-effect
    switch(rfpower) {
        case RF24_POWER_0DBM:
            strcpy_P(buf, PSTR("0dBm"));
            break;

        case RF24_POWER_MINUS6DBM:
            strcpy_P(buf, PSTR("-6dBm"));
            break;

        case RF24_POWER_MINUS12DBM:
            strcpy_P(buf, PSTR("-12dBm"));
            break;

        case RF24_POWER_MINUS18DBM:
            strcpy_P(buf, PSTR("-18dBm"));
            break;

        default:
            strcpy_P(buf, PSTR("Unknown"));
    }
    return(buf);
}

/** Set the transmit power level
 * @param dBm power level. Valid values are -18, -12, -6, and 0. Setting will be rounded down to nearest. */
void rf24::setTxPower(int8_t dBm)
{
    if (dBm < -12) {
        setPowerReg(RF24_POWER_MINUS18DBM);
    } else if (dBm <= -6) {
        setPowerReg(RF24_POWER_MINUS12DBM);
    } else if (dBm <= 0) {
        setPowerReg(RF24_POWER_MINUS6DBM);
    } else {
        setPowerReg(RF24_POWER_0DBM);
    }
}

uint8_t rf24::transfer(uint8_t data)
{
    return SPI.transfer(data);
}

/** Send data
 * @param data the data to send
 * @param len number of bytes to send
 * @param max the total packet size to send. Data is padded out to this length.
 */
void rf24::tx(const void *data, uint8_t len, uint8_t max)
{

    for (uint8_t ind=0; ind < len; ind++) {
	if (ind < max) {
	    transfer(((uint8_t *)data)[ind]);
	} else {
	    transfer(0);
	}
    }
}

/** Send register data - LSBFirst */
void rf24::txlsbfirst(const void *data, uint8_t len)
{
    int8_t ind=len-1; 
    while (ind >= 0) {
	transfer(((uint8_t *)data)[ind--]);
    }
}

/** Receive data
 * @param data receive data here
 * @param len number of bytes to write to data
 * @param max total number of bytes to read. Bytes greater than len are discarded.
 */
void rf24::rx(void *data, uint8_t len, uint8_t max)
{

    for (uint8_t ind=0; ind < len; ind++) {
	if (ind < max) {
	    ((uint8_t *)data)[ind] = transfer(0);
	} else {
	    transfer(0);
	}
    }
}

void rf24::rxlsbfirst(void *data, uint8_t len, uint8_t max)
{
    int8_t ind;
    if (max < len) {
	ind = max-1;
    } else {
	ind = len-1;
    }

    while (ind >= 0) {
	((uint8_t *)data)[ind--] = transfer(0);
    }
}

/** Send and receive data */
void rf24::txrx(uint8_t *txdata, uint8_t *rxdata, uint8_t len, uint8_t max)
{
    for (uint8_t ind=0; ind < len; ind++) {
	if (ind < max) {
	    if (rxdata != NULL) {
		if (txdata != NULL) {
		    rxdata[ind] = transfer(txdata[ind]);
		} else {
		    rxdata[ind] = transfer(0);
		}
	    } else {
		if (txdata != NULL) {
		    transfer(txdata[ind]);
		} else {
		    transfer(0);
		}
	    }
	} else {
	    transfer(0);
	}
    }
}

/** Set TX or RX power state */
void rf24::setPower(uint8_t value)
{
    bool settle = ((config & (1<<PWR_UP)) == 0);
    config = (config & ~(1<<PWR_UP | 1<<PRIM_RX)) | (value & (1<<PWR_UP | 1<<PRIM_RX));
    writeReg(CONFIG, config);
    if (settle) {
	// note this could reduce as low as 1500 usecs if 
	// the series inductance of the crystal is < 30mH
	// <30mH -> 1500 us
	// <60mH -> 3000 us
	// <90mH -> 4500 us
	delayMicroseconds(4500);   // tpd2stby - power down -> standby external crystal
    }
}

/** Enable receive (disables transmit) */
void rf24::enableRx(bool force)
{
    if (!rxEnabled || force) {
	chipDisable();
	setPower(1<<PWR_UP | 1<<PRIM_RX);
	//delayMicroseconds(150);   // tpd2stby - power down -> standby
	chipEnable();
	delayMicroseconds(130);   // Tstby2a - minimum delay ("RX settling")
	rxEnabled = true;
	txEnabled = false;
    }
}

/** Flush the receive buffers. All received data is discarded */
void rf24::flushRx()
{
    writeReg(FLUSH_RX);
}

/** Flush the transmit buffers. All pending tx data is discarded */
void rf24::flushTx()
{
    writeReg(FLUSH_TX);
}


/** Enable transmit (disables receive) */
void rf24::enableTx()
{
    if (!txEnabled) {
	setPower(1<<PWR_UP);
	delayMicroseconds(130);  // Tstby2a - Standby-to-Active minimum delay ("TX settling")
	txEnabled = true;
	rxEnabled = false;
    }
}

/** Report # of retransmits since the last sent packet */
uint8_t rf24::getRetransmits()
{
    return ( (readReg(OBSERVE_TX) >> ARC_CNT) & 0x0F );
}

uint8_t rf24::getFailedSends()
{
    return ( (readReg(OBSERVE_TX) >> PLOS_CNT) & 0x0F );
}

void rf24::resetFailedSends()
{
    uint8_t chan;

    chan = getChannel();
    setChannel(chan);  // According to the datasheet, PLOS_CNT is only reset by writing to RF_CH!
}

/** Disable transmit and receive. 900nA current draw. */
void rf24::powerDown()
{
    setPower(0);
    rxEnabled = false;
    txEnabled = false;
}

/** Set the receive address for a queue 
 * @param id The id of the queue to set the address for. Default
 *           receive queue is 1.
 * @param addr Pointer to the 5 byte address to set.
 * @ note first 4 bytes of addresses 2 through 5 must match address 1
 */
void rf24::setRxAddr(uint8_t id, const void *addr)
{
    chipDisable();
    if (id < 2) {
	writeReg(RX_ADDR_P0+id, (const uint8_t *)addr, RF24_ADDR_LEN);
    } else {
	writeReg(RX_ADDR_P0+id, ((uint8_t *)addr)[4]);
    }
    chipEnable();
}

/** Set the transmit address. Also sets the receive address to the same 
 * to support the auto-ack feature.
 * @param addr pointer to the 5 byte address to set
 */
void rf24::setTxAddr(const void *addr)
{
    writeReg(TX_ADDR, (const uint8_t *)addr, RF24_ADDR_LEN);
    if (autoAck) {
	/* 
	 * RX_ADDR_P0 is used for the auto ack feature, and 
	 * needs to be the same as the TX address 
	 */
	writeReg(RX_ADDR_P0, (const uint8_t *)addr, RF24_ADDR_LEN);
    }
}

char *rf24::getTxAddr(char *addr)
{
    readReg(TX_ADDR, addr, RF24_ADDR_LEN);
    addr[RF24_ADDR_LEN] = 0;
    return addr;
}

char *rf24::getRxAddr(char *addr)
{
    readReg(RX_ADDR_P1, addr, RF24_ADDR_LEN);
    addr[RF24_ADDR_LEN] = 0;
    return addr;
}

/** Read the value of a multi-byte register.
 * @param reg The register to read
 * @param value pointer to a buffer to store the value
 * @param size number of bytes to read
 */
void rf24::readReg(uint8_t reg, void *value, uint8_t size)
{
    chipSelect();
    transfer(R_REGISTER | (REGISTER_MASK & reg));
    rxlsbfirst(value,size);
    chipDeselect();
}

/** Read the value of byte register. */
uint8_t rf24::readReg(uint8_t reg)
{
    uint8_t data;
    chipSelect();
    transfer(R_REGISTER | (REGISTER_MASK & reg));
    data = transfer(0);
    chipDeselect();

    return data;
}

/** Write a command */
void rf24::writeReg(uint8_t reg)
{
    chipSelect();
    transfer(reg);
    chipDeselect();
}

/** Write a value to a single-byte register
 * @param reg The register to write
 * @param value the value to write
 */
void rf24::writeReg(uint8_t reg, uint8_t value)
{
    chipSelect();
    transfer(W_REGISTER | (REGISTER_MASK & reg));
    transfer(value);
    chipDeselect();
}

/** Write a value to a multi-byte register
 * @param reg The register to write
 * @param value pointer to the value to write
 * @param size number of bytes to write
 */
void rf24::writeReg(uint8_t reg, const void *value, uint8_t size)
{
    chipSelect();
    transfer(W_REGISTER | (REGISTER_MASK & reg));
    txlsbfirst(value,size);
    chipDeselect();
}

/** Check to see if the rf24 is sending a packet.
 * If enableReceive is true and the rf24 is not sending, enable the receiver.
 * @param enableReceive if true then enable the receiver if not sending.
 * @returns true if sending a packet, false otherwise.
 */
boolean rf24::isSending(bool enableReceive)
{
    if (!sending) {
        return false;
    }

    if (txFifoEmpty()) {
	acked = ((readReg(STATUS) & (1 << TX_DS)) != 0);
	sending = false;
	writeReg(STATUS, (1 << TX_DS) | (1 << MAX_RT)); 
	if (enableReceive) {
	    enableRx();
	}
	return false;
    } else {
	uint8_t status = readReg(STATUS);

	if (status & ((1 << TX_DS) | (1 << MAX_RT))) {
	    if (!(status && (1<<MAX_RT))) {
		debug.print(F("isSending: txFifo not empty, but STATUS=0x"));
		debug.println(status,HEX);
		flushTx();
	    }
	    acked = ((status & (1 << TX_DS)) != 0);
	    sending = false;
	    writeReg(STATUS, (1 << TX_DS) | (1 << MAX_RT)); 
	    if (enableReceive) {
		enableRx();
	    }
#ifdef DEBUG
	    debug.print(F("isSending: txFifo not empty, acked="));
	    debug.print(acked);
	    debug.print(F(", CONFIG=0x"));
	    debug.print(readReg(CONFIG),HEX);
	    debug.print(F(", STATUS=0x"));
	    debug.print(readReg(STATUS),HEX);
	    debug.print(F(", OBSERVE_TX=0x"));
	    debug.print(readReg(OBSERVE_TX),HEX);
	    debug.print(F(", FIFO_STATUS=0x"));
	    debug.println(readReg(FIFO_STATUS),HEX);
#endif
	    return false;
	}
    }

    return true;
}

/** Check to see if an ACK was received for the last packet sent.
 * @returns true if an ACK was received, else false.
 */
boolean rf24::gotAck()
{
    return acked;
}

/** Resend last failed transmission */
void rf24::resend()
{
    chipEnable();
    chipDisable();
}

uint8_t rf24::getTxRetries()
{
    return readReg(OBSERVE_TX) & 0xF;
}

uint8_t rf24::getTxLoss(bool clear)
{
    uint8_t loss = (readReg(OBSERVE_TX) >> 4) & 0xF;
    if (clear) {
	writeReg(RF_CH, channel);	// reset loss count
    }

    return loss;
}

/** Send a packet
 * @param data pointer to the data to send
 * @param size number of bytes to send
 * @param blocking false for nonblocking, true to wait for ack
 * @param timeout time out send if blocking and no ack in this many milliseconds
 * @note the packet will always contain packetSize bytes, 
 *       if size > packetSize then the data is truncated,
 *       if size < packetSize then the packet is padded with 0.
 */
bool rf24::send(void *data, uint8_t size, bool blocking, uint16_t timeout) 
{
    if (isSending(false)) {
	uint32_t start = millis();
	bool time=false;
	do {
	    time = (millis() - start) > timeout;
	} while (!time && isSending(false));

	if (time) {
	    debug.println(F("rf24::send() timed out on initial isSending check"));
	    dumpRegisters();
	    return false;
	}
	debug.print(F("rf24::send() delayed "));
	debug.print(millis()-start);
	debug.println(F(" msecs due to isSending()"));
    }

    chipDisable();
    enableTx();

    writeReg(FLUSH_TX);
    
    chipSelect();
    transfer(W_TX_PAYLOAD);
    tx(data, packetSize, size);
    chipDeselect();

    chipEnable();
    sending = true;
    acked = false;

    txByteCount += size;
    txCount++;

    if (blocking) {
	uint32_t start = millis();
	bool time=false;

	do {
	    time = (millis() - start) > timeout;
	} while (!time && isSending());

	if (time) {
	    debug.println(F("rf24::send() timed out"));
	    return false;
	}
	return gotAck();
    }

    return false;
}

uint8_t rf24::getAverageTxSize()
{
    if (txCount > 0) {
	return (uint8_t)(txByteCount / txCount);
    } else {
	return 0;
    }
}

/** Read a packet
 *@param data Pointer to buffer to read packet into
 *@param size the size of the buffer, max amount of data to read
 */
void rf24::read(void *data, uint8_t size) 
{
    chipSelect();
    transfer(R_RX_PAYLOAD);
    rx((uint8_t *)data, packetSize, size);
    chipDeselect();
    writeReg(STATUS, 1<<RX_DR);
}

/** See if the device is contactable and registers readable
 * Address Width cannot be 0x00, only 0x01-0x03
 */
boolean rf24::isAlive()
{
    byte aw;

    aw = readReg(SETUP_AW);
    return((aw & 0xFC) == 0x00 && (aw & 0x03) != 0x00);
}

/** See if the Rx FIFO has data available */
boolean rf24::rxFifoAvailable()
{
    return ((readReg(FIFO_STATUS) & (1 << RX_EMPTY)) == 0);
}

boolean rf24::txFifoEmpty()
{
    return readReg(FIFO_STATUS) & (1 << TX_EMPTY);
}


/** Return true if there is a packet available to read */
boolean rf24::available() 
{
    bool ready = ((readReg(STATUS) & (1 << RX_DR)) != 0) || rxFifoAvailable();

#if 0
    if (ready) {
	debug.print(F("Available: STATUS=0x"));
	debug.print(readReg(STATUS),HEX);
	debug.print(F(" FIFO_STATUS=0x"));
	debug.println(readReg(FIFO_STATUS),HEX);
    }
#endif

    return ready;
}

boolean rf24::txFull()
{

    return (readReg(STATUS) & (1 << TX_FULL)) != 0;
}

/** Return true if a packet becomes available to read within the time
 * specified by timeout.
 * @param timeout The number of milliseconds to wait for a packet
 * @returns true if packet is available to read, false if not
 */
boolean rf24::available(uint32_t timeout) 
{
    uint32_t start = millis();

    do {
	if (available()) {
	    return true;
	}
    } while ((start - millis()) < timeout);

    return false;
}

/** Set the RF channel. Sets the tx and rx frequency to 
 * 2.400 GHz + chan MHz.
 * @param chan The RF channel, 0 through maxChan (device can support up to 125).
 */
void rf24::setChannel(uint8_t chan)
{
    if (chan > maxChan) {
	chan = maxChan;
    }
    writeReg(RF_CH, chan);
    channel = chan;
}

void rf24::setMaxChannel(uint8_t chan)
{
    if (chan > 125) {
	maxChan = 125;
    } else {
	maxChan = chan;
    }
}

uint8_t rf24::getChannel(void)
{
    return readReg(RF_CH);
}


/** Enable the auto-ack feature to improve packet reliability. With this
 * feature enabled the rf24 will wait for an ACK from the receiving unit,
 * and it will resend the packet if it does not receive one.
 * @param delay How long to wait for an ACK (in microseconds).
 * @param retry How many times to retransmit a packet (max is 15).
 * @note the delay resolution is 250 microseconds, values are rounded up
 *       to the nearest multiple of 250. Max is 4000 microseconds.
 */
void rf24::enableAck(uint16_t delay, uint8_t retry)
{
    uint8_t addr[RF24_ADDR_LEN];

    if (retry > 15) {
	retry = 15;
    }
    delay = (_scrubDelay(delay) + 249) / 250;
    if (delay > 15) {
	delay = 15; /* Max 4000us */
    }

    writeReg(EN_AA, 0x3F); /* enable auto-ack */
    writeReg(SETUP_RETR, (delay << 4) | (retry & 0x0F));

    /* 
     * RX_ADDR_P0 is used for the auto ack feature, and 
     * needs to be the same as the TX address 
     */
    readReg(TX_ADDR, addr, RF24_ADDR_LEN);
    writeReg(RX_ADDR_P0, addr, RF24_ADDR_LEN);
    autoAck = true;
}

/** Private function to clamp the delay to sensible values based on the nRF24L01+'s datasheet
 *  stated limits.
 */
uint16_t rf24::_scrubDelay(uint16_t delay)
{
    /* Per datasheet for nRF24L01+, page 34 (section 7.4.2) */
    switch (rfspeed) {
        case RF24_SPEED_250KBPS:
            if (packetSize <= 8 && delay < 750) {
                delay=750;
                break;
            }
            if (packetSize <= 16 && delay < 1000) {
                delay=1000;
                break;
            }
            if (packetSize <= 24 && delay < 1250) {
                delay=1250;
                break;
            }
            if (delay < 1500) {
                delay=1500;
                break;
            }
            break;

        case RF24_SPEED_1MBPS:
            if (packetSize <= 5 && delay < 250) {
                delay=250;
            }
            break;

        case RF24_SPEED_2MBPS:
            if (packetSize <= 15 && delay < 250) {
                delay=250;
            }
            break;
    }

    return(delay);
}

/** Disable the auto-ack feature */
void rf24::disableAck()
{
    writeReg(EN_AA, 0);
    writeReg(SETUP_RETR, 0);
    autoAck = false;
}

boolean rf24::sendAndRead(void *msg, uint8_t size, uint32_t timeout)
{
    send(msg);
    while (isSending());
    if (acked) {
	/* want a response */
	if (available(timeout)) {
	    read(msg, size);
	    return true;
	} else {
	    return false;
	}
    }
    debug.println(F("Failed to send"));
    return false;
}

/**
 * Scan for active channels. Fills in the chans array with an estimate of
 * received signal strength for each channel.
 * The estimate is a count of how many times a >-64dBm signal was seen on the channel.
 *
 * Based on Rolf Henkel's concept and work.
 * http://arduino.cc/forum/index.php/topic,54795.0.html
 *
 * @param chans - channel array to fill with signal estimate
 * @param start - start scanning from this channel
 * @param count - scan this many channels
 * @param depth - number of samples for each channel
 */
void rf24::scan(uint8_t *chans, uint8_t start, uint8_t count, uint8_t depth)
{
    uint8_t end = start+count;
    uint8_t configuredChan = getChannel();

    if (end > 125) {
	end = 125;
    }

    chipDisable();

    memset(chans, 0, count);

    for (uint8_t rep=0; rep<depth; rep++) {
        for (uint8_t chan=start; chan < end; chan++) {
	    setChannel(chan);
	    enableRx();
	    delayMicroseconds(40);
	    chipDisable();

	    if (readReg(RF24_RPD) & 0x01) {
	        chans[chan]++;
	    }
	}
    }

    setChannel(configuredChan);
}

void rf24::loop()
{
    if (handler == NULL) return;

    if (available()) {
	read(handlerMsg, handlerMsgSize);
	handler(handlerMsg, handlerMsgSize);
    }
}

void rf24::setHandler(void (*rfHandler)(void *msg, uint8_t size), void *msg, uint8_t size)
{
    handler = rfHandler;
    handlerMsg = msg;
    handlerMsgSize = size;
}

void rf24::disableHandler()
{
    handler = NULL;
    handlerMsg = NULL;
    handlerMsgSize = 0;
}

/* Register addresses, sizes, and names for dump */
static struct {
    uint8_t reg;
    uint8_t size;
    char name[12];
} regs[] __attribute__((__progmem__)) = {
    { CONFIG,      1, "CONFIG" },
    { EN_AA,       1, "EN_AA" },
    { EN_RXADDR,   1, "EN_RXADDR" },
    { SETUP_AW,    1, "SETUP_AW" },
    { SETUP_RETR,  1, "SETUP_RETR" },
    { RF_CH,       1, "RF_CH" },
    { RF_SETUP,    1, "RF_SETUP" },
    { STATUS,      1, "STATUS" },
    { OBSERVE_TX,  1, "OBSERVE_TX" },
    { RPD,         1, "RPD" },
    { RX_ADDR_P0,  5, "RX_ADDR_P0" },
    { RX_ADDR_P1,  5, "RX_ADDR_P1" },
    { RX_ADDR_P2,  1, "RX_ADDR_P2" },
    { RX_ADDR_P3,  1, "RX_ADDR_P3" },
    { RX_ADDR_P4,  1, "RX_ADDR_P4" },
    { RX_ADDR_P5,  1, "RX_ADDR_P5" },
    { TX_ADDR,     5, "TX_ADDR" },
    { RX_PW_P0,    1, "RX_PW_P0" },
    { RX_PW_P1,    1, "RX_PW_P1" },
    { RX_PW_P2,    1, "RX_PW_P2" },
    { RX_PW_P3,    1, "RX_PW_P3" },
    { RX_PW_P4,    1, "RX_PW_P4" },
    { RX_PW_P5,    1, "RX_PW_P5" },
    { FIFO_STATUS, 1, "FIFO_STATUS" }
};

void rf24::dumpRegisters(void)
{
    uint8_t ind;
    uint8_t data[5];
    uint8_t dind;
#ifdef USE_SNPRINTF
    char buf[20];
#endif

    for (ind=0; ind<(sizeof(regs)/sizeof(regs[0])); ind++) {
	uint8_t reg = pgm_read_byte(&regs[ind].reg);
	uint8_t size = pgm_read_byte(&regs[ind].size);
#ifdef USE_SNPRINTF
        snprintf(buf, sizeof(buf), "%-12S (%02x): ", regs[ind].name, reg);
	debug.print(buf);
#else
	debug.print((const __FlashStringHelper *)regs[ind].name);
	/* Pad the output to 12 spaces */
	for (dind=12-strlen_P(regs[ind].name); dind; dind--) {
	    debug.print(' ');
	}
	debug.print('(');
	if (reg < 0x10)
	    debug.print('0');
	debug.print(reg,HEX);
	debug.print(F("): "));
#endif
	readReg(reg, data, size);

	for (dind=0; dind<size; dind++) {
#ifdef USE_SNPRINTF
	    snprintf(buf, sizeof(buf), "%02x ", data[dind]);
	    debug.print(buf);
#else
	    if (data[dind] < 0x10)
		debug.print('0');
	    debug.print(data[dind],HEX);
	    debug.print(' ');
#endif
	}

	if ((reg >= RX_ADDR_P0) && (reg <= TX_ADDR)) {
	    debug.print(' ');
	    for (dind=0; dind<size; dind++) {
		if (isprint(data[dind])) {
		    debug.print((char)data[dind]);
		} else {
		    debug.print('.');
		}
	    }
	}
	debug.println();
    }
}

RFDebug::RFDebug()
{
    debug = NULL;
}

void RFDebug::begin(Print *debugPrint)
{
    debug = debugPrint;
}

size_t RFDebug::write(uint8_t data)
{
    if (debug != NULL) {
	return debug->write(data);
    }

    return 0;
}

