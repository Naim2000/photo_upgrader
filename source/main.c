#define VERSION "0.1.0"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/isfs.h>
#include <ogc/es.h>
#include <ogc/machine/processor.h>
#include <network.h>
#include <libpatcher/libpatcher.h>

#include "tools.h"
#include "sha1.h"
#include "aes.h"
#include "http.h"

#define ALIGN(a,b) ((((a)+(b)-1)/(b))*(b))

const char header[] = "Photo Channel 1.1 installer v" VERSION ", by thepikachugamer\n\n";
static fstats file_stats ATTRIBUTE_ALIGN(32);
bool offline_mode = true;

#define syscall(id)				(0xE6000010 | (id << 5))
#define SC_SETUID				0x2b
#define SC_INVALIDATEDCACHE		0x3f
#define SC_FLUSHDCACHE			0x40



void* NUS_Download(const uint64_t tid, char* obj, unsigned int* size, int* ec) {
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

void hexdump(void* ptr, unsigned int cnt) {
	printf("%p ", ptr);
	for(unsigned int i = 0; i < cnt; i++) {
		printf( "%02x ", ((unsigned char*)ptr)[i] );
		if(! ((i+1) % 16) && i != cnt)
			printf("\n%p ", ((unsigned char*)ptr) + i + 1);
	}

}

int main() {
	uint32_t armCode[] = {
	/* 0x00 */ 0xEA000000, // b       0x8
	/* 0x04 */ 0x00000000, // MESSAGE_VALUE
	// Set PPC UID to root
	/* 0x08 */ 0xE3A0000F, // mov     r0, #15
	/* 0x0C */ 0xE3A01000, // mov     r1, #0
	/* 0x10 */ syscall(SC_SETUID),
	// Send response to PPC
	/* 0x14 */ 0xE24F0018, // adr     r0, MESSAGE_VALUE
	/* 0x18 */ 0xE3A01001, // mov     r1, #1
	/* 0x1C */ 0xE5801000, // str     r1, [r0]
	// Flush the response to main memory
	/* 0x20 */ 0xE3A01004, // mov     r1, #4
	/* 0x24 */ syscall(SC_FLUSHDCACHE),
	// Wait for response back from PPC
	// loop_start:
	/* 0x28 */ 0xE24F002C, // adr     r0, MESSAGE_VALUE
	/* 0x2C */ 0xE5902000, // ldr     r2, [r0]
	/* 0x30 */ 0xE3520002, // cmp     r2, #2
	/* 0x34 */ 0x0A000001, // beq     loop_break
	/* 0x38 */ syscall(SC_INVALIDATEDCACHE),
	/* 0x3C */ 0xEAFFFFF9, // b       loop_start
	// loop_break:
	// Reset PPC UID back to 15
	/* 0x40 */ 0xE3A0000F, // mov     r0, #15
	/* 0x44 */ 0xE3A0100F, // mov     r1, #15
	/* 0x48 */ syscall(SC_SETUID),
	// Send response to PPC
	/* 0x4C */ 0xE24F0050, // adr     r0, MESSAGE_VALUE
	/* 0x50 */ 0xE3A01003, // mov     r1, #3
	/* 0x54 */ 0xE5801000, // str     r1, [r0]
	// Flush the response to main memory
	/* 0x58 */ 0xE3A01004, // mov     r1, #4
	/* 0x5C */ syscall(SC_FLUSHDCACHE),
	/* 0x60 */ 0xE12FFF1E, // bx      lr
};

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
		_cnt = 0,
		keynum = 0;

	signed_blob
		*s_certs = NULL,
		*s_tmd = NULL,
		*s_tik = NULL;

	struct AES_ctx _aes_ctx;

	init_video(2, 0);
	printf(header);

	printf("Applying IOS patches... ");
	if (!apply_patches()) {
		printf("failed! Is your Homebrew Channel updated?\n"
			"Is <ahb_access/> in meta.xml?" "\n\n"
			"Exiting in 5s...");
		sleep(5);
		return 0x0D800064;
	}
	printf("OK!\n\n");

	WPAD_Init();
	PAD_Init();
	ISFS_Initialize();

	ES_GetTitleContentsCount(tid, &cnt);

	ret = ISFS_Open("/ticket/00010000/48415a41.tik", 0);
	if (ret > 0) {
		ISFS_Close(ret);
		ret = ES_GetTitleContentsCount(tid_stub, &_cnt);
		if (!ret)
			printf("Photo channel 1.1 stub is already installed.\n");
		else
			printf("This console owns the Photo Channel 1.1 stub.\n" "Maybe you're looking for the Wii Shop Channel?\n");

//		return quit(0);
	}

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
		printf("\x1b[41m This is Dolphin Emulator, fakesigning doesn't work! \x1b[40m\n");

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

	if(!check_dolphin()) {
		printf("\x1b[43m> Identifying... \x1b[40m");

		ret = ES_Identify(s_certs, certs_size, s_tmd, tmd_size, s_tik, STD_SIGNED_TIK_SIZE, &keynum);
		if(ret < 0) {
			printf("failed! (%d)\n", ret);
			return quit(ret);
		}
		printf("OK! keynum = %u\n", keynum);

		printf("\x1b[43m> Escalating priveleges... \x1b[40m");

		int shaFd = IOS_Open("/dev/sha", 0);
		if(!shaFd) {
			printf("failed! (open, %d)\n", ret);
			return quit(ret);
		}

		int shaHeap = iosCreateHeap(0x50);
		if(shaHeap < 0) {
			printf("failed! (iosCreateHeap() -> %d)\n", ret);
			return quit(ret);
		}

		uint32_t* MEM1 = (void*) SYS_BASE_CACHED;
		*MEM1++ = 0x4903468D; // ldr r1, =0x10100000; mov sp, r1;
		*MEM1++ = 0x49034788; // ldr r1, =entrypoint; blx r1;
		// Overwrite reserved handler to loop infinitely
		*MEM1++ = 0x49036209; // ldr r1, =0xFFFF0014; str r1, [r1, #0x20];
		*MEM1++ = 0x47080000; // bx r1
		*MEM1++ = 0x10100000; // temporary stack
		*MEM1++ = MEM_VIRTUAL_TO_PHYSICAL(armCode);
		*MEM1++ = 0xFFFF0014; // reserved handler

		ioctlv* ACE = iosAlloc(shaHeap, sizeof(ioctlv) * 4);
		if (!ACE) {
			printf("heap too small?\n");
			return quit(-ENOMEM);
		}

		ACE[0].data = NULL;
		ACE[0].len  = 0;

		ACE[1].data = (void*) 0xFFFE0028;
		ACE[1].len  = 0;

		ACE[2].data = (void*) SYS_BASE_CACHED;
		ACE[2].len  = 0x40;


		ret = IOS_Ioctlv(shaFd, 0, 1, 2, ACE);
		if(ret < 0) {
			printf("failed! (ioctlv -> %d)\n", ret);
			return ret;
		}

		int msg = 0;
		while(msg != 1) {
			msg = read32((uint32_t)(armCode + 1));
			putc(0x30 + msg, stdout);
		}
		fflush(stdout);
		printf("\nnow bear with me ES...\n");

	}

	printf("> Changing Title ID... ");

	/*
	aeskey
		keyout	ATTRIBUTE_ALIGN(0x20),
		keyin	ATTRIBUTE_ALIGN(0x20),
		iv		ATTRIBUTE_ALIGN(0x20),
		tkey	ATTRIBUTE_ALIGN(0x20); // probably don't need this
	*/
	int heapOfKeys = iosCreateHeap(0x20 * 16); // ?
	if (heapOfKeys < 0) {
		printf("failed? (%d)\n", ret);
		return quit(ret);
	}

	aeskey* keychain = iosAlloc(heapOfKeys, sizeof(aeskey) * 8);
	if(!keychain) {
		printf("heap too small?\n");
		return quit(-ENOMEM);
	}
	printf("\x1b[44m keychain @ %p~%p \x1b[40m\n", keychain, keychain + 8);

	/*
	memcpy(keyin, p_tik->cipher_title_key, sizeof(aeskey));
	memset(iv, 0, sizeof(aeskey));
	memcpy(iv, &p_tik->titleid, sizeof(p_tik->titleid));
	memset(keyout, 0, sizeof(aeskey));
	*/
	memset(keychain, 0, sizeof(aeskey) * 8);
	memcpy(keychain[0], p_tik->cipher_title_key, sizeof(aeskey));
	memcpy(keychain[2], &p_tik->titleid, sizeof(uint64_t));

	hexdump(keychain, sizeof(aeskey) * 8);

	ret = ES_Decrypt(ES_KEY_COMMON, keychain[2], keychain[0], sizeof(aeskey), keychain[4]);
	printf("\n\x1b[44m ES_Decrypt(%d, %p, %p, %u, %p) -> %d \x1b[40m\n",
		   ES_KEY_COMMON, keychain[2], keychain[0], sizeof(aeskey), keychain[4], ret);

	if (ret < 0) {
		printf("failed! (decrypt, %d)\n", ret);
		return quit(ret);
	}
	memcpy(keychain[1], keychain[4], sizeof(aeskey));
	AES_init_ctx(&_aes_ctx, keychain[1]);

	p_tik->titleid = tid_new;
	memset(keychain[2], 0, sizeof(aeskey));
	memcpy(keychain[2], &p_tik->titleid, sizeof(uint64_t));

	ret = ES_Encrypt(ES_KEY_COMMON, keychain[2], keychain[4], sizeof(aeskey), keychain[0]);
	if (ret < 0) {
		printf("failed! (encrypt, %d)\n", ret);
		return quit(ret);
	}
	memcpy(p_tik->cipher_title_key, keychain[0], sizeof(aeskey));

	p_tmd->title_id = tid_new;
	printf("OK!\n");

	if(!check_dolphin()) {
		printf("Thank you /dev/sha ...\n");
		write32((uint32_t)(armCode + 1), 2);
		while(read32((uint32_t)(armCode + 1)) != 3);
	}

	return quit(0);

	if(check_vwii()) {
		printf("* This seems to be a vWii, setting IOS to 58.\n");
		p_tmd->sys_version = 0x000000010000003ALL;
	}

	/*
	 * no silly strcmp anymore. iosc just says ok
	 * ily noahpistilli
	printf("> Faking signatures... ");

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

	printf("> Installing ticket... ");
	ret = ES_AddTicket(s_tik, STD_SIGNED_TIK_SIZE, s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		printf("failed! (%d)\n", ret);
		return quit(ret);
	}
	printf("OK!\n");

	printf("> Starting title installation...\n");

	printf(">> Installing TMD... ");
	ret = ES_AddTitleStart(s_tmd, SIGNED_TMD_SIZE(s_tmd), s_certs, certs_size, NULL, 0);
	if (ret < 0) {
		ES_AddTitleCancel();
		printf("failed! (%d)\n", ret);
		return quit(ret);
	}
	printf("OK!\n");

	for(unsigned short i = 0; i < p_tmd->num_contents; i++) {
		tmd_content* content = &p_tmd->contents[i];
		unsigned int
			cid = content->cid,
			csize = content->size,
			_csize = 0,
			enc_buf_sz = 1024;

		unsigned char
			*buffer = NULL,
			*enc_buf = NULL;

		int cfd;

		if (offline_mode) {
			printf(">> Installing content #%d... ", i);
			if(content->type == 0x8001) { // // content->type & ~1 (probably bad), content->type != 1
				printf("Skipped. (This is a shared content.)\n");
				continue;
			}

			char filepath[128];
			unsigned int _csize;
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

			enc_buf = memalign(0x20, enc_buf_sz);
			if(!enc_buf) {
				printf("failed! (No memory! [%u])\n", csize);
				ES_AddContentFinish(cfd);
				ES_AddTitleCancel();
				return quit(-ENOMEM);
			}

			aeskey encrypt_iv ATTRIBUTE_ALIGN(32);
			memset(encrypt_iv, 0, sizeof(encrypt_iv));
			*(uint16_t*)encrypt_iv = i;
			AES_ctx_set_iv(&_aes_ctx, encrypt_iv);

			putc('\n', stdout);

			unsigned int j = 0;
			while( j < csize ) {
				unsigned int z = ALIGN(
					(( csize - j > enc_buf_sz ) ? enc_buf_sz : ( csize - j )),
					32);

				memcpy(enc_buf, buffer + j, z);
				AES_CBC_encrypt_buffer(&_aes_ctx, enc_buf, z);
				printf("\r%u/%u bytes / %.2f%% ... ", j + z, csize, ((double)(j + z) / csize) * 100);
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
			printf("OK!\n");

		}

		free(buffer);
		free(enc_buf);
		ret = ES_AddContentFinish(cfd);
		if (ret < 0) {
			printf("failed! (%d)\n", ret);
			ES_AddTitleCancel();
			return quit(ret);
		}
		printf("OK!\n");

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
