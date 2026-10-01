/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * irq-unclaimed: the kernel masks an interrupt that no handler claims
 *
 * The AArch64 GIC dispatcher masks an SPI after UNCLAIMED_LIMIT deliveries in
 * a row in which every handler declined it (returned < 0) and the line was
 * still pending afterwards -- the signature of a level line whose owner died.
 * This drives that path on an SPI no device is wired to, using the
 * distributor's software pend (GICD_ISPENDR):
 *
 *   A  the handler declines, the line is released (pended once per delivery
 *      from here, waiting for each delivery to end): 2 x LIMIT deliveries,
 *      the IRQ must stay enabled;
 *   B  the handler claims and re-pends the IRQ from inside itself, so the line
 *      is "held" for 2 x LIMIT deliveries: it must stay enabled;
 *   C  the handler declines and re-pends itself (a stuck level line nobody
 *      services): the kernel must mask the IRQ after exactly LIMIT deliveries
 *      and print "interrupts: IRQ <n> unclaimed <LIMIT> times in a row, masked";
 *   D  registering a second handler must enable the IRQ again.
 *
 * A software pend is consumed when the interrupt is acknowledged, so it works
 * like one edge; re-pending from the handler is what makes a held line. The
 * handler stops re-pending after a budget, so on a kernel without the guard
 * phase C ends (FAIL) instead of hanging the board.
 *
 * Copyright 2026 Phoenix Systems
 *
 * %LICENSE%
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/interrupt.h>
#include <sys/mman.h>
#include <sys/threads.h>


/* Must match UNCLAIMED_LIMIT in hal/aarch64/interrupts_gicv2.c */
#define UNCLAIMED_LIMIT 100000u

/* BCM2711 GIC-400 distributor, and an SPI in the GIC's range that no device
 * drives (the ETH_PCIe block ends at ID 216). */
#define DEFAULT_GICD 0xff841000u
#define DEFAULT_IRQ  223u

#define GICD_TYPER     0x004u
#define GICD_ISENABLER 0x100u
#define GICD_ISPENDR   0x200u
#define GICD_ICPENDR   0x280u
#define GICD_ISACTIVER 0x300u

#define WAIT_MS       2000
#define WAIT_STORM_MS 20000


enum { mode_decline, mode_claim, mode_decline_held };


static struct {
	volatile uint32_t *gicd;
	unsigned int irq;
	volatile int mode;
	volatile unsigned int fired;
	volatile unsigned int fired2;
	volatile unsigned int budget; /* re-pends left in the held modes */
} common;


static unsigned int gicd_bit(uint32_t reg)
{
	return (common.gicd[(reg / 4u) + (common.irq / 32u)] >> (common.irq % 32u)) & 1u;
}


static void gicd_setBit(uint32_t reg)
{
	common.gicd[(reg / 4u) + (common.irq / 32u)] = 1u << (common.irq % 32u);
}


/* Runs in interrupt context: touches only resident globals and the GIC. */
static int irq_handler(unsigned int n, void *arg)
{
	(void)n;
	(void)arg;

	common.fired++;
	if ((common.mode != mode_decline) && (common.budget != 0u)) {
		common.budget--;
		gicd_setBit(GICD_ISPENDR);
	}

	return (common.mode == mode_claim) ? 0 : -1;
}


static int irq_handler2(unsigned int n, void *arg)
{
	(void)n;
	(void)arg;

	common.fired2++;

	return 0;
}


static long long now_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


/* Waits until the handler count is `fired` and the delivery has ended (EOI
 * done: not active, and not pending unless the IRQ is disabled). */
static int wait_idle(volatile unsigned int *counter, unsigned int fired, long long ms)
{
	long long deadline = 0;
	unsigned int spins;

	for (spins = 0;; spins++) {
		if ((*counter == fired) && (gicd_bit(GICD_ISACTIVER) == 0u) &&
				((gicd_bit(GICD_ISPENDR) == 0u) || (gicd_bit(GICD_ISENABLER) == 0u))) {
			return 0;
		}
		if ((spins % 1024u) == 0u) {
			if (deadline == 0) {
				deadline = now_ms() + ms;
			}
			else if (now_ms() > deadline) {
				return -1;
			}
		}
	}
}


/* Waits until the IRQ is masked, or the handler stops being called */
static void wait_masked_or_stalled(void)
{
	unsigned int last;

	do {
		last = common.fired;
		usleep(100 * 1000);
	} while ((gicd_bit(GICD_ISENABLER) != 0u) && (common.fired != last));

	usleep(100 * 1000);
}


static void usage(const char *prog)
{
	printf("usage: %s [-i irq] [-g gicd_phys]\n", prog);
}


int main(int argc, char *argv[])
{
	unsigned long gicdPhys = DEFAULT_GICD;
	handle_t h1, h2;
	int haveH2 = 0;
	const char *fail = NULL;
	unsigned int i, nirqs, fired;
	void *page;
	int c, err;

	common.irq = DEFAULT_IRQ;
	while ((c = getopt(argc, argv, "i:g:h")) != -1) {
		switch (c) {
			case 'i':
				common.irq = (unsigned int)strtoul(optarg, NULL, 0);
				break;
			case 'g':
				gicdPhys = strtoul(optarg, NULL, 0);
				break;
			default:
				usage(argv[0]);
				return (c == 'h') ? 0 : 2;
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	page = mmap(NULL, _PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_DEVICE | MAP_UNCACHED | MAP_PHYSMEM | MAP_ANONYMOUS, -1, (off_t)gicdPhys);
	if (page == MAP_FAILED) {
		printf("IRQ-UNCLAIMED: SKIP cannot map the distributor at 0x%lx\n", gicdPhys);
		return 2;
	}
	common.gicd = page;

	nirqs = 32u * ((common.gicd[GICD_TYPER / 4u] & 0x1fu) + 1u);
	printf("irq-unclaimed: IRQ %u (GIC has %u), limit %u\n", common.irq, nirqs, UNCLAIMED_LIMIT);
	if ((common.irq < 32u) || (common.irq >= nirqs)) {
		printf("IRQ-UNCLAIMED: SKIP IRQ %u is not an implemented SPI\n", common.irq);
		return 2;
	}
	if ((gicd_bit(GICD_ISENABLER) != 0u) || (gicd_bit(GICD_ISPENDR) != 0u) || (gicd_bit(GICD_ISACTIVER) != 0u)) {
		printf("IRQ-UNCLAIMED: SKIP IRQ %u is in use (enabled=%u pending=%u active=%u)\n", common.irq,
			gicd_bit(GICD_ISENABLER), gicd_bit(GICD_ISPENDR), gicd_bit(GICD_ISACTIVER));
		return 2;
	}

	/* Fault in the handler's code and data before it can run in interrupt context */
	common.mode = mode_decline;
	common.budget = 0;
	(void)irq_handler(common.irq, NULL);
	(void)irq_handler2(common.irq, NULL);
	common.fired = 0;
	common.fired2 = 0;

	err = interrupt(common.irq, irq_handler, NULL, 0, &h1);
	if (err < 0) {
		printf("IRQ-UNCLAIMED: SKIP interrupt(%u) failed: %d\n", common.irq, err);
		return 2;
	}

	do {
		/* A: declined, line released after each delivery */
		for (i = 0; i < 2u * UNCLAIMED_LIMIT; i++) {
			gicd_setBit(GICD_ISPENDR);
			if (wait_idle(&common.fired, i + 1u, WAIT_MS) < 0) {
				break;
			}
		}
		printf("irq-unclaimed: A declined+released: fired=%u enabled=%u\n", common.fired, gicd_bit(GICD_ISENABLER));
		if ((common.fired != 2u * UNCLAIMED_LIMIT) || (gicd_bit(GICD_ISENABLER) == 0u)) {
			fail = "A (declined deliveries that released the line masked it, or were lost)";
			break;
		}

		/* B: claimed, line held */
		fired = common.fired;
		common.budget = 2u * UNCLAIMED_LIMIT - 1u;
		common.mode = mode_claim;
		gicd_setBit(GICD_ISPENDR);
		(void)wait_idle(&common.fired, fired + 2u * UNCLAIMED_LIMIT, WAIT_STORM_MS);
		printf("irq-unclaimed: B claimed+held: fired=%u enabled=%u\n", common.fired - fired, gicd_bit(GICD_ISENABLER));
		if ((common.fired - fired != 2u * UNCLAIMED_LIMIT) || (gicd_bit(GICD_ISENABLER) == 0u)) {
			fail = "B (a claimed line was masked)";
			break;
		}

		/* C: declined, line held -- must be masked after exactly LIMIT */
		fired = common.fired;
		common.budget = 3u * UNCLAIMED_LIMIT;
		common.mode = mode_decline_held;
		gicd_setBit(GICD_ISPENDR);
		wait_masked_or_stalled();
		printf("irq-unclaimed: C declined+held: fired=%u enabled=%u pending=%u\n", common.fired - fired,
			gicd_bit(GICD_ISENABLER), gicd_bit(GICD_ISPENDR));
		if (gicd_bit(GICD_ISENABLER) != 0u) {
			fail = "C (an unclaimed held line was not masked)";
			break;
		}
		if (common.fired - fired != UNCLAIMED_LIMIT) {
			fail = "C (masked after the wrong number of deliveries)";
			break;
		}

		/* D: a new handler enables it again; the latched pend fires at once */
		fired = common.fired;
		common.budget = 0;
		common.mode = mode_claim;
		err = interrupt(common.irq, irq_handler2, NULL, 0, &h2);
		if (err < 0) {
			printf("irq-unclaimed: D interrupt() failed: %d\n", err);
			fail = "D (could not register a second handler)";
			break;
		}
		haveH2 = 1;
		(void)wait_idle(&common.fired2, 1u, WAIT_MS);
		gicd_setBit(GICD_ISPENDR);
		(void)wait_idle(&common.fired2, 2u, WAIT_MS);
		printf("irq-unclaimed: D re-register: fired=%u fired2=%u enabled=%u\n", common.fired - fired, common.fired2,
			gicd_bit(GICD_ISENABLER));
		if ((common.fired2 != 2u) || (common.fired - fired != 2u) || (gicd_bit(GICD_ISENABLER) == 0u)) {
			fail = "D (registering a handler did not enable the IRQ again)";
			break;
		}
	} while (0);

	/* Stop any re-pending before dropping the handlers */
	common.budget = 0;
	common.mode = mode_decline;
	if (haveH2 != 0) {
		(void)resourceDestroy(h2);
	}
	(void)resourceDestroy(h1);
	gicd_setBit(GICD_ICPENDR);

	if (gicd_bit(GICD_ISENABLER) != 0u) {
		printf("irq-unclaimed: IRQ %u still enabled after removing both handlers\n", common.irq);
		if (fail == NULL) {
			fail = "cleanup (IRQ left enabled)";
		}
	}

	(void)munmap(page, _PAGE_SIZE);

	if (fail != NULL) {
		printf("IRQ-UNCLAIMED: FAIL %s\n", fail);
		return 1;
	}

	printf("IRQ-UNCLAIMED: PASS\n");

	return 0;
}
