#ifndef SCREENSTREAMERMULTI_H
#define SCREENSTREAMERMULTI_H

#include <stdio.h>
#include <stdint.h>

#include <x264.h>

#define DBG(fmt,...) do{/*fprintf(stdout,"[%s][%s][%d]",__FILE__,__FUNCTION__,__LINE__);fprintf(stdout,fmt,##__VA_ARGS__);fprintf(stdout,"\n");fflush(stdout);*/}while(0);

#define ERR(fmt,...) do{fprintf(stderr,"[%s][%s][%d]",__FILE__,__FUNCTION__,__LINE__);fprintf(stderr,fmt,##__VA_ARGS__);fprintf(stderr,"\n");fflush(stderr);}while(0);

#define LOG(fmt,...) do{fprintf(stdout,"[%s][%s][%d]",__FILE__,__FUNCTION__,__LINE__);fprintf(stdout,fmt,##__VA_ARGS__);fprintf(stdout,"\n");fflush(stdout);}while(0);

#define MAX_UDP_SIZE 1472

#define MAX_ENCODERS 16

dumpRGBAjpeg(unsigned char *data,unsigned int width,unsigned int height,const char *name);


typedef struct streamerConfig_t
{

  unsigned int xOffset;
  unsigned int yOffset;
  
  unsigned int width;
  unsigned int height;

  x264_param_t x264param;

  unsigned char targetHost[128];
  unsigned char interface[32];

  unsigned int targetPort;

} streamerConfig_t;

#endif