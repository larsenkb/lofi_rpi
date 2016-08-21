/*
 * lofi_rx.c:
 *	Receive and process lofi security packets.
 *	on odroid c1+  - spi not working correctly so I added a bit-bang spi
 *	when using bit-bang spi don't add the following modules, otherwise do add them...
 *	spicc
 *	spidev
 *	aml_i2c
 *
 */

#include <sys/signalfd.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>

#define EN_ENH_SWAVE	1
#define nrfIrq		4
#define nrfCSN		10
#define nrfCE		6
#define SPI_BIT_BANG	1
#if SPI_BIT_BANG
#define MOSI_PIN	12
#define MISO_PIN	13
#define SCLK_PIN	14
#endif

#define MAX_NODES		20

#define handle_error(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define NRF_CONFIG			0x00
#define NRF_EN_AA			0x01
#define NRF_EN_RXADDR		0x02
#define NRF_SETUP_AW		0x03
#define NRF_SETUP_RETR		0x04
#define NRF_RF_CH			0x05
#define NRF_RF_SETUP		0x06
#define NRF_STATUS			0x07
#define NRF_OBSERVE_TX		0x08
#define NRF_CD				0x09
#define NRF_RX_ADDR_P0		0x0A
#define NRF_RX_ADDR_P1		0x0B
#define NRF_RX_ADDR_P2		0x0C
#define NRF_RX_ADDR_P3		0x0D
#define NRF_RX_ADDR_P4		0x0E
#define NRF_RX_ADDR_P5		0x0F
#define NRF_TX_ADDR			0x10
#define NRF_RX_PW_P0		0x11
#define NRF_RX_PW_P1		0x12
#define NRF_RX_PW_P2		0x13
#define NRF_RX_PW_P3		0x14
#define NRF_RX_PW_P4		0x15
#define NRF_RX_PW_P5		0x16
#define NRF_FIFO_STATUS		0x17
#define NRF_DYNPD			0x1C
#define NRF_FEATURE			0x1D


typedef enum {
	SENID_NONE = 0,
	SENID_CTR,
	SENID_SW1,
	SENID_VCC,
	SENID_TEMP,
	SENID_SW2
} senId_t;

typedef struct {
	int	online;
	int ctr;
} node_t;

node_t nodes[MAX_NODES];

unsigned char payload[8];
int longStr = 0;
int printPayload = 0;
char *pgmName = NULL;
int speed_1M = 0;
int rf_chan = 2;
//static int mainThreadPid;
#if !SPI_BIT_BANG
static int spiFd;
#endif



//************  Forward Declarations
int parse_payload( uint8_t *payload );
void spiSetup( int speed );
int spiXfer( uint8_t *buf, int cnt );
uint8_t nrfRegRead( int reg );
int nrfRegWrite( int reg, int val );
void nrfPrintDetails(void);
int nrfAvailable( uint8_t *pipe_num );
int nrfRead( uint8_t *payload, int len );
int nrfFlushTx( void );
int nrfFlushRx( void );
int nrfAddrRead( uint8_t reg, uint8_t *buf, int len );
uint8_t nrfReadRxPayloadLen(void);


void nrfIntrHandler(void)
{
	uint8_t pipeNum;
	uint8_t payLen __attribute__ ((unused));

//	printf("ISR"); 
	while (nrfAvailable(&pipeNum)) {
//		nrfRegRead(RX_PW_P0
		payLen = nrfReadRxPayloadLen();
//		printf(" pipeNum: %d  payLen: %d\n", pipeNum, payLen);fflush(stdout);

		nrfRead( payload, 8 );
		parse_payload( payload );
	}
//	sigqueue(mainThreadPid, SIGQUIT, (const union sigval)0);
}

int Usage(void)
{
	fprintf(stderr, "Usage: %s [-l] [-p] [-c chan] [-s]\n", pgmName);
	return 0;
}


/*
 * main
 */
int main(int argc, char *argv[])
{
//	sigset_t mask;
//	int sfd;
//	struct signalfd_siginfo fdsi;
//	ssize_t s;
//	uint8_t spiBuf[16];
	uint8_t val8 __attribute__ ((unused));
//	int rv;
	int opt;

	pgmName = argv[0];

	while ((opt = getopt(argc, argv, "lpsc:")) != -1) {
		switch (opt) {
		case 'l':
			longStr = 1;
			break;
		case 'p':
			printPayload = 1;
			break;
		case 's':
			speed_1M = 1;
			break;
		case 'c':
			rf_chan = atoi(optarg);
			break;
		default:
			Usage();
			exit(-1);
			break;
		}
	}


	wiringPiSetup();

	pinMode(nrfCSN, OUTPUT);
    digitalWrite(nrfCSN, HIGH);
	pinMode(nrfCE, OUTPUT);
    digitalWrite(nrfCE, LOW);

#if SPI_BIT_BANG
    printf("BIT BANG SPI\n");
	pinMode(MOSI_PIN, OUTPUT);
	digitalWrite(MOSI_PIN, LOW);
	pinMode(SCLK_PIN, OUTPUT);
	digitalWrite(SCLK_PIN, LOW);
	pinMode(MISO_PIN, INPUT);
#else
    spiSetup(1000000);
#endif

	// NRF setup
	// enable 8-bit CRC; mask TX_DS and MAX_RT
	nrfRegWrite( NRF_CONFIG, 0x38 );

#if EN_ENH_SWAVE
	// set nbr of retries and delay
	// only needed for PTX???
	nrfRegWrite( NRF_SETUP_RETR, 0x5F );

	// enable auto ack
	nrfRegWrite( NRF_EN_AA, 3 );
#else
	nrfRegWrite( NRF_SETUP_RETR, 0 );
	nrfRegWrite( NRF_EN_AA, 0 );
#endif

	// Disable dynamic payload
	nrfRegWrite( NRF_FEATURE, 0);
	nrfRegWrite( NRF_DYNPD, 0);

	// Reset STATUS
	nrfRegWrite( NRF_STATUS, 0x70 );

	nrfRegWrite( NRF_EN_RXADDR, 3 );
	nrfRegWrite( NRF_RX_PW_P0, 8 );
#if 1
	nrfRegWrite( NRF_RX_PW_P1, 8 );
	nrfRegWrite( NRF_RX_PW_P2, 0 );
	nrfRegWrite( NRF_RX_PW_P3, 0 );
	nrfRegWrite( NRF_RX_PW_P4, 0 );
	nrfRegWrite( NRF_RX_PW_P5, 0 );
#else
	nrfRegWrite( NRF_RX_PW_P1, 8 );
	nrfRegWrite( NRF_RX_PW_P2, 8 );
	nrfRegWrite( NRF_RX_PW_P3, 8 );
	nrfRegWrite( NRF_RX_PW_P4, 8 );
	nrfRegWrite( NRF_RX_PW_P5, 8 );
#endif

	// Set up channel
	nrfRegWrite( NRF_RF_CH, rf_chan );

#if 1
	if (speed_1M) {
		nrfRegWrite( NRF_RF_SETUP, 0x06 );
	} else {
		nrfRegWrite( NRF_RF_SETUP, 0x0e );
	}
#else
	val8 = nrfRegRead( NRF_RF_SETUP );
	val8 &= ~0x28;
	if (speed_1M) {
	} else {
		nrfRegWrite( NRF_RF_SETUP, val8 | 0x08 );
	}
#endif

	nrfFlushTx();
	nrfFlushRx();

	wiringPiISR(nrfIrq, INT_EDGE_FALLING, &nrfIntrHandler);

	// Power up radio and delay 5ms
	nrfRegWrite( NRF_CONFIG, nrfRegRead( NRF_CONFIG ) | 0x02 );
	delay(5);

	// Enable PRIME RX (PRX)
	nrfRegWrite( NRF_CONFIG, nrfRegRead( NRF_CONFIG ) | 0x01 );

    digitalWrite(nrfCE, HIGH);

//	nrfRegWrite( NRF_EN_RXADDR, 3 );

	nrfPrintDetails();

#if 0
	sigemptyset(&mask);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGUSR1);

	// Block signals so that they aren't handled
	// according to their default dispositions
	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
		handle_error("sigprocmask");

	sfd = signalfd(-1, &mask, 0);

	mainThreadPid = pthread_self();

	if (sfd == -1)
		handle_error("signalfd");
#endif

	for (;;) {
		delay(10000);
#if 0
		s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
		if (s != sizeof(struct signalfd_siginfo)) {
			handle_error("read");
		}
printf("s = %d\n", s); fflush(stdout);

		if (fdsi.ssi_signo == SIGUSR1) {
			printf("Got SIGUSR1\n");
			fflush(stdout);
		} else if (fdsi.ssi_signo == SIGQUIT) {
			printf("Got SIGQUIT\n"); fflush(stdout);
		} else {
			printf("Read unexpected signal\n"); fflush(stdout);
		}
#endif
	}
#if 0
	for (;;) {

#if 0
		while (nrfAvailable(0)) {
			nrfRead( payload, 8 );
			parse_payload( payload );
		}
#endif
		usleep(50000);
	}
#endif

#if 0
#if 1
	uint8_t nrfConfigReg;
	nrfConfigReg = nrfRegRead(0);
	printf("CONFIG: %02X\n", nrfConfigReg);
#else
	spiBuf[0] = 0;
	spiBuf[1] = 0;

	rv = spiXfer(spiBuf, 2);
	if (rv < 0) {
		printf("spiXfer error\n");
	}
	printf("[0]:%02X  [1]:%02X\n", spiBuf[0], spiBuf[1]);
#endif
#endif

	return 0;


#if 0

	if (sfd == -1)
		handle_error("signalfd");

	for (;;) {
		s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
		if (s != sizeof(struct signalfd_siginfo)) {
			handle_error("read");
		}
printf("s = %d\n", s); fflush(stdout);

		if (fdsi.ssi_signo == SIGUSR1) {
			printf("Got SIGUSR1\n");
			fflush(stdout);
		} else if (fdsi.ssi_signo == SIGQUIT) {
			printf("Got SIGQUIT\n"); fflush(stdout);
		} else {
			printf("Read unexpected signal\n"); fflush(stdout);
		}
	}
#endif
  return 0;
}

int parse_payload( uint8_t *payload )
{
	struct timespec ts;
	int i;
	unsigned short val;
	uint8_t	sensorId;
	uint8_t nodeId;
	char tbuf[128];
	char sbuf[80];
	int		tbufIdx = 0;


	tbuf[0] = '\0';
	sbuf[0] = '\0';
	if (printPayload) {
		tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "Payload: %02X %02X %02X %02X %02X %02X %02X %02X\n",
			payload[0], payload[1], payload[2], payload[3],
			payload[4], payload[5], payload[6], payload[7]);
	}
			
	clock_gettime(CLOCK_REALTIME, &ts);
	nodeId = payload[0];

	if (nodeId >= MAX_NODES) {
		tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "Bad nodeId: %d\n", nodeId);
		// probably want to print this and exit and not continue parsing...
	}

	if (longStr)
		tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "%d NodeId: %2d", (int)ts.tv_sec, nodeId);
	i = 1;

	while ((sensorId = (payload[i]>>3) & 0x1F) != 0) {
		switch (sensorId) {
		case SENID_CTR:
			val = payload[i++] & 0x03;
			val <<= 8;
			val += payload[i++];
			if (nodes[nodeId].online) {
				if (val != ((nodes[nodeId].ctr + 1) & 0x3ff)) {
					sprintf(sbuf, "  Skipped ctr: was: %d  is: %d", nodes[nodeId].ctr, val);
				}
			}
			nodes[nodeId].online = 1;
			nodes[nodeId].ctr = val;
			if (longStr)
				tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "  Ctr: %4d", val);
			else
				printf("%d NodeId: %2d  Ctr: %4d\n", (unsigned int)ts.tv_sec, nodeId, val);
			break;
		case SENID_SW1:
//			if (payload[i] & 0x01)
//				printf(" toggled");
			if (longStr)
				tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "  SW1 %2d: %s", sensorId, (payload[i++] & 0x02) ? "OPEN  " : "CLOSED");
			else
				printf("%d NodeId: %2d  SW1 %2d: %s", (unsigned int)ts.tv_sec, nodeId, sensorId,  (payload[i++] & 0x02) ? " OPEN\n" : " CLOSED\n");
			break;
		case SENID_SW2:
//			if (payload[i] & 0x01)
//				printf(" toggled");
			if (longStr)
				tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "  SW2 %2d: %s", sensorId, (payload[i++] & 0x02) ? "OPEN  " : "CLOSED");
			else
				printf("%d NodeId: %2d  SW2 %2d: %s", (unsigned int)ts.tv_sec, nodeId, sensorId,  (payload[i++] & 0x02) ? " OPEN\n" : " CLOSED\n");
			break;
		case SENID_VCC:
			val = payload[i++] & 0x03;
			val <<= 8;
			val += payload[i++];
			if (longStr)
				tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "  Vcc: %4.2f",(1.1 * 1024.0)/(float)val);
			else
				printf("%d NodeId: %2d  Vcc: %4.2f\n", (unsigned int)ts.tv_sec, nodeId, (1.1 * 1024.0)/(float)val);
			break;
		case SENID_TEMP:
			val = payload[i++] & 0x03;
			val <<= 8;
			val += payload[i++];
			if (longStr)
				tbufIdx += snprintf(&tbuf[tbufIdx], 127-tbufIdx, "  Temp: %4.2f",1.0 * (float)val - 260.0);
			else
				printf("%d NodeId: %2d  Vcc: %4.2f\n", (unsigned int)ts.tv_sec, nodeId, 1.0 * (float)val - 260.0);
			break;
		default:
			break;
		}
	}

//	printf(" %d\n", radio.testRPD());
	if (longStr) {
		printf("%s", tbuf);
		if (sbuf[0] != '\0') printf("%s", sbuf);
		printf("\n");
	} else
		printf("\n");
				
	fflush(stdout);
	return 0;
}


#if 0
void spiSetup(int speed)
{
	if ((spiFd = wiringPiSPISetup(0, speed)) < 0) {
		fprintf(stderr, "Can't open the SPI bus: %s\n", strerror (errno));
		exit (EXIT_FAILURE);
	}
}
#endif

#if SPI_BIT_BANG
int spiXfer(uint8_t *buf, int cnt)
{
	uint8_t tmpOut, tmpIn;
	int i;

	digitalWrite(nrfCSN, LOW);
	while (cnt--) {
		tmpIn = 0;
		tmpOut = *buf;
		for (i = 0; i < 8; i++) {
			tmpIn<<=1;
			// write MOSI
			if (tmpOut & (1<<(7-i)))
				digitalWrite(MOSI_PIN, HIGH);
			else
				digitalWrite(MOSI_PIN, LOW);
			// write SCLK
			digitalWrite(SCLK_PIN, HIGH);
			// read MISO
			if (digitalRead(MISO_PIN) == HIGH)
				tmpIn |= 1;
			digitalWrite(SCLK_PIN, LOW);
		}
		*buf++ = tmpIn;;
	}
	digitalWrite(nrfCSN, LOW);
	digitalWrite(nrfCSN, LOW);
	digitalWrite(nrfCSN, HIGH);
	return 0;
}
#else
int spiXfer(uint8_t *buf, int cnt)
{
	int rv;

    digitalWrite(nrfCSN, LOW);
	rv = wiringPiSPIDataRW(0, buf, cnt);
    digitalWrite(nrfCSN, HIGH);
	if (rv == -1) {
		fprintf(stderr, "Error in spiXfer: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}
#endif

int nrfAddrRead( uint8_t reg, uint8_t *buf, int len )
{
	if (buf && len > 1) {
		buf[0] = reg & 0x1f;
		spiXfer(buf, len+1);
		return buf[1];
	}
	return -1;
}


int nrfFlushRx( void )
{
	uint8_t spiBuf[1];

	spiBuf[0] = 0xe2;
	return spiXfer(spiBuf, 1);
}

int nrfFlushTx( void )
{
	uint8_t spiBuf[1];

	spiBuf[0] = 0xe1;
	return spiXfer(spiBuf, 1);
}

int nrfRegWrite( int reg, int val)
{
	uint8_t spiBuf[2];

	spiBuf[0] = 0x20 | (reg & 0x1f);
	spiBuf[1] = val;
	return spiXfer(spiBuf, 2);
}

uint8_t nrfRegRead( int reg )
{
	uint8_t spiBuf[2];

	spiBuf[0] = reg & 0x1f;
	spiBuf[1] = 0;
	spiXfer(spiBuf, 2);
	return spiBuf[1];
}

uint8_t nrfReadRxPayloadLen(void)
{
	uint8_t spiBuf[2];

	spiBuf[0] = 0x60;
	spiBuf[1] = 0;
	spiXfer(spiBuf, 2);
	return spiBuf[1];
}

int nrfAvailable( uint8_t *pipe_num )
{
	uint8_t status;

	status = nrfRegRead( NRF_STATUS );
	if (status & 0x40 ) {
		if ( pipe_num ) {
			*pipe_num = ((status>>1) & 0x7);
		}
		return 1;
	}
	return 0;
}

int nrfRead( uint8_t *payload, int len )
{
	uint8_t spiBuf[33];
	int i;

	if (len > 32)
		return -1;
	if (len < 1)
		return -1;

	spiBuf[0] = 0x61;
	for (i = 1; i < len+1; i++)
		spiBuf[i] = 0;
	spiXfer(spiBuf, len+1);
	if (payload)
		for (i = 1; i < len+1; i++)
			payload[i-1] = spiBuf[i];
	
	nrfRegWrite( NRF_STATUS, 0x40 );

	return 0;
}


void nrfPrintDetails(void)
{
	uint8_t		buf[6];

	printf("================ SPI Configuration ================\n" );
	printf("CSN Pin  \t = Custom GPIO%d\n", nrfCSN  );
	printf("CE Pin  \t = Custom GPIO%d\n", nrfCE );
	printf("Clock Speed\t = " );
	printf("1 Mhz");
	printf("\n================ NRF Configuration ================\n");
 

	printf("STATUS: %02X\n", nrfRegRead( NRF_STATUS ));
	nrfAddrRead( NRF_RX_ADDR_P0, buf, 5 );
	printf("RX_ADDR_P0: %02X%02X%02X%02X%02X\n", buf[1], buf[2], buf[3], buf[4], buf[5]);
//	printf("RX_ADDR_P0: %02X\n", nrfRegRead( NRF_RX_ADDR_P0 ));
	nrfAddrRead( NRF_RX_ADDR_P1, buf, 5 );
	printf("RX_ADDR_P1: %02X%02X%02X%02X%02X\n", buf[1], buf[2], buf[3], buf[4], buf[5]);
//	printf("RX_ADDR_P1: %02X\n", nrfRegRead( NRF_RX_ADDR_P1 ));
	printf("RX_ADDR_P2: %02X\n", nrfRegRead( NRF_RX_ADDR_P2 ));
	printf("RX_ADDR_P3: %02X\n", nrfRegRead( NRF_RX_ADDR_P3 ));
	printf("RX_ADDR_P4: %02X\n", nrfRegRead( NRF_RX_ADDR_P4 ));
	printf("RX_ADDR_P5: %02X\n", nrfRegRead( NRF_RX_ADDR_P5 ));
//	printf("TX_ADDR: %02X\n", nrfRegRead( NRF_TX_ADDR ));
	nrfAddrRead( NRF_TX_ADDR, buf, 5 );
	printf("TX_ADDR: %02X%02X%02X%02X%02X\n", buf[1], buf[2], buf[3], buf[4], buf[5]);

//  print_byte_register(PSTR("RX_PW_P0-6"),RX_PW_P0,6);
  printf("EN_AA: %02X\n", nrfRegRead( NRF_EN_AA ));
  printf("EN_RXADDR: %02X\n", nrfRegRead( NRF_EN_RXADDR ));
  printf("RF_CH: %02X\n", nrfRegRead( NRF_RF_CH ));
  printf("RF_SETUP: %02X\n", nrfRegRead( NRF_RF_SETUP ));
  printf("RX_PW_P0: %02X\n", nrfRegRead( NRF_RX_PW_P0 ));
  printf("RX_PW_P1: %02X\n", nrfRegRead( NRF_RX_PW_P1 ));
  printf("RX_PW_P2: %02X\n", nrfRegRead( NRF_RX_PW_P2 ));
  printf("RX_PW_P3: %02X\n", nrfRegRead( NRF_RX_PW_P3 ));
  printf("RX_PW_P4: %02X\n", nrfRegRead( NRF_RX_PW_P4 ));
  printf("RX_PW_P5: %02X\n", nrfRegRead( NRF_RX_PW_P5 ));
  printf("CONFIG: %02X\n", nrfRegRead( NRF_CONFIG ));
  printf("CD: %02X\n", nrfRegRead( NRF_CD ));
  printf("SETUP_AW: %02X\n", nrfRegRead( NRF_SETUP_AW ));
  printf("SETUP_RETR: %02X\n", nrfRegRead( NRF_SETUP_RETR ));
  printf("DYNPD: %02X\n", nrfRegRead( NRF_DYNPD ));
  printf("FEATURE: %02X\n", nrfRegRead( NRF_FEATURE ));

#if 1
  if (speed_1M)
	printf("Data Rate\t = %s\n", "1Mbps" );
  else
	printf("Data Rate\t = %s\n", "2Mbps" );
  printf("Model\t\t = %s\n", "nRF24L01+"  );
  printf("CRC Length\t = %s\n", "8 bits");
  printf("PA Power\t = %s\n", "PA_MAX" );
#endif
  fflush(stdout);

}





#if 0
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#define handle_error(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)

int main (int argc, char *argv[])
{
	sigset_t mask;
	int sfd;
	struct signalfd_siginfo fdsi;
	ssize_t s;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);

	// Block signals so that they aren't handled
	// according to their default dispositions
	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
		handle_error("sigprocmask");

	sfd = signalfd(-1, &mask, 0);
	if (sfd == -1)
		handle_error("signalfd");

	for (;;) {
		s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
		if (s != sizeof(struct signalfd_siginfo))
			handle_error("read");

		if (fdsi.ssi_signo == SIGINT) {
			printf("Got SIGINT\n");
		} else if (fdsi.ssi_signo == SIGQUIT) {
			printf("Got SIGQUIT\n");
			exit(EXIT_SUCCESS);
		} else {
			printf("Read unexpected signal\n");
		}
	}
	return 0;
}
#endif

