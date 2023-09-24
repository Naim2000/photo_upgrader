#define VERSION "1.0.0"

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
#include <libpatcher/libpatcher.h>

#include "tools.h"
#include "aes.h"
#include "http.h"

#define ALIGN(a,b) ((((a)+(b)-1)/(b))*(b))

const char header[] = "Photo Channel 1.1 installer v" VERSION ", by thepikachugamer\n\n";

bool offline_mode = true;
static fstats file_stats ATTRIBUTE_ALIGN(32);
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
	*filesize = 0;
	*ret = ISFS_Open(filepath, ISFS_OPEN_READ);
	if(*ret < 0) return NULL;
	int fd = *ret;


	*ret = ISFS_GetFileStats(fd, &file_stats);
	if(*ret < 0) return NULL;
	*filesize = file_stats.file_length;

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
		printf("> Reading %016llx title metadata... ", tid);
		*ret = ES_GetStoredTMDSize(tid, size);
		if(*ret < 0) {
			printf("failed! (%d)\n", *ret);
			return NULL;
		}
		s_tmd = memalign(0x20, *size);
		if(!s_tmd) {
			printf("failed! (No memory? [%u bytes])\n", *size);
			*ret = -ENOMEM;
			return NULL;
		}
		*ret = ES_GetStoredTMD(tid, s_tmd, *size);
		if(! *ret) {
			printf("OK! (size = %u)\n", *size);
			return s_tmd;
		}
		else {
			free(s_tmd);
			printf("failed! (%d)\n", *ret);
			return NULL;
		}
	}
	else {
		printf("> Downloading %016llx title metadata... ", tid);
		unsigned char* _tmd = NUS_Download(tid, "tmd", size, ret);
		if(*ret < 0) {
			printf("failed! (%d)\n", (*ret < -117000) ? abs(*ret + 117000) : *ret);
			return NULL;
		}

		s_tmd = memalign(0x20, *size);
		if(!s_tmd) {
			printf("failed! (No memory? [%u bytes])\n", *size);
			*ret = -ENOMEM;
		}
		else {
			memcpy(s_tmd, _tmd, *size);
			printf("OK! (size = %u)\n", *size);
		}
		free(_tmd);
	}

	return s_tmd;
}

signed_blob* fetch_tik(const uint64_t tid, unsigned int* size, int* ret) {
	signed_blob* s_tik;

	if(offline_mode) {
		printf("> Reading %016llx ticket... ", tid);
		char filepath[30];
		sprintf(filepath, "/ticket/%08x/%08x.tik", tid_hi(tid), tid_lo(tid));
		s_tik = FS_Read(filepath, size, ret);
		if(*ret < 0)
			printf("failed! (%d)\n", *ret);
		else
			printf("OK! (size = %u)\n", *size);

		return s_tik;
	}
	else {
		printf("> Downloading %016llx ticket... ", tid);
		unsigned char* _tik = NUS_Download(tid, "cetk", size, ret);
		if(*ret < 0) {
			printf("failed! (%d)\n", (*ret + 117000) > -1000 ? abs(*ret + 117000) : *ret);
			return NULL;
		}

		s_tik = memalign(0x20, *size);
		if(!s_tik) {
			printf("failed! (No memory? [%u bytes])\n", *size);
			*ret = -ENOMEM;
		}
		else {
			memcpy(s_tik, _tik, *size);
			printf("OK! (size = %u)\n", *size);
		}

		free(_tik);
		return s_tik;
	}
}

int main() {
	const uint64_t
		tid = 0x0001000248415941LL,
		tid_new = 0x0001000248414141LL,
		tid_stub =  0x0001000048415A41LL;

	int ret = 0;

	unsigned int
		certs_size = 0,
		tmd_size = 0,
		tik_size = 0,
		cnt = 0,
		_cnt = 0;

	signed_blob
		*s_certs = NULL,
		*s_tmd = NULL,
		*s_tik = NULL;

	struct AES_ctx AESctx_title = {}, AESctx_titlekey = {};

	init_video(2, 0);
	printf(header);

	printf("Applying IOS patches... ");
	if (!apply_patches()) {
		printf("Failed! Is your Homebrew Channel updated?\n"
			"Is <ahb_access/> in meta.xml?" "\n\n"

			"Exiting in 5s...");
		sleep(5);
		return 0x0D800064;
	}
	printf("OK!\n\n");

	WPAD_Init();
	PAD_Init();
	ISFS_Initialize();

	ES_GetNumTicketViews(tid_stub, &_cnt);
	if(_cnt) {
		printf("This console owns the Photo Channel 1.1 stub.\n" "Retrieve it at the Wii Shop Channel.\n");
		return quit(0);
	}

	printf(
		"This application will install the hidden Photo Channel 1.1" "\n"
		"title directly over Photo Channel 1.0." "\n\n"

		"Is this OK?" "\n"
	);
	sleep(3);

	if(!confirmation()) return quit(2976579765);

	ES_GetTitleContentsCount(tid, &cnt);
	if (!cnt) {
		printf("HAYA is not present, using online mode.\n");

		printf("> Initializing network... ");

		for (int s = 0; s < 5; s++) {
			ret = net_init();
			if(!ret || ret != -EAGAIN) break;
		}
		if (ret < 0) {
			printf(
				"failed! (%d)\n\n"
				"Try grab a Photo Channel 1.1 WAD from NUS Downloader and install it.\n"
				"This will work around the internet requirement.\n", ret);
			return quit(ret);
		}
		uint32_t hostip = net_gethostip();
		printf("OK! Wii IP address: %hhu.%hhu.%hhu.%hhu\n\n",
			   hostip >>24, hostip >>16, hostip >>8, hostip >>0);

		offline_mode = false;
	}
	else
		printf("HAYA is present, using offline mode.\n\n");

	if(check_dolphin())
		printf("\x1b[41m This is Dolphin Emulator, expect -2011. \x1b[40m\n");

	printf("> Reading certs... ");
	s_certs = FS_Read("/sys/cert.sys", &certs_size, &ret);

	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return quit(ret);
	}

	printf("OK! (size = %u)\n", certs_size);

	s_tmd = fetch_tmd(tid, &tmd_size, &ret);
	if(ret < 0) return quit(ret);

	tmd* p_tmd = SIGNATURE_PAYLOAD(s_tmd);

	s_tik = fetch_tik(tid, &tik_size, &ret);
	if(ret < 0) return quit(ret);

	tik* p_tik = SIGNATURE_PAYLOAD(s_tik);

	printf("> Changing Title ID... ");

	aeskey cipher_tkey = {}, title_key = {}, iv = {};
	memcpy(title_key, p_tik->cipher_title_key, sizeof(aeskey));
	*(uint64_t*) iv = p_tik->titleid;

	AES_init_ctx_iv(&AESctx_titlekey, wii_ckey, iv);
	AES_CBC_decrypt_buffer(&AESctx_titlekey, title_key, sizeof(aeskey));

	AES_init_ctx(&AESctx_title, title_key);
	memcpy(cipher_tkey, title_key, sizeof(aeskey));

	p_tik->titleid = tid_new;
	*(uint64_t*) iv = p_tik->titleid;
	AES_ctx_set_iv(&AESctx_titlekey, iv);
	AES_CBC_encrypt_buffer(&AESctx_titlekey, cipher_tkey, sizeof(aeskey));

	memcpy(p_tik->cipher_title_key, cipher_tkey, sizeof(aeskey));

	p_tmd->title_id = tid_new;
	printf("OK!\n");

	if(check_vwii()) {
		printf("* This seems to be a vWii, setting IOS to 58.\n");
		p_tmd->sys_version = 0x000000010000003ALL;
	}

/*	printf("> Faking signatures... ");

	memset(SIGNATURE_SIG(s_tik), 0, SIGNATURE_SIZE(s_tik) - 4);
	memset(SIGNATURE_SIG(s_tmd), 0, SIGNATURE_SIZE(s_tmd) - 4);

	sha1 tmdhash, tikhash;
	short int filler = 0;
	for(; filler < ((1 << (sizeof(filler) * 8) ) - 1); filler++) {
		if(tmdhash[0]) {
			p_tmd->fill3 = filler;
			SHA1((unsigned char*)p_tmd, TMD_SIZE(p_tmd), tmdhash);
		}
		if(tikhash[0]) {
			p_tik->padding = filler;
			SHA1((unsigned char*)p_tik, sizeof(tik), tikhash);
		}
		if(!tmdhash[0] && !tikhash[0]) break;
	}
	if(tmdhash[0] || tikhash[0]) {
		printf("failed! What?\n");
		return quit(~1);
	}
	printf("OK!\n");
*/
	printf("> Removing Photo Channel 1.0... ");
	tikview
		*views = NULL,
		view ATTRIBUTE_ALIGN(0x20) = {};
	unsigned int viewcnt = 0;

	ret = ES_GetNumTicketViews(tid_new, &viewcnt);
	if (ret < 0) {
		printf("failed! (get view count, %d)\n", ret);
		return quit(ret);
	}

	if(viewcnt) {
		views = memalign(0x20, sizeof(tikview) * viewcnt);
		if (!views) {
			printf("No memory?\n");
			return -ENOMEM;
		}

		ret = ES_GetTicketViews(tid_new, views, viewcnt);
		if (ret < 0) {
			printf("failed! (get views, %d)\n", ret);
			return quit(ret);
		}

		for(unsigned int i = 0; i < viewcnt; i++) {
			memcpy(&view, views + i, sizeof(tikview));
			ret = ES_DeleteTicket(&view);
			if (ret < 0) {
				printf("failed! (delete view #%d -> %d)", i, ret);
				return quit(ret);
			}
		}
		free(views);

		ES_DeleteTitleContent(tid_new);
		ES_DeleteTitle(tid_new); // not fatal enough to matter tbh
	} else
		printf("not present.. ");

	printf("OK!\n");

	printf("> Installing ticket... ");
	ret = ES_AddTicket(s_tik, STD_SIGNED_TIK_SIZE, s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return quit(ret);
	}
	printf("OK!\n");
/*
	printf("> Installing TMD... ");
	ret = ES_AddTitleTMD(s_tmd, SIGNED_TMD_SIZE(s_tmd));
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return quit(ret);
	}
	printf("OK!\n");
*/
	printf("> Starting title installation... ");
	ret = ES_AddTitleStart(s_tmd, SIGNED_TMD_SIZE(s_tmd), s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		ES_AddTitleCancel();
		printf("failed! (%d)\n", ret);
		return quit(ret);
	}

	printf("OK!\n");

	for(uint16_t i = 0; i < p_tmd->num_contents; i++) {
		tmd_content* content = &p_tmd->contents[i];
		unsigned int
			cid = content->cid,
			csize = content->size,
			_csize = 0,
			enc_buf_sz = 1024;

		unsigned char
			*buffer = NULL,
			enc_buf[1024] ATTRIBUTE_ALIGN(0x20) = {};

		int cfd;

		if (offline_mode) {
			printf(">> Installing content #%02d... ", i);
			if(content->type == 0x8001) { // // content->type & ~1 (probably bad), content->type != 1
				printf("Skipped. (This is a shared content.)\n\n");
				continue;
			}

			char filepath[128];
			unsigned int _csize = 0, align_csize = ALIGN(csize, 32);
			sprintf(filepath, "/title/%08x/%08x/content/%08x.app", tid_hi(tid), tid_lo(tid), cid);
			buffer = FS_Read(filepath, &_csize, &ret);
			if(ret < 0) {
				printf("failed! (read, %d)\n", ret);
				ES_AddTitleCancel();
				return quit(ret);
			}

			ret = ES_AddContentStart(p_tmd->title_id, cid);
			if(ret < 0) {
				printf("failed! (ES_AddContentStart -> %d)\n", ret);
				ES_AddTitleCancel();
				return quit(ret);
			}
			cfd = ret;

			aeskey encrypt_iv ATTRIBUTE_ALIGN(32) = {};
			encrypt_iv[1] = i & 0xFF;
			AES_ctx_set_iv(&AESctx_title, encrypt_iv);

			putc('\n', stdout);

			unsigned int j = 0;
			while( j < csize ) {
				unsigned int z = ALIGN(
					(( csize - j > enc_buf_sz ) ? enc_buf_sz : ( csize - j )),
					32);

				memcpy(enc_buf, buffer + j, z);
				AES_CBC_encrypt_buffer(&AESctx_title, enc_buf, z);
				printf("\r%u/%u bytes / %.2f%% ... ", j + z, csize, ((double)(j + z) / align_csize) * 100);
				ret = ES_AddContentData(cfd, enc_buf, z);
				if (ret < 0) {
					printf("error! (%d)\n", ret);
					ES_AddContentFinish(cfd);
					ES_AddTitleCancel();
					return quit(ret);
				}
				j += z;
			}
		} else {
			printf(">> Downloading content id %08x... ", cid);
			char cidstr[9]; // muh \x00
			unsigned int __csize = 0; _csize = ALIGN(csize, 0x20);
			sprintf(cidstr, "%08x", cid);

			buffer = NUS_Download(tid, cidstr, &__csize, &ret);
			if(ret < 0) {
				printf("failed! (%d)\n", ret);
				ES_AddTitleFinish();
				return quit(ret);
			}

			printf("OK! (size = %u)\n", __csize);

			printf(">> Installing... ");
			ret = ES_AddContentStart(p_tmd->title_id, cid);
			if(ret < 0) {
				printf("failed! (%d)\n", ret);
				ES_AddTitleCancel();
				return quit(ret);
			}
			cfd = ret;

			ret = ES_AddContentData(cfd, buffer, _csize);
			if (ret < 0) {
				printf("failed! (%d)\n", ret);
				ES_AddContentFinish(cfd);
				ES_AddTitleCancel();
				return quit(ret);
			}
		}

		free(buffer);
		ret = ES_AddContentFinish(cfd);
		if (ret < 0) {
			printf("failed! (%d)\n", ret);
			ES_AddTitleCancel();
			return quit(ret);
		}
		printf("OK!\n\n");

	}
	printf("> Finishing installation... ");
	ret = ES_AddTitleFinish();
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		ES_AddTitleCancel();
		return quit(ret);
	}
	printf("OK!\n");

	printf("\x1b[42m All done! \x1b[40m\n");
	return quit(0);
}
