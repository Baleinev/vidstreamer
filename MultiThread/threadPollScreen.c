#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/resource.h>

#include <arpa/inet.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include "screenStreamerMulti.h"

/*
 * The variables below are used by the videostream threads. They are initialized by the poll thread before use.
 */
extern unsigned int screenWidth;
extern unsigned int screenHeight;
extern unsigned int bytesPerPixelSrc;
extern unsigned int bytesPerLineSrc;


// extern unsigned int fps;

extern char displayName[128];

extern bool flagQuit;
// extern bool flagSleep;

extern char *sharedFrame;


extern unsigned int memcopyDone;
extern unsigned int frameId;

// extern unsigned int nbEncoders;

extern pthread_cond_t condDataAvailable;
extern pthread_cond_t condDataConsummed ;

extern pthread_mutex_t mutexCapturedFrame;

void *threadPollScreen(void * param)
{
  Display *dpy;
  XImage *image;
  XWindowAttributes xwAttr;  
  unsigned int frameSize;
  XShmSegmentInfo shminfo;
  int screen;  
  Window root;
  struct timeval now,last,timeGrab,timeWait;

  struct grabberConfig_t *config = (struct grabberConfig_t *)param;

  const unsigned int xOffset = 0;
  const unsigned int yOffset = 0;

  if(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &(config->affinity)) != 0)
  {
    ERR("Cannot set affinity. errno:%d",errno);
  }

  if(setpriority(PRIO_PROCESS,gettid(), config->niceness) != 0)
  {
    ERR("Cannot set niceness. errno:%d",errno);
  }

  if((dpy = XOpenDisplay(displayName)) == NULL)
  {
    ERR("Cannot XOpenDisplay: %lx",(long unsigned)dpy);
    goto FAIL_XOPENDISPLAY;
  }
 
  screen = DefaultScreen(dpy);

  if((root = RootWindow(dpy, screen)) == NULL)
  {
    ERR("Cannot RootWindow: %lx",(long unsigned int)root);
    goto FAIL_ROOTWINDOW;    
  }

  if(XGetWindowAttributes(dpy,root,&xwAttr) == 0)
  {
    ERR("Cannot XGetWindowAttributes: 0");
    goto FAIL_XGETWINDOWATTRIBUTES;    
  }

  screenWidth = xwAttr.width;
  screenHeight = xwAttr.height;

  LOG("Frame size: %dx%d",screenWidth,screenHeight); 

  if((image = XShmCreateImage(
    dpy, 
    DefaultVisual(dpy, XDefaultScreen(dpy)),
    DefaultDepth(dpy, XDefaultScreen(dpy)),
    ZPixmap,
    NULL,
    &shminfo,
    screenWidth,
    screenHeight)) == NULL)
  {
    ERR("Cannot XShmCreateImage: %lx",(long unsigned)image);  
    goto FAIL_XSHMCREATEIMAGE;    
  }

  if(image->bits_per_pixel != 32)
  {
    ERR("Ximage is not 32 bits per pixel");
    goto FAIL_BITSPERPIXEL;    
  }

  frameSize = screenWidth * screenHeight * image->bits_per_pixel / 8;

  bytesPerPixelSrc = (image->bits_per_pixel)/8;
  bytesPerLineSrc = image->bytes_per_line;

  LOG("Frame bytes per pixel: %d",bytesPerPixelSrc);  
  LOG("Frame bytes per line: %d",bytesPerLineSrc);  
  LOG("Frame size: %d Kib",frameSize/1024);
  LOG("Frame masks (rgb): 0x%.8lx,0x%.8lx,0x%.8lx",image->red_mask,image->green_mask,image->blue_mask);

  if((shminfo.shmid = shmget(IPC_PRIVATE, frameSize,IPC_CREAT | 0777)) < 0)
  {
    ERR("Cannot shmget: %d",shminfo.shmid);    
    goto FAIL_SHMGET;    
  }

  shminfo.shmaddr  = image->data = shmat(shminfo.shmid, 0, 0);
  shminfo.readOnly = False;

  if(XShmAttach(dpy, &shminfo) == 0)
  {
    ERR("Cannot XShmAttach");
    goto FAIL_XSHMATTACH;    
  }

  sharedFrame = image->data;

  gettimeofday(&last,NULL);

  while(!flagQuit)
  {
    gettimeofday(&now,NULL);

    if(XShmGetImage(dpy, root, image, xOffset, yOffset, AllPlanes) == False)
    {
      ERR("XShmGetImage failed: False");
      goto FAIL_XSHMGETIMAGE;
    }

    gettimeofday(&timeGrab,NULL);
    DBG("Time XShmGetImage: %ld ms",(timeGrab.tv_sec-now.tv_sec)*1000+(timeGrab.tv_usec-now.tv_usec)/1000);

    /* 
     * New BRGX data is now in sharedFrame
     * - notify consummers
     * - wait for them all to memcopy the part of the frame they need in their own safe memory zone   
     */

    pthread_mutex_lock(&mutexCapturedFrame);
      
      frameId++;

      pthread_cond_broadcast(&condDataAvailable);

      while(config->waitForAll && !flagQuit && memcopyDone != config->nbStreamers)
        pthread_cond_wait(&condDataConsummed,&mutexCapturedFrame);
      
      memcopyDone = 0;

    pthread_mutex_unlock(&mutexCapturedFrame);

    if(flagQuit)
      break;

    gettimeofday(&timeWait,NULL);
    DBG("Time waiting: %ld ms",(timeWait.tv_sec-timeGrab.tv_sec)*1000+(timeWait.tv_usec-timeGrab.tv_usec)/1000);

    gettimeofday(&now,NULL);

    double delta = (now.tv_sec-last.tv_sec)*1000+(now.tv_usec-last.tv_usec)/1000;

    DBG("Delta: %f",delta);

    if(config->hardFpsLimiter > 0 && delta < 1000/config->hardFpsLimiter)
    {
      DBG("Sleeping %d ms",(unsigned int)(1000/config->hardFpsLimiter - delta));      
      usleep((unsigned int)((1000/config->hardFpsLimiter - delta)*1000));
    }
    
    gettimeofday(&now,NULL);


    last.tv_usec = now.tv_usec;
    last.tv_sec = now.tv_sec;  
  }

  LOG("Exiting normally");

  FAIL_XSHMGETIMAGE:

    if(XShmDetach(dpy,&shminfo)==False)
      ERR("Cannot XShmDetach: False");
  FAIL_XSHMATTACH:
  
    if(shmdt(shminfo.shmaddr) < 0)
      ERR("Cannot shmdt: %d",errno);      

    if(shmctl(shminfo.shmid, IPC_RMID, NULL)<0)
      ERR("Cannot shmctl(IPC_RMID): %d",errno);      
  FAIL_SHMGET:
  FAIL_BITSPERPIXEL:

    XDestroyImage(image);    
  FAIL_XSHMCREATEIMAGE:
  FAIL_XGETWINDOWATTRIBUTES:
  FAIL_ROOTWINDOW:

    XCloseDisplay(dpy);
  FAIL_XOPENDISPLAY:

  LOG("Exiting now");

  return NULL;
}