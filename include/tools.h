#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define tid_hi(tid) (uint32_t)(tid >> 32)
#define tid_lo(tid) (uint32_t)(tid & ~0)

#define input_up		(1 << 0x0)
#define input_down		(1 << 0x1)
#define input_left		(1 << 0x2)
#define input_right		(1 << 0x3)

#define input_a			(1 << 0x4)
#define input_b			(1 << 0x5)
#define input_x			(1 << 0x6)
#define input_y			(1 << 0x7)

#define input_start		(1 << 0x8)
#define input_select	(1 << 0x9)

#define input_home		(1 << 0xB)

void init_video(int row, int col);

uint16_t input_scan(uint16_t value);
int quit(int ret);
int net_init_retry(unsigned int retries);
int get_title_rev(const uint64_t tid);
unsigned int check_title(const uint64_t tid);

bool check_dolphin(void);
bool check_vwii(void);
bool confirmation(void);

extern __attribute__((weak)) void OSReport(const char* fmt, ...) {}
extern void* memalign(size_t alignment, size_t size);
extern unsigned int sleep(unsigned int seconds);
