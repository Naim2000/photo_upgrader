#include "tools.h"

#include <stdio.h>
#include <wiiuse/wpad.h>
#include <gccore.h>
#include <ogc/conf.h>
#include <network.h>
#include <math.h>

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

uint32_t wii_down = 0;
uint16_t gcn_down = 0;
unsigned int dolphin_emu = ~0, vwii = ~0;

void init_video(int row, int col) {
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	CON_Init(xfb,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();
	printf("\x1b[%d;%dH", row, col);
}

bool check_dolphin(void) {
	if(!~dolphin_emu) {
		int dolphin_fd = IOS_Open("/dev/dolphin", IPC_OPEN_NONE);
		IOS_Close(dolphin_fd);
		dolphin_emu = dolphin_fd > 0;
	}
	return dolphin_emu;
}

bool check_vwii(void) {
	if(!~vwii) {
		unsigned int cnt = 0;
		ES_GetTitleContentsCount(0x0000000100000200LL, &cnt);
		vwii = cnt > 0;
	}
    return vwii;
}

int quit(int ret) {
	net_deinit();
	ISFS_Deinitialize();
	printf("\nPress HOME/START to return to loader.\n");
	while(!input_scan(input_start)) VIDEO_WaitVSync();
	WPAD_Shutdown();
	return ret;
}

unsigned short input_scan(unsigned short value) {
	unsigned short input = 0x00;

	WPAD_ScanPads();
	 PAD_ScanPads();
	wii_down = WPAD_ButtonsDown(0);
	gcn_down =  PAD_ButtonsDown(0);

	if (wii_down & WPAD_BUTTON_UP		|| gcn_down & PAD_BUTTON_UP)
		input |= input_up;
	if (wii_down & WPAD_BUTTON_DOWN		|| gcn_down & PAD_BUTTON_DOWN)
		input |= input_down;
	if (wii_down & WPAD_BUTTON_LEFT		|| gcn_down & PAD_BUTTON_LEFT)
		input |= input_left;
	if (wii_down & WPAD_BUTTON_RIGHT	|| gcn_down & PAD_BUTTON_RIGHT)
		input |= input_right;

	if (wii_down & WPAD_BUTTON_A		|| gcn_down & PAD_BUTTON_A)
		input |= input_a;
	if (wii_down & WPAD_BUTTON_B		|| gcn_down & PAD_BUTTON_B)
		input |= input_b;
	if (wii_down & WPAD_BUTTON_1		|| gcn_down & PAD_BUTTON_X)
		input |= input_x;
	if (wii_down & WPAD_BUTTON_2		|| gcn_down & PAD_BUTTON_Y)
		input |= input_y;

	if (wii_down & WPAD_BUTTON_HOME		|| gcn_down & PAD_BUTTON_START || SYS_ResetButtonDown())
		input |= input_start;
	if (wii_down & WPAD_BUTTON_PLUS		|| gcn_down & PAD_TRIGGER_Z)
		input |= input_select;

	return value ? input & value : input;
}

