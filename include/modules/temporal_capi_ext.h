#ifndef ANT_TEMPORAL_CAPI_EXT_H
#define ANT_TEMPORAL_CAPI_EXT_H

#ifndef _WIN32
#include "Provider.d.h"

#ifdef __cplusplus
extern "C" {
#endif

Provider *temporal_rs_Provider_new_fs(void);

#ifdef __cplusplus
}
#endif
#endif

#endif
