#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <string.h>
#include <stdlib.h>
#include <jpeglib.h>

#include <sys/syscall.h>

#include <x264.h>

#include "screenStreamerMulti.h"
#include "threadVideoStream.h"
#include "threadPollScreen.h"
#include "configParser.h"

char displayName[CONFIG_DISPLAYNAME_MAXLENGTH] = ":0.0";
char configFile[CONFIG_FILEPATH_MAXLENGTH] = "/etc/pmw.d/streamer.conf";

// unsigned int fps = 30;

unsigned int screenWidth;
unsigned int screenHeight;
unsigned int bytesPerPixelSrc;
unsigned int bytesPerLineSrc;

bool flagQuit = false; 

globalConfig_t globalConfig;

// bool flagAffinity = false;
// bool flagSleep = false;
// bool flagIntra = false;
// bool flagWaitForConsumers = false;
// bool flagForceKeyint = false;

char *sharedFrame;

// unsigned int nbEncoders;

// bool flagDataAvailable = false;
// 
unsigned int memcopyDone = 0;
unsigned int frameId = 0;

pthread_cond_t condDataAvailable = PTHREAD_COND_INITIALIZER;
pthread_cond_t condDataConsummed = PTHREAD_COND_INITIALIZER;

pthread_mutex_t mutexCapturedFrame = PTHREAD_MUTEX_INITIALIZER; 

pthread_t grabber;
pthread_t *streamers; //[MAX_STREAMERS];

// /* TODO: get all these infos from config file */
// const unsigned int xOffset[5] = {0,1024,2048,3072,4096};
// const unsigned int yOffset[5] = {0,0,0,0,0};
// const unsigned int width[5] = {1024,1024,1024,1024,1024};
// const unsigned int height[5] = {768,768,768,768,768};

// unsigned char targetHost[5][128] = {
//   "192.168.1.135",
//   "192.168.1.122",
//   "192.168.1.127",
//   "192.168.1.128",
//   "192.168.1.126"
// };
// unsigned int targetPort[5] = {4243,4243,4243,4243,4243};

int gettid()
{
  return syscall(SYS_gettid);
}

dumpRGBAjpeg(unsigned char *data,unsigned int width,unsigned int height,const char *name)
{
  FILE *outfile;

  outfile = fopen(name,"w");

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  JSAMPROW scanline[1];

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  jpeg_stdio_dest(&cinfo, outfile);    

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 4;
  cinfo.in_color_space = JCS_EXT_BGRX;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 75, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  while (cinfo.next_scanline < (unsigned int) height)
  {
    scanline[0] = data + 4 * width * cinfo.next_scanline;
    jpeg_write_scanlines(&cinfo, scanline, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  fclose(outfile);
}

int main(int argc,char *argv[])
{
  int i;
  int c;

  opterr = 0;

  int num_cores;
  cpu_set_t cpuset;

  x264_param_t x264defaultParam;  

  streamerConfig_t *streamerConfig;

  static struct termios oldt, newt;  

  // while ((c = getopt (argc, argv, "iaswkn:d::f::")) != -1)
  
  while ((c = getopt (argc, argv, "d:c:")) != -1)
  {
    switch (c)
      {
      // case 'i':
      //   flagIntra = true;
      //   break;        
      // case 'a':
      //   flagAffinity = true;
      //   break;
      // case 's':
      //   flagSleep = true;
      //   break;
      // case 'w':
      //   flagWaitForConsumers = true;
      //   break;
      // case 'n':
      //   nbEncoders = atoi(optarg);
      //   break;
      // case 'k':
      //   flagForceKeyint = true;
      //   break;
      case 'd':
        strncpy(displayName,optarg,CONFIG_DISPLAYNAME_MAXLENGTH);
        break;
      case 'c':
      	strncpy(configFile,optarg,CONFIG_FILEPATH_MAXLENGTH);
      break;
      // case 'f':
      //   fps = atoi(optarg);
      //   break;
      case '?':
        if (optopt == 'd' | optopt == 'c')
          fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        else if (isprint (optopt))
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf (stderr,"Unknown option character `\\x%x'.\n",optopt);
        return 1;
      default:
        abort();
      }
  }

  // LOG("Config: intra:%d, affinity:%d, sleep:%d, wait:%d nbEncoder:%d, forcekeyint:%d, displayName:%s, fps:%d",
  //   flagIntra,
  //   flagAffinity,
  //   flagSleep,
  //   flagWaitForConsumers,
  //   nbEncoders,
  //   flagForceKeyint,
  //   displayName,
  //   fps
  // );

  if(parseConfig(configFile,&globalConfig) == false)
  {
  	ERR("Error parsing config");
  	goto FAIL_CONFIG;
  }

  printConfig(&globalConfig);

  // for (i = optind; i < argc; i++)
    // printf ("Non-option argument %s\n", argv[i]);

  // if(nbEncoders < 1 || nbEncoders > MAX_ENCODERS)
  // {
  //   ERR("Invalid requested number of encoders: %d ",nbEncoders);
  //   goto FAIL_NBENCODERS;
  // }
  


  /*
   * Prepare config struct for every encoder thread
   */
  // if((streamerConfig = (streamerConfig_t *)(malloc(sizeof(streamerConfig_t)*nbEncoders))) == NULL)
  // {
  //   ERR("Cannot malloc %ld bytes for streamerConfig",sizeof(streamerConfig_t)*nbEncoders);
  //   goto FAIL_MALLOCCONFIG;
  // }

  // for(i=0;i < nbEncoders;i++)
  // {
  //   streamerConfig[i].xOffset = xOffset[i];
  //   streamerConfig[i].yOffset = yOffset[i];
  //   streamerConfig[i].width = width[i];
  //   streamerConfig[i].height = height[i];
  //   streamerConfig[i].targetPort = targetPort[i];

  //   strncpy(streamerConfig[i].interface,i%2 ? "eth0" : "eth1", 32);
  //   strncpy(streamerConfig[i].targetHost,targetHost[i],128);
  //   memcpy(&(streamerConfig[i].x264param),&x264defaultParam,sizeof(x264_param_t));

  //   streamerConfig[i].x264param.i_width = streamerConfig[i].width;
  //   streamerConfig[i].x264param.i_height = streamerConfig[i].height;
  // }
  // 
  
  streamers = (pthread_t *)malloc(sizeof(pthread_t)*globalConfig.grabber.nbStreamers);

  pthread_create(&grabber,NULL,&threadPollScreen,(void *)&(globalConfig.grabber));

  for(i=0;i < globalConfig.grabber.nbStreamers;i++) 
  {
    pthread_create(&(streamers[i]),NULL,&threadVideoStream,(void *)&(globalConfig.streamers[i]));  
  }

  /*
   * If multi core system, keep the first core for the poller
   */
  // if(flagAffinity)
  // {
  //   if((num_cores = sysconf(_SC_NPROCESSORS_ONLN)) > 1)
  //   {
  //     LOG("Setting affinity. %d detected cores",num_cores);

  //     CPU_ZERO(&cpuset);
  //     CPU_SET(0, &cpuset);

  //     pthread_setaffinity_np(poller, sizeof(cpu_set_t), &cpuset);      

  //     CPU_ZERO(&cpuset);

  //     for(i=1;i<num_cores;i++)
  //       CPU_SET(i, &cpuset);

  //     for(i=0;i < nbEncoders;i++)
  //       pthread_setaffinity_np(encoders[i], sizeof(cpu_set_t), &cpuset);      
      
  //   }
  //   else if(num_cores != 1)
  //   {
  //     ERR("Cannot detect numbers of cores: %d. Cannot set affinity. Not fatal but performance will suffer.",num_cores);
  //   }
  // }
  /*
   * Configure the terminal so any key press will be processed imediately (without the need of a return)
   */
  tcgetattr( STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON);          
  tcsetattr( STDIN_FILENO, TCSANOW, &newt);  

  LOG("Press any key to quit");

  getchar();

  tcsetattr( STDIN_FILENO, TCSANOW, &oldt);

  flagQuit = true;

  pthread_cond_broadcast(&condDataConsummed);
  pthread_cond_broadcast(&condDataAvailable);


  for(i=0;i< globalConfig.grabber.nbStreamers;i++)
    pthread_join(streamers[i],NULL);

  free(streamerConfig);

  FAIL_MALLOCCONFIG:
  FAIL_NBENCODERS:
  FAIL_CONFIG:
  return 0;
}


