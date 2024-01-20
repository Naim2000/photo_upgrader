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

#define INIT_STRUCT(v) v = (__typeof__(v)){}

[[gnu::weak, gnu::format(printf, 1, 2)]]
void OSReport(const char* fmt, ...) {}

int main() {
	int ret;
	static int64_t id_HABA = 0x0001000248414241LL;
	struct Title HABA = {};

	printf(
		"RSC \"DiiShop\" Uninstaller; by thepikachugamer\n"
		"Based off photo_upgrader v" VERSION " by... thepikachugamer\n\n"
	);

	if (patchIOS(false) < 0) {
		puts("Failed to apply IOS patches...!");
		sleep(2);
		leave(true);
	}

	initpads();
	ISFS_Initialize();

	printf(
		"Super basic menu\n\n"

		"Press A to re-install stock Wii shop channel.\n"
		"Press HOME/START to return to loader.\n\n"
	);

	for (;;) {
		scanpads();
		uint32_t buttons = buttons_down(0);
		if (buttons & WPAD_BUTTON_A) {
			puts("Initializing network...");
			ret = network_init();
			if (ret < 0) {
				printf("Failed! (%i)\n", ret);
				return ret;
			}

			puts("Downloading title metadata...");
			ret = DownloadTitleMeta(id_HABA, -1, &HABA);
			if (ret < 0) {
				printf("Failed! (%i)\n%s\n", ret, GetLastDownloadError());
				return ret;
			}

			puts("Installing...");
			ret = InstallTitle(&HABA, true);
			FreeTitle(&HABA);
			if (ret < 0) {
				printf("Failed! (%i)\n%s\n", ret, GetLastDownloadError());
				return ret;
			}
			puts("\n\x1b[42mDone!\x1b[40m");
			break;
		}
		else if (buttons & WPAD_BUTTON_HOME)
			return 0;

		VIDEO_WaitVSync();
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
