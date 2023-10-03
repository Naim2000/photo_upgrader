#include "tools.h"

#include <stdio.h>
#include <errno.h>
#include <wiiuse/wpad.h>
#include <gccore.h>
#include <ogc/es.h>
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

unsigned int check_title(const uint64_t tid) {
	OSReport("check_title(%016llx)\n", tid);
	unsigned int lv = 0, viewcnt = 0, cnt = 0;

	ES_GetNumTicketViews(tid, &viewcnt);
	OSReport("* view count: %u\n", viewcnt);
	if (viewcnt) {
		lv++;
		ES_GetTitleContentsCount(tid, &cnt);
		OSReport("* contents count: %u\n", cnt);
		if (cnt) lv++;
	}
	OSReport("total existence level is %u\n", lv);
	return lv;
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
	if(!~vwii) vwii = check_title( 1LL << 32 | 0x200 ) >= 2;
	return vwii;
}

int get_title_rev(const uint64_t tid) {
	unsigned int viewsize = 0;
	OSReport("get_title_rev(%016llx)\n", tid);

	ES_GetTMDViewSize(tid, &viewsize);
	OSReport("view size %u\n", viewsize);
	if(!viewsize) return -1;

	unsigned char _view[viewsize] ATTRIBUTE_ALIGN(0x20);
	tmd_view* view =  (void*)_view;

	int ret = ES_GetTMDView(tid, _view, viewsize);
	if(ret < 0) return -1;
	else return view->title_version;
}

int net_init_retry(unsigned int retries) {
	int ret = 0;
	for (int s = 0; s < 5; s++) {
		ret = net_init();
		if(!ret || ret != -EAGAIN) break;
	}

	return ret;
}

uint16_t input_scan(uint16_t value) {
	uint16_t input = 0x00;

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

	if (wii_down & WPAD_BUTTON_PLUS		|| gcn_down & PAD_BUTTON_START)
		input |= input_start;
	if (wii_down & WPAD_BUTTON_MINUS	|| gcn_down & PAD_TRIGGER_Z)
		input |= input_select;
	if (wii_down & WPAD_BUTTON_HOME		|| gcn_down & PAD_BUTTON_START || SYS_ResetButtonDown())
		input |= input_home;

	return value ?
		input & value :
		input;
}

int quit(int ret) {
	net_deinit();
	ISFS_Deinitialize();
	printf("\nPress HOME/START to return to loader.\n");
	while(!input_scan(input_home)) VIDEO_WaitVSync();
	WPAD_Shutdown();
	return ret;
}

bool confirmation(void) {
	printf("\nPress +/START to continue." "\n" "Press any other button to cancel.\n");
	unsigned int input = input_scan(0);
	while(!input) {
		input = input_scan(0);
		VIDEO_WaitVSync();
	}
	return (input & input_start) > 0;
}
