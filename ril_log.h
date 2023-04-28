#include <cutils/log.h>

#ifndef LOGD
#if defined RLOGD
#define LOGD RLOGD
#define LOGE RLOGE
#define LOGI RLOGI
#define LOGW RLOGW
#define LOGV RLOGV
#else
#define LOGD ALOGD
#define LOGE ALOGE
#define LOGI ALOGI
#define LOGW ALOGW
#define LOGV ALOGV
#endif
#endif
