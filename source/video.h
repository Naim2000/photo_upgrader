#define clear() printf("\x1b[2J")
#define CONSOLE_HEIGHT		(448)
#define CONSOLE_WIDTH		(640)

[[gnu::constructor]] void init_video();
