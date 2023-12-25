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
	bool vwii = false;
	static int64_t id_HAYA = 0x0001000248415941LL, id_HAAA = 0x0001000248414141LL;
	struct Title HAYA = {}, HAAA = {};

	printf(
		"Photo Channel 1.1 Installer v" VERSION ", by thepikachugamer\n\n"
	);

	if (patchIOS(false) < 0) {
		puts("Failed to apply IOS patches...!");
		sleep(2);
		leave(true);
	}

	initpads();
	ISFS_Initialize();
	CONF_Init();

	uint32_t x = 0;
	ES_GetTitleContentsCount(0x100000200LL, &x);
	if (x) {
		puts("This seems to be a \x1b[34mvWii\x1b[39m (BC-NAND is present)\n");
		vwii = true;
	}

	if (CONF_GetRegion() == CONF_REGION_KR) {
		puts("This Wii seems to be Korean, using Korean photo channels.\n");
		id_HAYA = (id_HAYA & ~0xFF) | 'K';
	//	id_HAAA = (id_HAAA & ~0xFF) | 'K'; never actually existed? i'm stupid
	}

	uint16_t rev_HAAA = 0;
	ret = GetInstalledTitle(id_HAAA, &HAAA);
	if (HAAA.id) {
		rev_HAAA = HAAA.tmd->title_version;
		FreeTitle(&HAAA);
		INIT_STRUCT(HAAA);
	}

	printf(
		"Super basic menu\n\n"

		"Press A to install Photo Channel 1.1\n"
		"Press B to restore Photo Channel 1.0\n"
		"Press HOME/START to return to loader.\n\n"
	);

	for (;;) {
		scanpads();
		uint32_t buttons = buttons_down(0);
		if (buttons & WPAD_BUTTON_A) {
			if (rev_HAAA > 2) {
				printf("It doesn't seem like you need this.\n"
					   "(HAAA title version %u > 2)\n", rev_HAAA);
				return 0;
			}

			printf("This will install the hidden Photo Channel 1.1\n"
				 "title directly over Photo Channel 1.0.\n\n"

				 "Is this OK?\n\n"

				 "Press +/START to continue.\n"
				 "Press any other button to cancel.\n\n");

			wait_button(0);
			if (!buttons_down(WPAD_BUTTON_PLUS))
				return 0;

			puts("Getting title metadata...");
			ret = GetInstalledTitle(id_HAYA, &HAYA);
			if (ret < 0) {
				puts("HAYA is not present, initializing network...");
				ret = network_init();
				if (ret < 0) {
					puts("Failed to initialize network!");
					return ret;
				}
				printf("Initialized network. Wii IP Address: %s\n", PrintIPAddress());

				puts("Downloading title metadata...");
				ret = DownloadTitleMeta(id_HAYA, -1, &HAYA);
				if (ret < 0) {
					printf("Failed! (%i)\n%s\n", ret, GetLastDownloadError());
					return ret;
				}
			}
			puts("Changing title ID...");
			ChangeTitleID(&HAYA, id_HAAA);

			if (vwii)
				HAYA.tmd->sys_version = 1LL << 32 | 58;

			Fakesign(&HAYA);

			puts("Installing...");
			ret = InstallTitle(&HAYA, true);
			FreeTitle(&HAYA);
			if (ret < 0) {
				printf("Failed! (%i)\n%s\n", ret, GetLastDownloadError());
				return ret;
			}
			puts("\n\x1b[42mDone!\x1b[40m");
			break;
		}
		else if (buttons & WPAD_BUTTON_B) {
			if (rev_HAAA && rev_HAAA < 3) {
				printf("It doesn't seem like you need this.\n"
						"(HAAA title version %u < 3)\n", rev_HAAA);
				return 0;
			}

			puts("Initializing network...");
			ret = network_init();
			if (ret < 0) {
				puts("Failed to initialize network!");
				return ret;
			}
			printf("Initialized network. Wii IP Address: %s\n", PrintIPAddress());

			puts("Downloading title metadata...");
			ret = DownloadTitleMeta(id_HAAA, -1, &HAAA);
			if (ret < 0) {
				printf("Failed! (%i)\n%s\n", ret, GetLastDownloadError());
				return ret;
			}

			puts("\nInstalling...");
			ret = InstallTitle(&HAAA, true);
			FreeTitle(&HAAA);
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
