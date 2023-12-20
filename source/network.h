#include <stddef.h>
#include <wiisocket.h>

typedef struct {
	void* ptr;
	size_t size;
} blob;

typedef enum {
	DOWNLOAD_BLOB,
	DOWNLOAD_FILE,
	DOWNLOAD_CUSTOM,
} DownloadType;

// TODO: Where is this?
extern long gethostid(void);

int network_init();
char* PrintIPAddress();
void network_deinit();
int DownloadFile(char* url, DownloadType, void*, void*);
const char* GetLastDownloadError();
