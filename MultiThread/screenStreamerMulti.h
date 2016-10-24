#ifndef SCREENSTREAMERMULTI_H
#define SCREENSTREAMERMULTI_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <x264.h>

#define DBG(fmt,...) do{fprintf(stdout,"[%s][%s][%d]",__FILE__,__FUNCTION__,__LINE__);fprintf(stdout,fmt,##__VA_ARGS__);fprintf(stdout,"\n");fflush(stdout);}while(0);
#define ERR(fmt,...) do{fprintf(stderr,"[%s][%s][%d]",__FILE__,__FUNCTION__,__LINE__);fprintf(stderr,fmt,##__VA_ARGS__);fprintf(stderr,"\n");fflush(stderr);}while(0);
#define LOG(fmt,...) do{fprintf(stdout,"[%s][%s][%d]",__FILE__,__FUNCTION__,__LINE__);fprintf(stdout,fmt,##__VA_ARGS__);fprintf(stdout,"\n");fflush(stdout);}while(0);

#define MAX_UDP_SIZE 1472
#define CONFIG_FILEPATH_MAXLENGTH 128
#define CONFIG_DISPLAYNAME_MAXLENGTH 128
#define IPSTRING_MAXLENGTH 128
#define INTERFACENAME_MAXLENGTH 128

typedef struct streamerConfig_t
{
  unsigned int offsetX;
  unsigned int offsetY;
  unsigned int sizeX;
  unsigned int sizeY;
  char ip[IPSTRING_MAXLENGTH];
  unsigned int port;
  char interface[INTERFACENAME_MAXLENGTH];
  unsigned int bufferSize;
  float hardFpsLimiter;
  x264_param_t x264params;
  cpu_set_t affinity;


} streamerConfig_t;


typedef struct grabberConfig_t
{
  float hardFpsLimiter;
  bool waitForAll;
  unsigned int nbStreamers;
  cpu_set_t affinity;

} grabberConfig_t;


typedef struct globalConfig_t
{
  grabberConfig_t grabber;
  streamerConfig_t *streamers;

} globalConfig_t;

#endif