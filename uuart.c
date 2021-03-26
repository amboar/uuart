/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2021 IBM Corp. */

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BIT(x) (1UL << (unsigned long)(x))

#define D_VUART1		0x1e787000
#define D_VUART2		0x1e788000

#define R_RBR			0x00
#define R_THR			0x00
#define R_DLL			0x00
#define R_IER			0x04
#define   IER_ERBFI		BIT(0)
#define   IER_ETBEI		BIT(1)
#define   IER_ELSI		BIT(2)
#define   IER_EDSSI		BIT(3)
#define R_DLM			0x04
#define R_IIR			0x08
#define R_FCR			0x08
#define R_LCR			0x0c
#define R_MCR			0x10
#define R_LSR			0x14
#define   LSR_DR		BIT(0)
#define   LSR_OE		BIT(1)
#define   LSR_PE		BIT(2)
#define   LSR_FE		BIT(3)
#define   LSR_BI		BIT(4)
#define   LSR_THRE		BIT(5)
#define   LSR_TEMT		BIT(6)
#define   LSR_RFE		BIT(7)
#define R_MSR			0x18
#define R_SCR			0x1c
#define R_GCRA			0x20
#define   GCRA_H_RFT		(BIT(7) | BIT(6))
#define   GCRA_H_TX_CORK	BIT(5)
#define   GCRA_H_LOOP		BIT(4)
#define   GCRA_S_TIMEOUT	(BIT(3) | BIT(2))
#define   GCRA_SIRQ_POL		BIT(1)
#define   GCRA_VUART_EN		BIT(0)
#define R_GCRB			0x24
#define R_VARL			0x28
#define R_VARH			0x2c
#define R_GCRE			0x30
#define R_GCRF			0x34
#define R_GCRG			0x38
#define R_GCRH			0x3c

#ifdef __ARM_ARCH
#define mb() asm volatile("dmb 3\n" : : : "memory")
#else
#error Unsupported host architecture!
#endif

static uint8_t readb(const volatile void *regs, unsigned long offset)
{
	uint8_t val;

	val = ((const volatile uint8_t *)regs)[offset];
	mb();
	return val;
}

static void writeb(volatile void *regs, unsigned long offset, uint8_t val)
{
	((volatile uint8_t *)regs)[offset] = val;
	mb();
}

static void dump_regs(const volatile void *regs)
{
	fprintf(stderr, "\tIER:\t0x%02x\n", readb(regs, R_IER));
	fprintf(stderr, "\tIIR:\t0x%02x\n", readb(regs, R_IIR));
	fprintf(stderr, "\tLCR:\t0x%02x\n", readb(regs, R_LCR));
	fprintf(stderr, "\tMCR:\t0x%02x\n", readb(regs, R_MCR));
	fprintf(stderr, "\tLSR:\t0x%02x\n", readb(regs, R_LSR));
	fprintf(stderr, "\tMSR:\t0x%02x\n", readb(regs, R_MSR));
	fprintf(stderr, "\tGCRA:\t0x%02x\n", readb(regs, R_GCRA));
	fprintf(stderr, "\tGCRB:\t0x%02x\n", readb(regs, R_GCRB));
	fprintf(stderr, "\tVARL:\t0x%02x\n", readb(regs, R_VARL));
	fprintf(stderr, "\tVARH:\t0x%02x\n", readb(regs, R_VARH));
	fprintf(stderr, "\tGCRE:\t0x%02x\n", readb(regs, R_GCRE));
	fprintf(stderr, "\tGCRF:\t0x%02x\n", readb(regs, R_GCRF));
	fprintf(stderr, "\tGCRG:\t0x%02x\n", readb(regs, R_GCRG));
	fprintf(stderr, "\tGCRH:\t0x%02x\n", readb(regs, R_GCRH));
}

struct uuart_config {
	bool assume_dtr;
	bool assume_enabled;
	bool assume_fifos;
	bool ignore_rx;
	bool ignore_tx;
};

static const char help_text[] =
"%s: Userspace UART driver\n"
"\n"
"-D, --assume-dtr\n"
"\tAssume MCR[DTR] and MCR[RTS] are set appropriately\n"
"\n"
"-E, --assume-enabled\n"
"\tAssume the UART is enabled and configured to not drain the Rx FIFO\n"
"\n"
"-F, --assume-fifos\n"
"\tAssume the FIFOs are configured and do not need resetting\n"
"\n"
"-h, --help\n"
"\tHelp!\n"
"\n"
"-R, --ignore-rx\n"
"\tIgnore LSR[DR] and do not read RBR\n"
"\n"
"-T, --ignore-tx\n"
"\tIgnore LSR[THRE] and do not write THR\n";

int main(int argc, char * const argv[])
{
	unsigned long txd = 0, rxd = 0;
	struct uuart_config cfg = {0};
	volatile void *regs;
	uint8_t lsr, ier;
	bool stall;
	int iters;
	int fd;
	int o;

	while (1) {
		static struct option long_options [] = {
			{ "assume-dtr",     no_argument, NULL, 'D' },
			{ "assume-enabled", no_argument, NULL, 'E' },
			{ "assume-fifos",   no_argument, NULL, 'F' },
			{ "help",           no_argument, NULL, 'h' },
			{ "ignore-rx",      no_argument, NULL, 'R' },
			{ "ignore-tx",      no_argument, NULL, 'T' },
			{ NULL,             0,           NULL,  0  },
		};
		int oi = 0;

		o = getopt_long(argc, argv, "DEFhRT", long_options, &oi);
		if (o == -1)
			break;

		if (o == 'D')
			cfg.assume_dtr = true;
		else if (o == 'E')
			cfg.assume_enabled = true;
		else if (o == 'F')
			cfg.assume_fifos = true;
		else if (o == 'h')
			errx(EXIT_SUCCESS, help_text, argv[0]);
		else if (o == 'R')
			cfg.ignore_rx = true;
		else if (o == 'T')
			cfg.ignore_tx = true;
		else
			errx(EXIT_FAILURE, "Unexpected option: %c", o);
	}

	fd = open("/dev/mem", O_SYNC | O_RDWR);
	if (fd == -1)
		err(EXIT_FAILURE, "open");

	regs = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, D_VUART2);
	if (!regs)
		err(EXIT_FAILURE, "mmap");

	fprintf(stderr, "Startup configuration\n");
	dump_regs(regs);

	assert(optind <= argc);
	if (optind == argc)
		exit(EXIT_SUCCESS);

	/* Enable the VUART */
	if (!cfg.assume_enabled)
		writeb(regs, R_GCRA, GCRA_VUART_EN | GCRA_H_TX_CORK);

	/* Configure IER */
	ier = readb(regs, R_IER);
	if (!cfg.ignore_tx)
		ier &= ~IER_ETBEI;
	if (!cfg.ignore_rx)
		ier &= ~IER_ERBFI;
	if (!(ier & (IER_ETBEI | IER_ERBFI)))
		ier = 0;
	writeb(regs, R_IER, ier);

	/* Reset and enable the FIFOs */
	if (!cfg.assume_fifos)
		writeb(regs, R_FCR, 0x07);

	/* Indicate we're ready */
	if (!cfg.assume_dtr)
		writeb(regs, R_MCR, 0x0b);

	fprintf(stderr, "Initialised configuration\n");
	dump_regs(regs);

	stall = false;
	iters = atoi(argv[optind]);
	fprintf(stderr, "Running for %d iterations\n", iters);
	for (int i = 0; iters < 0 || i < iters; i += (iters > 0)) {
		lsr = readb(regs, R_LSR);

		if ((lsr & LSR_DR) || (lsr & LSR_THRE)) {
			if (stall) {
				struct timespec ts;
				int rc;

				rc = clock_gettime(CLOCK_BOOTTIME, &ts);
				if (rc)
					err(EXIT_FAILURE, "clock_gettime");

				fprintf(stderr,
					"[%7ld.%06ld] VUART resumed at %d, LSR: 0x%02x\n",
					ts.tv_sec, ts.tv_nsec / 1000, i, lsr);
			}
			stall = false;
		} else {
			if (!stall) {
				struct timespec ts;
				int rc;

				rc = clock_gettime(CLOCK_BOOTTIME, &ts);
				if (rc)
					err(EXIT_FAILURE, "clock_gettime");

				fprintf(stderr,
					"[%7ld.%06ld] VUART stalled at %d, LSR: 0x%02x\n",
					ts.tv_sec, ts.tv_nsec / 1000, i, lsr);
			}
			stall = true;
		}

		if (!cfg.ignore_tx && (lsr & LSR_THRE)) {
			writeb(regs, R_THR, 'y');
			txd++;
		}

		if (!cfg.ignore_rx && (lsr & LSR_DR)) {
			putchar(readb(regs, R_RBR));
			rxd++;
		}
	}

	fprintf(stderr, "Terminating configuration\n");
	dump_regs(regs);

	if (!cfg.ignore_tx)
		fprintf(stderr, "Transmitted:\t%lu\n", txd);

	if (!cfg.ignore_rx)
		fprintf(stderr, "Received:\t%lu\n", rxd);

	exit(EXIT_SUCCESS);
}
