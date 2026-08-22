#ifndef NE_DEVICEIDS_H
#define NE_DEVICEIDS_H
#include <stddef.h>

/* netease-music util/deviceid.go pool (24641 ids, 52 uppercase hex chars
 * each) embedded as one fixed-stride blob: id i = NE_DEVICE_IDS_BLOB+i*52.
 * request.go picks globalDeviceId = deviceIds[rand.Intn(len-1)] ONCE per
 * process — never the last element (Go quirk). */
extern const char NE_DEVICE_IDS_BLOB[];

size_t ne_device_ids_count(void);
/* pointer into the blob for id i (NOT nul-terminated; 52 chars) */
const char *ne_device_id_at(size_t i);
/* rand.Intn(len-1) pick, malloc'd nul-terminated copy */
char *ne_random_device_id(void);
#endif
