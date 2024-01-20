#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ogc/lwp_watchdog.h>
#include <curl/curl.h>
#include <arpa/inet.h>

static int network_up = false;
static char ebuffer[CURL_ERROR_SIZE] = {};

typedef size_t (*fwrite_wannabe)(void*, size_t, size_t, void*);
typedef struct xferinfo_data_s {
	u64 start;
} xferinfo_data;

char* PrintIPAddress() {
	uint32_t ipaddr = gethostid();
	static char ipstr[16] = {};
	if (inet_ntop(AF_INET, &ipaddr, ipstr, sizeof(ipstr)) < 0)
		return NULL;

	return ipstr;
}

static size_t WriteToBlob(void* buffer, size_t size, size_t nmemb, void* userp) {
	size_t length = size * nmemb;
	blob* blob = userp;

	// If ptr is NULL, then the call is equivalent to malloc(size), for all values of size.
	unsigned char* _buffer = realloc(blob->ptr, blob->size + length);
	if (!_buffer) {
		printf("WriteToBlob: out of memory (%zu + %zu bytes)\n", blob->size, length);
		free(blob->ptr);
		blob->ptr = NULL;
		blob->size = 0;
		return 0;
	}

	blob->ptr = _buffer;
	memcpy(blob->ptr + blob->size, buffer, length);
	blob->size += length;

	return length;
}

static int xferinfo_cb(void* userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
	xferinfo_data* data = userp;

	if (!dltotal)
		return 0;

	if (!data->start)
		data->start = gettime();

	u64 now = gettime();
	u32 elapsed = diff_sec(data->start, now);

	float f_dlnow = dlnow / 1024.f;
	float f_dltotal = dltotal / 1024.f;

	printf("\r%.2f/%.2f KB // %.2f KB/s...",
		  f_dlnow, f_dltotal, f_dlnow / elapsed);

	return 0;
}

int network_init() {
	if ((network_up = wiisocket_get_status()) > 0)
		return 0;

	int ret = wiisocket_init();
	if (ret >= 0) {
		network_up = true;
		curl_global_init(0);
	}

	return ret;
}

void network_deinit() {
	if (network_up) {
		wiisocket_deinit();
		curl_global_cleanup();
		network_up = false;
	}
}

int DownloadFile(char* url, DownloadType type, void* data, void* userp) {
	CURL* curl;
	CURLcode res;
	xferinfo_data xferdata = {};

	curl = curl_easy_init();
	if (!curl)
		return -1;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, ebuffer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xferdata);
	switch (type) {
		case DOWNLOAD_BLOB:
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToBlob);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
			break;
		case DOWNLOAD_FILE:
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
			break;
		case DOWNLOAD_CUSTOM:
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (fwrite_wannabe)data);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, userp);
			break;
	}
	ebuffer[0] = '\x00';
	printf("\x1b[30;1mDownloading %s\x1b[39m\n", url);
	res = curl_easy_perform(curl);
	putchar('\n');
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		if (!ebuffer[0])
			strcpy(ebuffer, curl_easy_strerror(res));

		return -res;
	}

	return res;
}

const char* GetLastDownloadError() {
	return ebuffer;
}


