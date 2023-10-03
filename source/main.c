#define VERSION "0.9.0"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/isfs.h>
#include <ogc/es.h>
#include <debug.h>
#include <network.h>

#include "runtimeiospatch.h"
#include "tools.h"
#include "aes.h"
#include "http.h"

#define ALIGN(a,b) ((((a)+(b)-1)/(b))*(b))
#define MAXIMUM(num, max) ( (num > max) ? max : num )

const char header[] = "Photo Channel 1.1 installer v" VERSION ", by thepikachugamer\n\n";

bool offline_mode = true;
static fstats file_stats ATTRIBUTE_ALIGN(32);

const uint64_t
	tid =		0x0001000248415941LL,
	tid_new =	0x0001000248414141LL,
	tid_stub = 	0x0001000048415A41LL;

const aeskey wii_ckey = {0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4, 0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7};

void* NUS_Download(const uint64_t tid, const char* obj, unsigned int* size, int* ec) {
	char url[100];
	unsigned int http_status = 0;
	unsigned char* buffer = NULL;

	sprintf(url, "http://nus.cdn.shop.wii.com/ccs/download/%016llx/%s", tid, obj);

	if(!http_request(url, ~0)) { // bleh
		printf("\n\x1b[41;30m!>> http_request() returned false; is it another tcp_read timeout? \x1b[40;37m\n");
		*ec = -1;
		return NULL;
	}
	http_get_result(&http_status, &buffer, size);

	if(http_status != 200)
		/*
		 * Section for error 117yyy: WC24 task type »download«.
		 * Error for error 11xyyy: WC24 returned HTTP status code yyy.
		 */
		*ec = -117000 - http_status;
	else
		*ec = 0;

	return buffer;
}

void* FS_Read(const char* filepath, unsigned int* filesize, int* ret) {
	*ret = ISFS_Open(filepath, ISFS_OPEN_READ);
	if(*ret < 0) return NULL;
	int fd = *ret;

	if(! *filesize) {
		*ret = ISFS_GetFileStats(fd, &file_stats);
		if(*ret < 0) return NULL;
		*filesize = file_stats.file_length;
	}
	unsigned char *buffer = memalign(0x20, *filesize);
	if(!buffer) {
		*ret = -ENOMEM;
		return buffer;
	}

	*ret = ISFS_Read(fd, buffer, *filesize);
	if(*ret < *filesize) {
		free(buffer);
		buffer = NULL;
		if(*ret > 0) *ret = -EIO;
	}
	else *ret = 0;

	ISFS_Close(fd);
	return buffer;
}

signed_blob* fetch_tmd(const uint64_t tid, unsigned int* size, int* ret) {
	signed_blob* s_tmd = NULL;

	if(offline_mode) {
		*ret = ES_GetStoredTMDSize(tid, size);
		if(*ret < 0) return NULL;

		s_tmd = memalign(0x20, *size);
		if(!s_tmd) {
			*ret = -ENOMEM;
			return NULL;
		}

		*ret = ES_GetStoredTMD(tid, s_tmd, *size);
		if(*ret < 0) {
			free(s_tmd);
			return NULL;
		}

		return s_tmd;
	}
	else {
		unsigned char* _tmd = NUS_Download(tid, "tmd", size, ret);
		if(*ret < 0) return NULL;

		s_tmd = memalign(0x20, *size);
		if(s_tmd)
			memcpy(s_tmd, _tmd, *size);
		else
			*ret = -ENOMEM;

		free(_tmd);
		return s_tmd;
	}
}

signed_blob* fetch_tik(const uint64_t tid, unsigned int* size, int* ret) {
	signed_blob* s_tik;

	if(offline_mode) {
		char filepath[30];
		sprintf(filepath, "/ticket/%08x/%08x.tik", tid_hi(tid), tid_lo(tid));
		return FS_Read(filepath, size, ret);
	}
	else {
		unsigned char* _tik = NUS_Download(tid, "cetk", size, ret);
		if(*ret < 0) return NULL;

		s_tik = memalign(0x20, *size);
		if (s_tik)
			memcpy(s_tik, _tik, *size);
		else
			*ret = -ENOMEM;

		free(_tik);
		return s_tik;
	}
}

void get_titlekey(tik* ticket, aeskey out) {
	struct AES_ctx aes = {};
	aeskey iv = {};

	memcpy(out, ticket->cipher_title_key, sizeof(aeskey));
	*(uint64_t*) iv = ticket->titleid;

	AES_init_ctx_iv(&aes, wii_ckey, iv);
	AES_CBC_decrypt_buffer(&aes, out, sizeof(aeskey));
}

void change_tid(tmd* tmd, tik* ticket, const uint64_t tid_new, aeskey title_key) {
	struct AES_ctx aes = {};
	aeskey iv = {};

	ticket->titleid = tid_new;
	*(uint64_t*) iv = ticket->titleid;

	AES_init_ctx_iv(&aes, wii_ckey, iv);
	AES_CBC_encrypt_buffer(&aes, title_key, sizeof(aeskey));
	memcpy(ticket->cipher_title_key, title_key, sizeof(aeskey));

	tmd->title_id = tid_new;
}

int purge_title(const uint64_t tid) {
	unsigned int viewcnt = 0;
	int ret;

	ret = ES_GetNumTicketViews(tid, &viewcnt);
	if (!viewcnt) return ret ? ret : ENOENT;

	tikview
		view ATTRIBUTE_ALIGN(0x20) = {},
		views[viewcnt] ATTRIBUTE_ALIGN(0x20) = {};

	ret = ES_GetTicketViews(tid, views, viewcnt);
	if (ret < 0) return ret;
	for(unsigned int i = 0; i < viewcnt; i++) {
		memcpy(&view, views + i, sizeof(tikview));
		ret = ES_DeleteTicket(&view);
		if (ret < 0) return ret;
	}

	ES_DeleteTitleContent(tid);
	return ES_DeleteTitle(tid); // not fatal enough to matter tbh
}

int install() {
	int ret = 0;
	unsigned int certs_size = 0, tmd_size = 0, tik_size = 0;

	signed_blob *s_certs = NULL, *s_tmd = NULL, *s_tik = NULL;

	struct AES_ctx AESctx_title = {};

	ret = get_title_rev(tid_new);
	if(ret > 2) {
		printf(
			"You already did this?" "\n"
			"(Photo channel 1.0 title version %u is > 2)" "\n", ret);
		return 0;
	}

	ret = check_title(tid_stub);
	if (ret) {
		printf( ret >= 2 ?
			"Photo channel 1.1 stub is already installed, do you need something..?\n" :
			"You own Photo Channel 1.1 in the Wii Shop Channel, maybe you want to get it?\n");
		return 0;
	}

	if (check_title(tid) >= 2)
		printf("HAYA is present, using offline mode.\n\n");

	else {
		printf("HAYA is not present, using online mode.\n");

		printf("> Initializing network... ");
		ret = net_init_retry(5);
		if (ret < 0) {
			printf(
				"failed! (%d)" "\n\n"

				"Try grab a Photo Channel 1.1 WAD from NUS Downloader and install it." "\n"
				"This will work around the internet requirement." "\n", ret);
			return ret;
		}

		uint32_t hostip = net_gethostip();
		printf("OK! Wii IP address: %hhu.%hhu.%hhu.%hhu\n\n",
			   hostip >>24, hostip >>16, hostip >>8, hostip >>0);
		offline_mode = false;
	}

	if(check_dolphin())
		printf("\x1b[41m This is Dolphin Emulator, expect -2011. \x1b[40m\n");

	printf(
		"This will install the hidden Photo Channel 1.1" "\n"
		"title directly over Photo Channel 1.0." "\n\n"

		"Is this OK?" "\n");
	sleep(3);

	if(!confirmation()) return 2976579765;

	printf("\n> Reading certs... ");
	s_certs = FS_Read("/sys/cert.sys", &certs_size, &ret);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK! (size = %u)\n", certs_size);

	printf("> Reading TMD... ");
	s_tmd = fetch_tmd(tid, &tmd_size, &ret);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK! (size = %u)\n", tmd_size);
	tmd* p_tmd = SIGNATURE_PAYLOAD(s_tmd);

	printf("> Reading ticket... ");
	s_tik = fetch_tik(tid, &tik_size, &ret);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK! (size = %u)\n", tik_size);
	tik* p_tik = SIGNATURE_PAYLOAD(s_tik);

	printf("> Changing Title ID... ");
	aeskey title_key = {};
	get_titlekey(p_tik, title_key);
	AES_init_ctx(&AESctx_title, title_key);
	change_tid(p_tmd, p_tik, tid_new, title_key);
	printf("OK!\n");

	if(check_vwii()) p_tmd->sys_version = 1LL << 32 | 58;

	printf("> Removing Photo Channel 1.0... ");
	ret = purge_title(tid_new);
	if (ret < 0) printf("failed..? (%d)\n", ret);
	else printf("OK!\n");

	printf("> Installing ticket... ");
	ret = ES_AddTicket(s_tik, STD_SIGNED_TIK_SIZE, s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK!\n");

	printf("> Starting title installation... ");
	ret = ES_AddTitleStart(s_tmd, SIGNED_TMD_SIZE(s_tmd), s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		ES_AddTitleCancel();
		printf("failed! (%d)\n", ret);
		return ret;
	}

	printf("OK!\n");

	for(unsigned int i = 0; i < p_tmd->num_contents; i++) {
		tmd_content* content = p_tmd->contents + i;
		unsigned int cid = content->cid, csize = content->size, align_csize = ALIGN(content->size, 0x20), _csize = 0;
		unsigned char* buffer = NULL;
		int cfd = 0;

		if (offline_mode) {
			if(content->type == 0x8001) continue; // Shared content, user already has it.
			printf(">> Installing content #%02d... ", i);

			ret = ES_AddContentStart(p_tmd->title_id, cid);
			if(ret < 0) {
				printf("failed! (start, %d)\n", ret);
				ES_AddTitleCancel();
				return ret;
			}
			cfd = ret;

			char filepath[128];
			sprintf(filepath, "/title/%08x/%08x/content/%08x.app", tid_hi(tid), tid_lo(tid), cid);
			buffer = FS_Read(filepath, &_csize, &ret);
			if(ret < 0) {
				printf("failed! (read, %d)\n", ret);
				ES_AddTitleCancel();
				return ret;
			}

			aeskey encrypt_iv = {};
			encrypt_iv[1] = i & 0xFF;
			AES_ctx_set_iv(&AESctx_title, encrypt_iv);

			putc('\n', stdout);

			unsigned char enc_buf[1024] ATTRIBUTE_ALIGN(0x20) = {};

			for (unsigned int total = 0, z = 0; total < csize; total += z) {
				z = MAXIMUM(align_csize - total, sizeof(enc_buf));
				memcpy(enc_buf, buffer + total, z);

				AES_CBC_encrypt_buffer(&AESctx_title, enc_buf, z);
				printf("\r%u/%u bytes / %.2f%% ... ",
						total + z, align_csize,
						( (total + z) / (double)align_csize ) * 100);

				ret = ES_AddContentData(cfd, enc_buf, z);
				if (ret < 0) {
					printf("error! (%d)\n", ret);
					ES_AddContentFinish(cfd);
					ES_AddTitleCancel();
					return ret;
				}
			}
		} else {
			printf(">> Downloading content %08x... ", cid);
			char cidstr[9]; // muh \x00
			sprintf(cidstr, "%08x", cid);

			buffer = NUS_Download(tid, cidstr, &_csize, &ret);
			if(ret < 0) {
				printf("failed! (%d)\n", ret);
				ES_AddTitleFinish();
				return ret;
			}
			printf("OK! (size = %u)\n", _csize);

			printf(">> Installing... ");
			ret = ES_AddContentStart(p_tmd->title_id, cid);
			if(ret < 0) {
				printf("failed! (start, %d)\n", ret);
				ES_AddTitleCancel();
				return ret;
			}
			cfd = ret;

			ret = ES_AddContentData(cfd, buffer, align_csize);
			if (ret < 0) {
				printf("failed! (%d)\n", ret);
				ES_AddContentFinish(cfd);
				ES_AddTitleCancel();
				return ret;
			}
		}

		free(buffer);
		ret = ES_AddContentFinish(cfd);
		if (ret < 0) {
			printf("failed! (%d)\n", ret);
			ES_AddTitleCancel();
			return ret;
		}
		printf("OK!\n\n");

	}
	printf("> Finishing installation... ");
	ret = ES_AddTitleFinish();
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		ES_AddTitleCancel();
		return ret;
	}
	printf("OK!\n");

	printf("\x1b[42m All done! \x1b[40m\n");
	return 0;
}

int restore() {
	int ret = 0;
	unsigned int certs_size = 0, tik_size = 0, tmd_size = 0;
	signed_blob *s_certs, *s_tmd, *s_tik;

	int HAAA_rev = get_title_rev(tid_new), HAZA = check_title(tid_stub);
	OSReport("HAAA rev is %u, HAZA existence level is %d\n", HAAA_rev, HAZA);
	if(HAAA_rev <= 2 && HAZA >= 2) {
		printf("Check Data management.\n");
		return 0;
	}

	printf("> Initializing network... ");
	ret = net_init_retry(5);
	if (ret < 0) {
		printf(
			"failed! (%d)" "\n\n"

			"May as well just grab the original Photo Channel 1.0 WAD" "\n\n", ret
		);
		return ret;
	}
	uint32_t hostip = net_gethostip();
	printf("OK! Wii IP address: %hhu.%hhu.%hhu.%hhu\n\n",
		hostip >>24, hostip >>16, hostip >>8, hostip >>0);
	offline_mode = false;

	printf("> Removing Photo Channel 1.0*... ");
	ret = purge_title(tid_new);
	if (ret < 0) printf("failed? (%d)...\n", ret);
	else printf("OK!\n");

	printf("> Reading certs... ");
	s_certs = FS_Read("/sys/cert.sys", &certs_size, &ret);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK! (size = %u)\n", certs_size);

	printf("> Downloading TMD... ");
	s_tmd = fetch_tmd(tid_new, &tmd_size, &ret);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK! (size = %u)\n", tmd_size);
	tmd* p_tmd = SIGNATURE_PAYLOAD(s_tmd);

	printf("> Downloading ticket... ");
	s_tik = fetch_tik(tid_new, &tik_size, &ret);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK! (size = %u)\n", tik_size);

	printf("> Installing ticket... ");
	ret = ES_AddTicket(s_tik, STD_SIGNED_TIK_SIZE, s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK!\n");

	printf("> Starting title installation... ");
	ret = ES_AddTitleStart(s_tmd, SIGNED_TMD_SIZE(s_tmd), s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		ES_AddTitleCancel();
		printf("failed! (%d)\n", ret);
		return ret;
	}
	printf("OK!\n");

	for (unsigned int i = 0; i < p_tmd->num_contents; ++i) {
		tmd_content* content = p_tmd->contents + i;
		unsigned int cid = content->cid, csize = content->size, align_csize = ALIGN(csize, 0x20), _csize = 0;
		unsigned char* buffer = NULL;
		int cfd = 0;

		printf(">> Downloading content %08x... ", cid);

		ret = ES_AddContentStart(p_tmd->title_id, cid);
		if (ret < 0) {
			ES_AddTitleCancel();
			printf("failed! (start, %d)\n", ret);
			return ret;
		}
		cfd = ret;

		char cidstr[9];
		sprintf(cidstr, "%08x", cid);

		buffer = NUS_Download(tid_new, cidstr, &_csize, &ret);
		if (ret < 0) {
			ES_AddContentFinish(cfd);
			ES_AddTitleCancel();
			printf("failed! (%d)\n", ret);
			return ret;
		}
		printf("OK! (size = %u)\n", _csize);

		printf(">> Installing... ");
		ret = ES_AddContentData(cfd, buffer, align_csize);
		if (ret < 0) {
			ES_AddContentFinish(cfd);
			ES_AddTitleCancel();
			printf("failed! (%d)\n", ret);
			return ret;
		}

		ret = ES_AddContentFinish(cfd);
		if (ret < 0) {
			ES_AddTitleCancel();
			printf("failed! (%d)\n", ret);
			return ret;
		}
		printf("OK!\n");
	}
	ret = ES_AddTitleFinish();
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return ret;
	}

	printf("OK!!\n");
	return 0;
}

int main() {
	int ret = 0;

	init_video(2, 0);
	printf(header);

	if(!check_dolphin()) {
		printf("Applying IOS patches... ");
		ret = IosPatch_RUNTIME(true, false, true, false);
		if (ret < 0) {
			printf("Failed! (%d)" "\n"
				"Is your Homebrew Channel updated?" "\n"
				"Is <ahb_access/> in meta.xml?" "\n\n"

				"Exiting in 5s...", ret);
			sleep(5);
			return 0x0D800064;
		}
		printf("OK! (patch count = %d)\n\n", ret);
	}
	WPAD_Init();
	PAD_Init();
	ISFS_Initialize();

	printf(
		"Super basic menu" "\n\n"

		"Press A to install Photo Channel 1.1." "\n"
		"Press B to restore Photo Channel 1.0." "\n"
		"Press HOME/START to return to loader." "\n"
	);

	while(true) {
		unsigned int input = input_scan(0);
		if(input & input_a) {
			ret = install();
			break;
		}
		else if(input & input_b) {
			ret = restore();
			break;
		}
		else if(input & input_home) return 0;
	}
	return quit(ret);

}
