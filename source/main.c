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

typedef enum {
	Wii  = 0x1,
	vWii = 0x2,
	Mini = 0x4,

	All = (Wii | vWii | Mini),
	Decaffeinator_Only = (Wii | Mini),
} ConsoleType;

enum ChannelFlags {
	/* Just download this title ID. */
	RegionFree		= 0x00,

	/* Fill in the last byte with the system's region. */
	RegionSpecific	= 0x01,

	/* Fill in the last byte with A, or K if the system is Korean. */
	RegionFreeAndKR = 0x02,

	/* Will only be available for Japanese systems. */
	JPonly			= 0x04,

	/* Will not be available on Korean systems. */
	NoKRVersion		= 0x08,
};

static ConsoleType ThisConsole = Wii;

typedef struct {
	const char* name;
	const char* desc;
	int64_t titleID;
	enum ChannelFlags flags;
	ConsoleType allowed;

	int64_t titleID_new;
	int vWii_IOS;
	bool selected;
} Channel;

static const char* strConsoleType(ConsoleType con) {
	switch (con) {
		case Wii : return "Wii";
		case vWii: return "vWii (Wii U)";
		case Mini: return "Wii Mini";
	}

	return "?";
}

static bool SelectChannels(Channel* channels[], int cnt) {
	int index = 0, curX = 0, curY = 0;

	CON_GetPosition(&curX, &curY);

	for (;;) {
		Channel* ch = channels[index];

		printf("\x1b[%i;0H", curY);
		for (int i = 0; i < cnt; i++)
			printf("%s%s	%s\x1b[40m\x1b[39m\n", i == index? ">>" : "  ",
				   channels[i]->selected? "\x1b[47;1m\x1b[30m" : "", channels[i]->name);

		printf("\n\x1b[0J%s", ch->desc ? ch->desc : "no description");

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

			else if (buttons & WPAD_BUTTON_A) { ch->selected ^= true; break; }
			else if (buttons & WPAD_BUTTON_PLUS) return true;
			else if (buttons & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) return false;
		}
	}
}

static int InstallChannel(Channel* ch) {
	struct Title title = {};

	printf("	>> Downloading %016llx metadata...\n", ch->titleID);
	int ret = DownloadTitleMeta(ch->titleID, -1, &title);
	if (ret < 0) {
		printf("failed! (%i)\n", ret);
		return ret;
	}

	if (ch->titleID_new) {
		ChangeTitleID(&title, ch->titleID_new);
		Fakesign(&title);
	}
	if (ch->vWii_IOS && ThisConsole == vWii) {
		title.tmd->sys_version=0x1LL<<32 | ch->vWii_IOS;
		Fakesign(&title);
	}

	printf("	>> Installing %016llx...\n", ch->titleID);
	ret = InstallTitle(&title, ch->titleID >> 32 != 0x1); //lazy fix
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

static const char* strRegionLetter(int c) {
	switch (c) {
		case 'J': return "Japan (NTSC-J)";
		case 'E': return "USA (NTSC-U)";
		case 'P': return "Europe (PAL)";
		case 'K': return "Korean (NTSC-K)";
	}

	return "Unknown (!?)";
}

static Channel channels[] = {
	{
		"EULA",

		"Often missing because people don't complete their region changes.\n"
		"This will stand out if the User Agreements button demands for a\n"
		"Wii System Update.",

		0x0001000848414B00, RegionSpecific, All
	},

	{
		"Region Select",

		"This hidden channel is launched by apps like Mario Kart Wii and\n"
		"the Everybody Votes Channel. And somehow not in the Forecast\n"
		"Channel, but whatever.\n\n"

		"Ideal if your console was region changed.",
		0x0001000848414C00, RegionSpecific, Decaffeinator_Only
	},

	{
		"Set Personal Data",

		"This hidden channel is only used by some Japanese-exclusive\n"
		"channels, namely the Digicam Print Channel and Demae Channel.\n\n"

		"This won't work very well with the WiiLink services.\n",
		0x000100084843434A, JPonly, All
	},

	{
		"Mii Channel",

		"Stock version of the Mii Channel.\n\n"

		"Will not remove your Miis when (re)installed,\n"
		"they are stored elsewhere.\n",
		0x0001000248414341, RegionFree, Wii | Mini
	},

	{
		"Mii Channel (Wii version)",

		"This version of the Mii Channel comes with features removed\n"
		"from the vWii version, specifically sending Miis to\n"
		"Wii remotes, Wii friends and the Mii Parade.\n\n"

		"Installing this will also remove wuphax (if applicable.)",
		0x0001000248414341, RegionFree, vWii
	},

	{
		"Photo Channel 1.0",

		"Please note that this version does not support SDHC (>2GB) cards.",
		0x0001000248414141, NoKRVersion, All
	},

	{
		"Photo Channel 1.1b (Update)",

		"This hidden channel is launched by the Wii menu when it detects\n"
		"the Photo Channel 1.1 stub (00010000-HAZA) on the system,\n"
		"i.e. you downloaded it from the Wii Shop Channel.\n\n"

		"See also: IOS61",
		0x0001000248415900, RegionFreeAndKR, Wii
	},

	{
		"Photo Channel 1.1b (photo_upgrader style)",

		"This is the hidden channel with it's title ID changed to HAAA,\n"
		"replacing Photo Channel 1.0 in the process.\n",

		0x0001000248415900, RegionFreeAndKR, All, 0x0001000248414141, 58
	},

	{
		"Wii Shop Channel",

		"Install this if the shop is bugging you to update.\n\n"

		"See also: IOS61",
		0x0001000248414200, RegionFreeAndKR, Decaffeinator_Only
	},

	{
		"Internet Channel",

		"Official Wii Internet browser, powered by Opera.\n"
		"Does not support modern encryption. Won't work with a lot of sites.",
		0x0001000148414400, RegionSpecific | NoKRVersion, All
	},

	{
		"IOS58",

		"The only part of the 4.3 update that mattered.\n"
		"If you do not already have this, re-install the Homebrew Channel\n"
		"to make it use IOS58.\n\n"

		"Re-launching the HackMii Installer: https://wii.hacks.guide/hackmii",
		0x0000000100000000 | 58, RegionFree, Wii
	},

	{
		"IOS61",

		"Released in 4.0.\n"
		"Used by the latest Wii Shop Channel and Photo Channel 1.1.",
		0x0000000100000000 | 61, RegionFree, Wii
	},

	{
		"IOS62",

		"Used by the Wii U Transfer Tool. If your Wii Shop Channel is not updated,\n"
		"you likely need this as well.",
		0x0000000100000000 | 62, RegionFree, Wii
	},
};
#define NBR_CHANNELS (sizeof(channels) / sizeof(Channel))

int main() {
	puts(
		"Wii System Channel Restorer by thepikachugamer\n"
	//	"This is a mix of photo_upgrader and cleartool\n"
	);

	if (patchIOS(false) < 0)
		puts("(failed to apply IOS patches..?)");

	initpads();
	ISFS_Initialize();

	const char regionLetter = GetSystemRegionLetter();
	if (!regionLetter) {
		puts("Failed to identify system region (!?)");
		goto exit;
	}

	uint32_t x = 0;

	if (!ES_GetTitleContentsCount(0x100000200LL, &x) && x) {
		ThisConsole = vWii;
	}
	else {
		struct Title sm = {};
		if (!GetInstalledTitle(0x100000002LL, &sm)) {
			uint16_t sm_rev = sm.tmd->title_version;
			FreeTitle(&sm);
			if ((sm_rev & ~0x0001) == 0x1200)
				ThisConsole = Mini;
		}
	}

	printf("Console region: %-24s    Console Type: %s\n\n", strRegionLetter(regionLetter), strConsoleType(ThisConsole));

	if (network_init() < 0) {
		puts("Failed to initialize network!");
		goto exit;;
	}

	puts(
		"Select the channels you would like to restore.\n"
		"Press A to toggle an option. Press +/START to begin.\n"
		"Press B to cancel.\n" );

	int i = 0;
	Channel* allowedChannels[NBR_CHANNELS] = {};
	for (Channel* ch = channels; ch < channels + NBR_CHANNELS; ch++) {
		if (!(ch->allowed & ThisConsole)) continue;

		if ((ch->flags & JPonly) && regionLetter != 'J') continue;
		else if ((ch->flags & NoKRVersion) && regionLetter == 'K') continue;

		if (ch->flags & RegionSpecific) ch->titleID |= regionLetter;
		else if (ch->flags & RegionFreeAndKR) ch->titleID |= (regionLetter == 'K') ? 'K' : 'A';
		allowedChannels[i++] = ch;

	}

	if (!SelectChannels(allowedChannels, i)) goto exit;

	putchar('\n');

	// what a mess of code this thing is
	for (Channel* ch = channels; ch < channels + NBR_CHANNELS; ch++) {
		if (!ch->selected) continue;

		printf("[*] Installing %s...\n", ch->name);
		InstallChannel(ch);
	}

exit:
	network_deinit();
	ISFS_Deinitialize();
	puts("\n\nPress HOME to exit.");
	wait_button(WPAD_BUTTON_HOME);
	WPAD_Shutdown();
	return 0;
}

