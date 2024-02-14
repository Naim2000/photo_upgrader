#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include "iospatch.h"
#include "video.h"
#include "pad.h"
#include "network.h"
#include "nus.h"

#define VERSION "1.1.0"

[[gnu::weak, gnu::format(printf, 1, 2)]]
void OSReport(const char* fmt, ...) {}

struct option {
	const char* name;
	bool selected;
};

static bool SelectOptionsMenu(struct option options[]) {
	int cnt = 0, index = 0, curX = 0, curY = 0;
	while (options[++cnt].name)
		;

	CON_GetPosition(&curX, &curY);

	for (;;) {
		struct option* opt = options + index;

		printf("\x1b[%i;%iH", curY, curX);
		for (int i = 0; i < cnt; i++)
			printf("%s%s	%s\x1b[40m\x1b[39m\n", i == index? ">>" : "  ",
				   options[i].selected? "\x1b[47;1m\x1b[30m" : "", options[i].name);

		for (;;) {
			scanpads();
			uint32_t buttons = buttons_down(0);

			if (buttons & WPAD_BUTTON_DOWN) {
				if (++index == cnt) index = 0;
				break;
			}
			else if (buttons & WPAD_BUTTON_UP) {
				if (--index < 0) index = cnt - 1;
				break;
			}

			else if (buttons & WPAD_BUTTON_A) { opt->selected ^= true; break; }
			else if (buttons & WPAD_BUTTON_PLUS) return true;
			else if (buttons & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) return false;
		}
	}
}

static int InstallChannelQuick(uint64_t titleID, uint64_t newTID, int32_t newIOS) {
	struct Title title = {};

	printf("	>> Downloading %016llx metadata...\n", titleID);
	int ret = DownloadTitleMeta(titleID, -1, &title);
	if (ret < 0) {
		printf("failed! (%i)\n", ret);
		return ret;
	}

	if (newTID) {
		ChangeTitleID(&title, newTID);
		Fakesign(&title);
	}
	if (newIOS) {
		title.tmd->sys_version=0x1LL<<32 | newIOS;
		Fakesign(&title);
	}

	printf("	>> Installing %016llx...\n", titleID);
	ret = InstallTitle(&title, true);
	FreeTitle(&title);
	if (ret < 0) {
		printf("failed! (%i)\n", ret);
		return ret;
	}

	return 0;
}

const char GetSystemRegionLetter(void) {
	CONF_Init();

	switch (CONF_GetRegion()) {
		case CONF_REGION_JP: return 'J';
		case CONF_REGION_US: return 'E';
		case CONF_REGION_EU: return 'P';
		case CONF_REGION_KR: return 'K';
	}

	return 0;
}

int main() {
	int ret;
	bool vwii = false;

	static struct option opts[] = {
		{"End-user License Agreement channel"},
		{"Mii Channel (Wii version)"},
		{"Photo Channel 1.1b"},
		{"Wii Shop Channel (!?)"},
		{}
	};

	puts(
		"System Channel Restorer by thepikachugamer\n"
		"This is a mix of photo_upgrader and cleartool\n"
	);

	if (patchIOS(false) < 0) {
		puts("Failed to apply IOS patches...!");
		sleep(2);
		leave(true);
	}

	initpads();
	ISFS_Initialize();

	struct Title sm = {};
	if (!GetInstalledTitle(0x100000002LL, &sm)) {
		uint16_t sm_rev = sm.tmd->title_version;
		FreeTitle(&sm);
		if (!(sm_rev & 0x1000) && !vwii)
			puts("\x1b[30;1mYou seem to be on a normal Wii. There isn't a lot to do here...\x1b[39m");

	}

	const char regionLetter = GetSystemRegionLetter();
	if (!regionLetter) {
		puts("Failed to identify system region (!?)");
		return -1;
	}

	uint32_t x = 0;
	ES_GetTitleContentsCount(0x100000200LL, &x);
	if (x) {
		puts("This seems to be a \x1b[34mvWii\x1b[39m (BC-NAND is present)\n");
		vwii = true;
	}

	if (network_init() < 0) {
		puts("Failed to initialize network..!");
		return -2;
	}

	puts(
		"Select the channels you would like to restore.\n"
		"Press A to toggle an option. Press +/START to begin.\n"
		"Press B to cancel." );

	if (!SelectOptionsMenu(opts)) return 0;

	putchar('\n');

	if (opts[0].selected) {
		puts("[+] Installing EULA channel...");
		if (InstallChannelQuick(0x0001000848414C00LL | regionLetter, 0, 0) < 0) return -1;
	}
	if (opts[1].selected) {
		puts("[+] Installing standard Mii Channel...");
		if (InstallChannelQuick(0x0001000248414341LL, 0, 0) < 0) return -1;
	}
	if (opts[2].selected) {
		puts("[+] Installing Photo Channel 1.1b...");
		if (InstallChannelQuick(0x0001000248415941LL, 0x0001000248414141LL, vwii? 58 : 61) < 0) return -1;
	}
	if (opts[3].selected) {
		if (vwii) puts("[*] Please use vWii decaffeinator for this...");
		else {
			puts("[+] Installing Wii Shop Channel...");
			if (InstallChannelQuick(0x0001000248414200LL | regionLetter == 'K' ? 'K' : 'A', 0, 0) < 0) return -1;
		}
	}

	return 0;
}

[[gnu::destructor]]
void leave(bool now) {
	network_deinit();
	ISFS_Deinitialize();

	if (now) return;
	printf("\nPress HOME/START to return to loader.");
	for (;;) {
		scanpads();
		if (buttons_down(WPAD_BUTTON_HOME))
			break;
	}
	WPAD_Shutdown();
}
