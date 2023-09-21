#include <stddef.h>
#include <stdbool.h>

#define tid_hi(tid) (uint32_t)(tid >> 32)
#define tid_lo(tid) (uint32_t)(tid & ~0)

#define input_up		(1 << 0)
#define input_down		(1 << 1)
#define input_left		(1 << 2)
#define input_right		(1 << 3)

#define input_a			(1 << 4)
#define input_b			(1 << 5)
#define input_x			(1 << 6)
#define input_y			(1 << 7)

#define input_start		(1 << 8)
#define input_select	(1 << 9)

void init_video(int row, int col);

unsigned short input_scan(unsigned short value);
int quit(int ret);

bool check_dolphin(void);
bool check_vwii(void);

extern __attribute__((weak)) void OSReport(const char* fmt, ...) {}
extern void* memalign(size_t alignment, size_t size);
extern unsigned int sleep(unsigned int seconds);
