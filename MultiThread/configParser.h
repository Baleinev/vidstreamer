
#define CONF_FILE_MAXLENGTH (1024*1024*4)

#include "screenStreamerMulti.h"

bool parseConfig(const char *configFile,globalConfig_t *globalConfig);

void printConfig(globalConfig_t *globalConfig);
