#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <errno.h>
// #include <sys/param.h>
#include <ogc/es.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>
#include <ogc/isfs.h>

#include "network.h"
#include "nus.h"

#define MIN(a, b) __extension__({         \
	__typeof__(a) x = (a);   \
	__typeof__(b) y = (b);   \
	x > y ? x : y;           \
})

#define roundup(val, by) \
	(val + (by - 1)) & ~(by - 1)

#define NUS_SERVER "nus.cdn.shop.wii.com"

#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

void* memalign(size_t, size_t);
static const aeskey wii_ckey = {0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4, 0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7};

typedef union {
	uint16_t index;
	int64_t tid;
	uint64_t part[2];

	aeskey full;
} aesiv;

/*
static size_t ES_DownloadContentData(void* buffer, size_t size, size_t nmemb, void* userp) {
	int cfd = *(int*)userp;
	size_t len = size * nmemb;

	if ((uintptr_t)buffer & 0x1F) {
		void* temp = memalign(0x20, len); iosc what the hell do you mean unaligned data
		if (!temp)
			return CURL_WRITEFUNC_ERROR;

		memcpy(temp, buffer, len);
		ES_AddContentData(cfd, temp, len);
		free(temp);
	}
	else
		ES_AddContentData(cfd, buffer, len);

	return len;
}
*/

int GetInstalledTitle(int64_t titleID, struct Title* title) {
	int ret;
	void* buffer = NULL;

	*title = (struct Title){};

	uint32_t tmd_size = 0;
	ret = ES_GetStoredTMDSize(titleID, &tmd_size);
	if (ret < 0 || !tmd_size)
		goto error;

	buffer = memalign(0x20, tmd_size);
	if (!buffer)
		goto error;

	ret = ES_GetStoredTMD(titleID, buffer, tmd_size);
	if (ret < 0)
		goto error;

	title->s_tmd = buffer;
	title->tmd_size = tmd_size;
	title->tmd = SIGNATURE_PAYLOAD(title->s_tmd);

	char filepath[30];
	sprintf(filepath, "/ticket/%08x/%08x.tik", (uint32_t)(titleID >> 32), (uint32_t)(titleID & 0xFFFFFFFF));
	int fd = ret = ISFS_Open(filepath, ISFS_OPEN_READ);
	if (ret < 0)
		goto error;

	buffer = memalign(0x20, STD_SIGNED_TIK_SIZE);
	if (!buffer)
		goto error;

	ret = ISFS_Read(fd, buffer, STD_SIGNED_TIK_SIZE);
	ISFS_Close(fd);

	if (ret != STD_SIGNED_TIK_SIZE) {
		ret = -EIO;
		goto error;
	}

	title->s_tik = buffer;
	title->tik_size = STD_SIGNED_TIK_SIZE;
	title->ticket = SIGNATURE_PAYLOAD(title->s_tik);

	mbedtls_aes_context aes = {};
	aesiv iv = {};

	iv.tid = title->ticket->titleid;
	mbedtls_aes_setkey_dec(&aes, wii_ckey, sizeof(aeskey) * 8);
	mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, sizeof(aeskey), iv.full, title->ticket->cipher_title_key, title->key);

	uint32_t viewcount = 0;
	ret = ES_GetNumTicketViews(titleID, &viewcount);
	if (ret < 0 || !viewcount)
		goto error;

	buffer = memalign(0x20, sizeof(tikview) * viewcount);
	if (!buffer) {
		ret = -ENOMEM;
		goto error;
	}

	ret = ES_GetTicketViews(titleID, buffer, viewcount);
	if (ret < 0)
		goto error;

	title->views = buffer;

	title->id = titleID;
	title->local = true;
	return 0;

error:
	free(buffer);
	return ret;
}

int DownloadTitleMeta(int64_t titleID, int titleRev, struct Title* title) {
	int ret;
	char url[0x80];
	blob tmd = {}, cetk = {};

	sprintf(url, "%s/ccs/download/%016llx/", NUS_SERVER, titleID);

	if (titleRev > 0 && !(titleRev & 0xFFFF0000))
		sprintf(strrchr(url, '/'), "/tmd.%hu", (uint16_t)titleRev);
	else
		strcpy (strrchr(url, '/'), "/tmd");

	ret = DownloadFile(url, DOWNLOAD_BLOB, &tmd, NULL);
	if (ret < 0)
		return ret;

	title->s_tmd = tmd.ptr;
	title->tmd_size = tmd.size;
	title->tmd = SIGNATURE_PAYLOAD(title->s_tmd);

	sprintf(strrchr(url, '/'), "/cetk");
	ret = DownloadFile(url, DOWNLOAD_BLOB, &cetk, NULL);
	if (ret < 0)
		return ret;

	title->s_tik = cetk.ptr;
	title->tik_size = cetk.size;
	title->ticket = SIGNATURE_PAYLOAD(title->s_tik);

	mbedtls_aes_context aes = {};
	aesiv iv = {};

	iv.tid = title->ticket->titleid;
	mbedtls_aes_setkey_dec(&aes, wii_ckey, sizeof(aeskey) * 8);
	mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, sizeof(aeskey), iv.full, title->ticket->cipher_title_key, title->key);

	title->id = titleID;

	return 0;
}

void ChangeTitleID(struct Title* title, int64_t new) {
	mbedtls_aes_context aes = {};
	aesiv iv = {};

	iv.tid = new;
	mbedtls_aes_setkey_enc(&aes, wii_ckey, sizeof(aeskey) * 8);
	mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, sizeof(aeskey), iv.full, title->key, title->ticket->cipher_title_key);
	title->tmd->title_id = new;

	title->ticket->titleid = new;
}

static int PurgeTitle(int64_t titleid) {
	int ret;
	uint32_t viewcnt = 0;
	tikview* views, view ATTRIBUTE_ALIGN(0x20);

	ret = ES_GetNumTicketViews(titleid, &viewcnt);
	if (ret < 0)
		return ret;

	if (!viewcnt)
		return ENOENT;

	views = memalign(0x20, sizeof(tikview) * viewcnt);
	if (!views)
		return -ENOMEM;

	ret = ES_GetTicketViews(titleid, views, viewcnt);
	if (ret < 0)
		return ret;

	for (int i = 0; i < viewcnt; i++) {
		view = views[i];
		ret = ES_DeleteTicket(&view);
		if (ret < 0)
			return ret;
	}
	free(views);

	ES_DeleteTitleContent(titleid);
	return ES_DeleteTitle(titleid);
}

int InstallTitle(struct Title* title, bool purge) {
	int ret;
	signed_blob* s_buffer = NULL;
	signed_blob* certs = NULL;
	size_t certs_size;
	fstats isfs_fstats ATTRIBUTE_ALIGN(0x20) = {};

	int fd = ret = ISFS_Open("/sys/cert.sys", ISFS_OPEN_READ);
	if (ret < 0)
		return ret;

	ret = ISFS_GetFileStats(fd, &isfs_fstats);
	if (ret < 0)
		return ret;

	certs_size = isfs_fstats.file_length;
	certs = memalign(0x20, certs_size);
	if (!certs)
		return -ENOMEM;

	ret = ISFS_Read(fd, certs, certs_size);
	ISFS_Close(fd);
	if (ret < 0)
		goto finish;

	if (purge) {
		ret = PurgeTitle(title->tmd->title_id);
		if (ret < 0)
			goto finish;
	}

	size_t
		tiksize = STD_SIGNED_TIK_SIZE,
		tmdsize = SIGNED_TMD_SIZE(title->s_tmd),
		bufsize = MIN(tmdsize, tiksize);

	s_buffer = memalign(0x20, bufsize);
	if (!s_buffer) {
		ret = -ENOMEM;
		goto finish;
	}

	memcpy(s_buffer, title->s_tik, tiksize);
	ret = ES_AddTicket(s_buffer, tiksize, certs, certs_size, NULL, 0);
	if (ret < 0)
		goto finish;

	memcpy(s_buffer, title->s_tmd, tmdsize);
	ret = ES_AddTitleStart(s_buffer, tmdsize, certs, certs_size, NULL, 0);
	if (ret < 0)
		goto finish;

	free(s_buffer);
	s_buffer = NULL;

	for (int i = 0; i < title->tmd->num_contents; i++) {
		tmd_content* content = title->tmd->contents + i;
		char url[100];

		if (content->type == 0x8001 && title->local)
			continue;

		int cfd = ret = ES_AddContentStart(title->tmd->title_id, content->cid);
		if (ret < 0)
			break;

		if (title->local) {
			/* ES DEVOPTAB!!!! LETS FUCKING GO!!! // <-- me when the ES_GetTitleID() fails
			FILE* contentfp = fopen("es:%016llx/ID%08x", "rb");
			if (!contentfp) {
				perror("ES devoptab failed :((\n");
				ret = -errno;
				break;
			}
			*/

			size_t align_csize = roundup(content->size, 0x10);
			void* buffer = memalign(0x20, align_csize);
			if (!buffer) {
				ret = -ENOMEM;
				break;
			}

			memset(buffer + align_csize - 0x10, 0, 0x10);
			/*
			size_t read = fread(buffer, 1, content->size, contentfp);
			fclose(contentfp);
			if (read < content->size) {
				perror("ES devoptab failed :((\n");
				ret = -errno;
				break;
			}
			*/

			int s_cfd = ret = ES_OpenTitleContent(title->id, title->views, content->index);
			if (ret < 0)
				break;

			ret = ES_ReadContent(s_cfd, buffer, content->size);
			ES_CloseContent(s_cfd);

			if (ret < 0)
				break;

			mbedtls_aes_context aes = {};
			aesiv iv = {};

			iv.index = content->index;
			mbedtls_aes_setkey_enc(&aes, title->key, sizeof(aeskey) * 8);
			mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, align_csize, iv.full, buffer, buffer);

			ret = ES_AddContentData(cfd, buffer, align_csize);
			free(buffer);
		}
		else {
			blob cdownload = { memalign(0x20, 0x20) };
			void* buffer = NULL;

			sprintf(url, "%s/ccs/download/%016llx/%08x", NUS_SERVER, title->id, content->cid);
			/* ret = DownloadFile(url, DOWNLOAD_CUSTOM, ES_DownloadContentData, &cfd); */
			ret = DownloadFile(url, DOWNLOAD_BLOB, &cdownload, NULL);
			if (ret != 0)
				break;

			if ((uintptr_t)cdownload.ptr & 0x1F) {
				buffer = memalign(0x20, cdownload.size);
				if (!buffer) {
					ret = -ENOMEM;
					break;
				}

				memcpy(buffer, cdownload.ptr, cdownload.size);
				free(cdownload.ptr);
			}
			else {
				buffer = cdownload.ptr;
			}

			ret = ES_AddContentData(cfd, buffer, cdownload.size);
			free(buffer);
		}

		ret = ES_AddContentFinish(cfd);
		if (ret < 0)
			break;

	}

	if (!ret)
		ret = ES_AddTitleFinish();

	if (ret < 0)
		ES_AddTitleCancel();

finish:
	free(s_buffer);
	free(certs);
	return ret;
}

static inline void zero_sig(signed_blob* blob) {
	memset(SIGNATURE_SIG(blob), 0, SIGNATURE_SIZE(blob) - 4);
}

bool Fakesign(struct Title* title) {
	sha1 hash;
	zero_sig(title->s_tik);
	zero_sig(title->s_tmd);

	for (uint16_t i = 0; i < 0xFFFFu; i++) {
		title->ticket->padding = i;
		mbedtls_sha1_ret(title->ticket, sizeof(tik), hash);
		if (!hash[0]) break;
	}
	if (hash[0]) return false;

	for (uint16_t i = 0; i < 0xFFFFu; i++) {
		title->tmd->fill3 = i;
		mbedtls_sha1_ret(title->tmd, TMD_SIZE(title->tmd), hash);
		if (!hash[0]) break;
	}
	if (hash[0]) return false;

	return true;
}

void FreeTitle(struct Title* title) {
	if (!title) return;
	free(title->s_tmd);
	free(title->s_tik);
	free(title->views);
}
