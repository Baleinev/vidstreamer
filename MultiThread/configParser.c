#define _GNU_SOURCE

#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#include <x264.h>

#include "cJSON/cJSON.h"

#include "configParser.h"
#include "screenStreamerMulti.h"

char buffer[CONF_FILE_MAXLENGTH];

enum jsonDataType_t {STRING,NUMBER,BOOL,FLOAT};

static void setAffinity(cpu_set_t *cores,unsigned int bitsFiled)
{
  int i;
  int numCores = sysconf(_SC_NPROCESSORS_ONLN);

  CPU_ZERO(cores);

  for(i=0;i < sizeof(unsigned int)*4;i++)
  {
    unsigned int bit = bitsFiled & 0x1;

    if(i == numCores)
      break;

    if(bit == 1)
      CPU_SET(i, cores);

    bitsFiled = bitsFiled >> 1;
  }
}

static void setDefaultX264paramConfig(x264_param_t *x264params)
{
  x264_param_default_preset(x264params, "ultrafast", "zerolatency");

  x264params->i_threads = 1;
  x264params->i_fps_num = 30;
  x264params->i_fps_den = 1;
  x264params->i_keyint_max = 60;
  x264params->b_intra_refresh = 0;
  x264params->rc.i_rc_method = X264_RC_CRF;
  x264params->rc.f_rf_constant = 25;
  // x264defaultParam.i_slice_max_size = MAX_UDP_SIZE;
  x264params->rc.i_vbv_max_bitrate = 20000;
  x264params->rc.i_vbv_buffer_size = 1;
  // x264defaultParam.b_annexb = 1; 

  x264_param_apply_profile(x264params, "baseline");  
}

void printConfig(globalConfig_t *globalConfig)
{
  int i;

  int cpuCount = CPU_COUNT(&(globalConfig->grabber.affinity));

  LOG("Grabber config:");
  LOG("hardFpsLimiter/waitForAll/nbStreamers/affinity: \n%f,\n%d,\n%d,\n%d",
    globalConfig->grabber.hardFpsLimiter,
    globalConfig->grabber.waitForAll,
    globalConfig->grabber.nbStreamers,
    globalConfig->grabber.affinity,
    cpuCount);

  for(i=0;i<globalConfig->grabber.nbStreamers;i++)
  {
    LOG("Streamer %d config:",i);
    LOG("offsetX/offsetY/sizeX/sizeY/ip/port/interface/bufferSize/hardFpsLimiter/affinity: \n%d,\n%d,\n%d,\n%d,\n%s,\n%d,\n%s,\n%d,\n%f",
      globalConfig->streamers[i].offsetX,
      globalConfig->streamers[i].offsetY,
      globalConfig->streamers[i].sizeX,
      globalConfig->streamers[i].sizeY,
      globalConfig->streamers[i].ip,                  
      globalConfig->streamers[i].port,   
      globalConfig->streamers[i].interface,
      globalConfig->streamers[i].bufferSize,
      globalConfig->streamers[i].hardFpsLimiter);

    LOG("-> x264 config: threads/fps/keymax/keymin/slicemax/intra/vbvmaxrate/vbvbuffer/rc_method/crf: \n%d,\n%d,\n%d,\n%d,\n%d,\n%d,\n%d,\n%d,\n%d,\n%f",
      globalConfig->streamers[i].x264params.i_threads,
      globalConfig->streamers[i].x264params.i_fps_num,
      globalConfig->streamers[i].x264params.i_keyint_max,
      globalConfig->streamers[i].x264params.i_keyint_min,
      globalConfig->streamers[i].x264params.i_slice_max_size,
      globalConfig->streamers[i].x264params.b_intra_refresh,  

      globalConfig->streamers[i].x264params.rc.i_vbv_max_bitrate,
      globalConfig->streamers[i].x264params.rc.i_vbv_buffer_size,
      globalConfig->streamers[i].x264params.rc.i_rc_method,
      globalConfig->streamers[i].x264params.rc.f_rf_constant);
  }
}

static void setDefaultStreamerConfig(streamerConfig_t *streamerConfig)
{
  int i;

  streamerConfig->offsetX = 0;
  streamerConfig->offsetY = 0;
  streamerConfig->sizeX = 640;
  streamerConfig->sizeY = 480;

  strncpy(streamerConfig->ip,"127.0.0.1",IPSTRING_MAXLENGTH);
  strncpy(streamerConfig->interface,"eth0",INTERFACENAME_MAXLENGTH);

  streamerConfig->port = 4243;
  streamerConfig->bufferSize = 1024*1024*32;
  streamerConfig->hardFpsLimiter = -1.0;

  setDefaultX264paramConfig(&(streamerConfig->x264params));

  setAffinity(&(streamerConfig->affinity),(unsigned int)0xFFFFFFFFE);
}

static void setDefaultGrabberConfig(grabberConfig_t *grabberConfig)
{
  grabberConfig->hardFpsLimiter = -1.0;
  grabberConfig->waitForAll = false;

  setAffinity(&(grabberConfig->affinity),(unsigned int)0x00000001);  
}

static bool updateConfig(cJSON *parent, const char *attribute, void *structData, enum jsonDataType_t type)
{
  cJSON *child = cJSON_GetObjectItem(parent,attribute);
  
  if(child == NULL)
    return false;

  /*
   * C struct int and float members are 4-bytes aligned, aren't they?
   */
  switch(type)
  {
    case NUMBER:
      *((int *)structData) = child->valueint;
    break;
    case STRING:
      strncpy((char *)structData,child->valuestring,strlen(child->valuestring));
    break;
    case FLOAT:
      *((float *)structData) = child->valuedouble;
    break;
    case BOOL:
      *((bool *)structData) = child->valueint;
    break;
    default:
      ERR("Unknown data type");
      return false;
    break;
  }

  return true;
}

bool parseConfig(const char *configFile,globalConfig_t *globalConfig)
{
  int i,j,nbAffinity,nbStreamers = 0;

  FILE *confHandle = fopen(configFile,"rb");

  if(confHandle == NULL)
  {
    ERR("Cannot open config file %s (errno:%d)",configFile,errno);
    goto FAIL_FOPEN;
  }

  int nbRead = fread(buffer,1, CONF_FILE_MAXLENGTH, confHandle);

  LOG("Config file %s is %d bytes lentgh",configFile,nbRead);

  fclose(confHandle);


  cJSON *root = cJSON_Parse(buffer);

  if(root == NULL)
  {
    ERR("Cannot parse config file.");
    goto FAIL_PARSE;
  }

  /*
   * Populate grabber config with default values
   */
  setDefaultGrabberConfig(&(globalConfig->grabber));

  cJSON *grabber = cJSON_GetObjectItem(root,"grabber");

  if(grabber)
  {
    updateConfig(grabber,"hardFpsLimiter",&(globalConfig->grabber.hardFpsLimiter),FLOAT);      
    updateConfig(grabber,"waitForAll",&(globalConfig->grabber.waitForAll),BOOL); 

    cJSON *affinity = cJSON_GetObjectItem(grabber,"affinity");
    int nbAffinity;

    /* Copy affinity array */
    if((affinity != NULL && (nbAffinity = cJSON_GetArraySize(affinity))>0))
    {
      unsigned int affinityByteField = 0;

      for(j=0;j<nbAffinity;j++)
      {
        cJSON * subitem = cJSON_GetArrayItem(affinity, j);

        affinityByteField &= 0x1 << subitem->valueint;
      }
      setAffinity(&(globalConfig->grabber.affinity),affinityByteField);
    }
  }

  cJSON *streamers = cJSON_GetObjectItem(root,"streamers");

  if(streamers != NULL && ((nbStreamers = cJSON_GetArraySize(streamers))>0))
  {
    /*
     * Allocate size for streamer array now we know its size
     */
    globalConfig->grabber.nbStreamers = nbStreamers;
    globalConfig->streamers = (streamerConfig_t *)malloc(sizeof(streamerConfig_t)*nbStreamers);

    /* For each streamer */
    for(i=0;i<nbStreamers;i++)
    {
      cJSON *streamer = cJSON_GetArrayItem(streamers,i);

      setDefaultStreamerConfig(&(globalConfig->streamers[i]));

      cJSON *affinity = cJSON_GetObjectItem(streamer,"affinity");

      /* Copy affinity array */
      if((affinity != NULL && (nbAffinity = cJSON_GetArraySize(affinity))>0))
      {
        unsigned int affinityByteField = 0;

        for(j=0;j<nbAffinity;j++)
        {
          cJSON * subitem = cJSON_GetArrayItem(affinity, j);

          affinityByteField &= 0x1 << subitem->valueint;
        }
        setAffinity(&(globalConfig->streamers[i].affinity),affinityByteField);
      }

      cJSON *source = cJSON_GetObjectItem(streamer,"source");

      if(source != NULL)
      {
        updateConfig(source,"offsetX",&(globalConfig->streamers[i].offsetX),NUMBER);      
        updateConfig(source,"offsetY",&(globalConfig->streamers[i].offsetY),NUMBER);      
        updateConfig(source,"sizeX",&(globalConfig->streamers[i].sizeX),NUMBER);      
        updateConfig(source,"sizeY",&(globalConfig->streamers[i].sizeY),NUMBER);      
      }

      cJSON *encoding = cJSON_GetObjectItem(streamer,"encoding");

      if(encoding != NULL)
      {
        cJSON *x264params = cJSON_GetObjectItem(streamer,"x264params");

        if(x264params != NULL)
        {
          updateConfig(x264params,"threads",&(globalConfig->streamers[i].x264params.i_threads),NUMBER);      
          updateConfig(x264params,"fps",&(globalConfig->streamers[i].x264params.i_fps_num),NUMBER);      
          updateConfig(x264params,"maxKeyint",&(globalConfig->streamers[i].x264params.i_keyint_max),NUMBER);      
          updateConfig(x264params,"minKeyint",&(globalConfig->streamers[i].x264params.i_keyint_min),NUMBER);      
          updateConfig(x264params,"sliceMaxSize",&(globalConfig->streamers[i].x264params.i_slice_max_size),NUMBER);       
          updateConfig(x264params,"intraRefersh",&(globalConfig->streamers[i].x264params.b_intra_refresh),NUMBER);      

          updateConfig(x264params,"vbvMaxBitrate",&(globalConfig->streamers[i].x264params.rc.i_vbv_max_bitrate),NUMBER);      
          updateConfig(x264params,"vbvBufferSize",&(globalConfig->streamers[i].x264params.rc.i_vbv_buffer_size),NUMBER);  
          updateConfig(x264params,"method",&(globalConfig->streamers[i].x264params.rc.i_rc_method),NUMBER);      
          updateConfig(x264params,"crf",&(globalConfig->streamers[i].x264params.rc.f_rf_constant),NUMBER);  
        }
        updateConfig(encoding,"hardFpsLimiter",&(globalConfig->streamers[i].hardFpsLimiter),FLOAT); 
      }

      cJSON *sending = cJSON_GetObjectItem(streamer,"sending");
      
      if(sending != NULL)
      {
        updateConfig(sending,"ip",&(globalConfig->streamers[i].ip),STRING);      
        updateConfig(sending,"port",&(globalConfig->streamers[i].port),NUMBER);      
        updateConfig(sending,"interface",&(globalConfig->streamers[i].interface),STRING);      
        updateConfig(sending,"bufferSize",&(globalConfig->streamers[i].bufferSize),NUMBER); 
      }      
    }
  }
  else
  {
    ERR("No streamers specified in config file");

    goto FAIL_STREAMER;
  }
  return true;

  FAIL_STREAMER:

  cJSON_Delete(root);
  FAIL_PARSE:

  fclose(confHandle);
  FAIL_FOPEN:

  return false;
} 

