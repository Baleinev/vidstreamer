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

  struct timeval now,timeWait,timeMemcpy,timeEncoding,timeSend,timeScaling;

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

  if ((sendingSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
  {
    ERR("Cannot open sending socket:%d errno:%d",sendingSocket,errno);
    goto FAIL_SOCKET;
  }

  int tmp = 1024*4*7;

  if(setsockopt(sendingSocket, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp)) < 0)
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
  si_other.sin_port = htons(config->targetPort);

  LOG("Sending to %s:%d",config->targetHost,config->targetPort);
   
  if (inet_aton(config->targetHost , &si_other.sin_addr) == 0)
  {
    ERR("Cannot convert string to ip: %s",config->targetHost);
    goto FAIL_INET_ATON;
  }


  if((encoder = x264_encoder_open(&(config->x264param))) == NULL)
  {
    ERR("Cannot open encoder: %ld",(long unsigned int)encoder);
    goto FAIL_ENCODER;
  }


  if(x264_picture_alloc(&pic_in, X264_CSP_I420, config->width, config->height) < 0)
  {
    ERR("Cannot allocate picture");
    goto FAIL_ALLOC;
  }
  ERR("Allocated picture of %d x %d",config->width,config->height);

  // x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);

  /*
   * Init filter from swslib to YUV420 from RGBA. The alpha component is not used.
   */
  if((convertCtx = sws_getContext(
    config->width,
    config->height,
    AV_PIX_FMT_BGRA,
    config->width,
    config->height,
    PIX_FMT_YUV420P,
    SWS_FAST_BILINEAR, // Not the best, but the fastest
    NULL,
    NULL,
    NULL)) == NULL)
  {
    ERR("Cannot get filtering context: %ld",(long unsigned int)convertCtx);
    goto FAIL_SWS_GETCONTEXT;
  }


  while(!flagQuit)
  {
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
      if((croppedFrame = (char *)malloc(config->width*config->height*bytesPerPixelSrc)) == NULL)
      {
        ERR("Cannot allocate %d bytes for cropped frame",config->width*config->height*bytesPerPixelSrc);
        goto FAIL_MALLOC_FRAME;
      }
      srcstride = config->width*bytesPerPixelSrc;      

      LOG("Allocate %d bytes (%d x %d x %d) for the cropped frame",config->width*config->height*bytesPerPixelSrc,config->width,config->height,bytesPerPixelSrc);
      LOG("Stride size %d",srcstride);
    }
    /*
     * Memcopy line by line from complete frame to our cropped frame. Assume it is thread safe as we only accesse the memory read-only.
     */
    unsigned int bytesPointer;
    unsigned int curLine = 0;
    unsigned int bytesOffset = config->yOffset*bytesPerLineSrc+config->xOffset*bytesPerPixelSrc;
    unsigned int bytesPerLineDst = config->width*bytesPerPixelSrc;

    for(curLine=0,bytesPointer=bytesOffset; curLine < config->height; curLine++,bytesPointer+=bytesPerLineSrc)
      memcpy(croppedFrame+curLine*bytesPerLineDst,sharedFrame+bytesPointer,bytesPerLineDst);

    // dumpRGBAjpeg(croppedFrame,config->width,config->height,"debug.jpg");

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
    sws_scale(convertCtx, &croppedFrame, &srcstride, 0, config->height, pic_in.img.plane, pic_in.img.i_stride);

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
    if(flagIntra)
      *(nals[0].p_payload+5) = 0x64; 

    alreadySent = 0;
    sent = 0;  

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
