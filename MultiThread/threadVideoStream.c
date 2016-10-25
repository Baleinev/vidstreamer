#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/shm.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <libswscale/swscale.h>

#include <x264.h>

#include "screenStreamerMulti.h"

extern unsigned int screenWidth;
extern unsigned int screenHeight;

extern unsigned int bytesPerPixelSrc;
extern unsigned int bytesPerLineSrc;

extern bool flagQuit;
extern bool flagIntra;

extern char *sharedFrame;

extern bool flagDataAvailable;
extern unsigned int memcopyDone;
extern unsigned int frameId;

extern pthread_cond_t condDataAvailable;
extern pthread_cond_t condDataConsummed;

extern pthread_mutex_t mutexCapturedFrame;

void *threadVideoStream(void * param)
{
  struct streamerConfig_t *config = (struct streamerConfig_t *)param;

  unsigned int curFrameId = 0;
  
  int frameSize;

  struct timeval now,last,timeWait,timeMemcpy,timeEncoding,timeSend,timeScaling;

  char *croppedFrame = NULL;

  /*
   * Those pictures will be reused across the encoder, so we don't need to free them
   */
  x264_picture_t pic_in, pic_out;

  x264_t* encoder;
  x264_nal_t* nals;
  int i_nals;

  struct SwsContext *convertCtx; 
  int srcstride;   

  int alreadySent = 0;
  int sent = 0;

  struct sockaddr_in si_other;
  int sendingSocket, slen = sizeof(si_other);

  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &(config->affinity));        

  if ((sendingSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
  {
    ERR("Cannot open sending socket:%d errno:%d",sendingSocket,errno);
    goto FAIL_SOCKET;
  }

  if(setsockopt(sendingSocket, SOL_SOCKET, SO_SNDBUF, &(config->bufferSize), sizeof(config->bufferSize)) < 0)
  {
    ERR("Cannot setsockopt. Non-fatal, but the latency will suffer if the UDP send buffer is too big.");
  }

  if(setsockopt(sendingSocket, SOL_SOCKET, SO_BINDTODEVICE, config->interface, strlen(config->interface)) < 0)
  {
      ERR("Cannot bind to selected interface: %s",config->interface);
      goto FAIL_SOCKET_INTERFACE;
  }  

  memset((char *) &si_other, 0, sizeof(si_other));
  si_other.sin_family = AF_INET;
  si_other.sin_port = htons(config->port);

  LOG("Sending to %s:%d",config->ip,config->port);
   
  if (inet_aton(config->ip , &si_other.sin_addr) == 0)
  {
    ERR("Cannot convert string to ip: %s",config->ip);
    goto FAIL_INET_ATON;
  }


  if((encoder = x264_encoder_open(&(config->x264params))) == NULL)
  {
    ERR("Cannot open encoder: %ld",(long unsigned int)encoder);
    goto FAIL_ENCODER;
  }


  if(x264_picture_alloc(&pic_in, X264_CSP_I420, config->sizeX, config->sizeY) < 0)
  {
    ERR("Cannot allocate picture");
    goto FAIL_ALLOC;
  }
  ERR("Allocated picture of %d x %d",config->sizeX,config->sizeY);

  // x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);

  /*
   * Init filter from swslib to YUV420 from RGBA. The alpha component is not used.
   */
  if((convertCtx = sws_getContext(
    config->sizeX,
    config->sizeY,
    AV_PIX_FMT_BGRA,
    config->sizeX,
    config->sizeY,
    PIX_FMT_YUV420P,
    SWS_FAST_BILINEAR, // Not the best, but the fastest
    NULL,
    NULL,
    NULL)) == NULL)
  {
    ERR("Cannot get filtering context: %ld",(long unsigned int)convertCtx);
    goto FAIL_SWS_GETCONTEXT;
  }

  gettimeofday(&last,NULL);

  while(!flagQuit)
  {
    // last = now;
    gettimeofday(&now,NULL);

    pthread_mutex_lock(&mutexCapturedFrame);

      /*
       * Wait for new data to be available
       */
      while(!flagQuit && frameId <= curFrameId)
        pthread_cond_wait(&condDataAvailable,&mutexCapturedFrame);
 
    pthread_mutex_unlock(&mutexCapturedFrame);

    if(flagQuit)
      break;

    gettimeofday(&timeWait,NULL);
    DBG("Time waiting: %ld ms",(timeWait.tv_sec-now.tv_sec)*1000+(timeWait.tv_usec-now.tv_usec)/1000);

    if(croppedFrame == NULL)
    {
      if((croppedFrame = (char *)malloc(config->sizeX*config->sizeY*bytesPerPixelSrc)) == NULL)
      {
        ERR("Cannot allocate %d bytes for cropped frame",config->sizeX*config->sizeY*bytesPerPixelSrc);
        goto FAIL_MALLOC_FRAME;
      }
      srcstride = config->sizeX*bytesPerPixelSrc;      

      LOG("Allocate %d bytes (%d x %d x %d) for the cropped frame",config->sizeX*config->sizeY*bytesPerPixelSrc,config->sizeX,config->sizeY,bytesPerPixelSrc);
      LOG("Stride size %d",srcstride);
    }
    /*
     * Memcopy line by line from complete frame to our cropped frame. Assume it is thread safe as we only accesse the memory read-only.
     */
    unsigned int bytesPointer;
    unsigned int curLine = 0;
    unsigned int bytesOffset = config->offsetY*bytesPerLineSrc+config->offsetX*bytesPerPixelSrc;
    unsigned int bytesPerLineDst = config->sizeX*bytesPerPixelSrc;

    for(curLine=0,bytesPointer=bytesOffset; curLine < config->sizeY; curLine++,bytesPointer+=bytesPerLineSrc)
      memcpy(croppedFrame+curLine*bytesPerLineDst,sharedFrame+bytesPointer,bytesPerLineDst);

    // dumpRGBAjpeg(croppedFrame,config->sizeX,config->sizeY,"debug.jpg");

    curFrameId = frameId;

    gettimeofday(&timeMemcpy,NULL);
    DBG("Time memcopying: %ld ms",(timeMemcpy.tv_sec-timeWait.tv_sec)*1000+(timeMemcpy.tv_usec-timeWait.tv_usec)/1000);

    pthread_mutex_lock(&mutexCapturedFrame);
    memcopyDone++;
    DBG("memcopyDone++");

    pthread_cond_broadcast(&condDataConsummed);
    pthread_cond_broadcast(&condDataAvailable);

    pthread_mutex_unlock(&mutexCapturedFrame);


    /*
     * Color conversion 
     */
    sws_scale(convertCtx, &croppedFrame, &srcstride, 0, config->sizeY, pic_in.img.plane, pic_in.img.i_stride);

    /*
     * Add an incrementing dummy timestamp to the frame
     */
    gettimeofday(&timeScaling,NULL);
    DBG("Time scaling: %ld ms",(timeScaling.tv_sec-timeWait.tv_sec)*1000+(timeScaling.tv_usec-timeWait.tv_usec)/1000);    
    pic_in.i_pts = curFrameId;


    if ((frameSize = x264_encoder_encode(encoder, &nals, &i_nals, &pic_in, &pic_out)) < 0)
    {
      ERR("Frame size with pts %ld: errno: %d",pic_in.i_pts,frameSize);
      continue;
    }

    gettimeofday(&timeEncoding,NULL);
    DBG("Time encoding: %ld ms",(timeEncoding.tv_sec-timeMemcpy.tv_sec)*1000+(timeEncoding.tv_usec-timeMemcpy.tv_usec)/1000);

    /*
     * Set manually IDR header for intra refresh?
     */
    if(config->x264params.b_intra_refresh)
      *(nals[0].p_payload+5) = 0x64; 

    alreadySent = 0;
    sent = 0;

    gettimeofday(&now,NULL);

    double delta = (now.tv_sec-last.tv_sec)*1000+(now.tv_usec-last.tv_usec)/1000;

    LOG("Delta: %f",delta);

    last.tv_usec = now.tv_usec;
    last.tv_sec = now.tv_sec;

    if(config->hardFpsLimiter > 0 && delta < 1000/config->hardFpsLimiter)
    {
      LOG("Sleeping %d ms",(unsigned int)(1000/config->hardFpsLimiter - delta));      
      usleep((unsigned int)((1000/config->hardFpsLimiter - delta)*1000);
    }

    do
    {
      sent = sendto(
        sendingSocket,
        nals[0].p_payload+alreadySent,
        (frameSize-alreadySent) > MAX_UDP_SIZE ? MAX_UDP_SIZE : frameSize - alreadySent,
        0,
        (struct sockaddr *) &si_other,
        slen);

      if(sent < 0)
      {
        ERR("Sendto returns -1. Errno: %d",errno);
        break;
      }
      else
        alreadySent += sent;
      
    } while (sent != -1 && alreadySent != frameSize);

    gettimeofday(&timeSend,NULL);

    DBG("Time sending: %ld ms",(timeSend.tv_sec-timeEncoding.tv_sec)*1000+(timeSend.tv_usec-timeEncoding.tv_usec)/1000);
    DBG("Time total: %ld ms",(timeSend.tv_sec-now.tv_sec)*1000+(timeSend.tv_usec-now.tv_usec)/1000);


    // DBG("Time total: %ld s %ld ms",(now.tv_sec-last.tv_sec),(now.tv_usec-last.tv_usec));



  }
  LOG("Exiting normally");

    free(croppedFrame);
  FAIL_MALLOC_FRAME:
  FAIL_SWS_GETCONTEXT:

    x264_picture_clean(&pic_in);
  FAIL_ALLOC:

    x264_encoder_close(encoder);
  FAIL_ENCODER:
  FAIL_INET_ATON:
  FAIL_SOCKET_INTERFACE:

    close(sendingSocket);
  FAIL_SOCKET:

  return NULL;
}
