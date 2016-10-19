#include <stdio.h>
#include <stdint.h>

#include <sys/time.h>
#include <sys/shm.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <libswscale/swscale.h>

#include <jpeglib.h>
#include <x264.h>

#define MAX_UDP_SIZE 1472

#define BUFFER_SIZE (16*1024*1024) 

#define FAIL_EXIT(fmt,...) do{fprintf(stderr,fmt, ##__VA_ARGS__);fprintf(stderr,"\n");exit(-1);}while(0);
#define LOG(fmt,...) do{fprintf(stdout,fmt, ##__VA_ARGS__);fflush(stdout);}while(0);

// #ifndef JCS_ALPHA_EXTENSIONS
// #error In this code, lib libjpeg-turbo's BGRX pixel format extension is used to accomodate native Ximage data format.
// #error Please check that you are using an up-to-date libjpeg-turbo and that is is configured properly
// #endif

const char outfileName[] = "img.jpg";

const char dpyname[] = ":0.0";

const outType = 2;

const unsigned int xOffset = 0;
const unsigned int yOffset = 0;
const unsigned int width = 800;
const unsigned int height = 600;

const unsigned int fps = 30;

const unsigned int bitsPerPixel = 32;

const unsigned int jpegQuality = 75;

const unsigned int udpPort = 4243;

const char targetHost[] = "192.168.1.135";

unsigned char outbuffer[BUFFER_SIZE];
unsigned int outlen = BUFFER_SIZE;

unsigned char sei[1024];

unsigned int frame_num;

int main()
{
  Display *dpy;
  XImage *image;
  unsigned int frame_size;
  XShmSegmentInfo shminfo;
  int screen;  
  Window root;
  struct timeval last,now,deltaX11,deltaFilter,deltaSocket;

  FILE *outfile;

  if((dpy = XOpenDisplay(dpyname)) == NULL)
    FAIL_EXIT("Cannot XOpenDisplay: %x",dpy);

  if((image = XShmCreateImage(
    dpy, 
    DefaultVisual(dpy, XDefaultScreen(dpy)),
    DefaultDepth(dpy, XDefaultScreen(dpy)),
    ZPixmap,
    NULL,
    &shminfo,
    width,
    height)) == NULL)
    FAIL_EXIT("Cannot XShmCreateImage: %x",image);  

  if(bitsPerPixel != image->bits_per_pixel)
    FAIL_EXIT("Ximage is not 32 bits per pixel");  

  frame_size = width * height * bitsPerPixel / 8;

  LOG("Frame size: %dx%d\n",width,height);
  LOG("Frame bits per pixel: %d\n",bitsPerPixel);  
  LOG("Frame bytes per line: %d\n",image->bytes_per_line);  
  LOG("Frame size: %d Kib\n",frame_size/1024);
  LOG("Frame masks (rgb): 0x%.8x,0x%.8x,0x%.8x\n",image->red_mask,image->green_mask,image->blue_mask);

  shminfo.shmid = shmget(IPC_PRIVATE, frame_size,IPC_CREAT | 0777);
  shminfo.shmaddr  = image->data = shmat(shminfo.shmid, 0, 0);
  shminfo.readOnly = False;

  if(XShmAttach(dpy, &shminfo) == 0)
    FAIL_EXIT("Cannot XShmAttach");  

  screen = DefaultScreen(dpy);

  if((root   = RootWindow(dpy, screen)) == NULL)
    FAIL_EXIT("Cannot RootWindow: %x",root);


  x264_param_t param;
  x264_param_default_preset(&param, "ultrafast", "zerolatency");
  param.i_threads = 1;
  param.i_width = width;
  param.i_height = height;
  param.i_fps_num = fps;
  param.i_fps_den = 1;
  // Intra refres:
  // param.i_keyint_max = fps;
  param.b_intra_refresh = 1;
  //Rate control:
  // param.rc.i_rc_method = X264_RC_CRF;
  // param.rc.f_rf_constant = 20;
  //For streaming:
  // param.b_repeat_headers = 1;
  // param.b_annexb = 1;
  x264_param_apply_profile(&param, "baseline");

  x264_t* encoder = x264_encoder_open(&param);

  // x264_nal_t* nal;
  // int i_nals;

  // x264_encoder_headers(encoder, &nal, &i_nals);
  // memcpy(sei, nal[i].p_payload, nal[i].i_payload);


  x264_picture_t pic_in, pic_out;
  x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);

  struct SwsContext* convertCtx = sws_getContext(
    width,
    height,
    AV_PIX_FMT_BGRA,
    width,
    height,
    PIX_FMT_YUV420P,
    SWS_FAST_BILINEAR,
    NULL,
    NULL,
    NULL);


  struct sockaddr_in si_other;
  int s, i, slen=sizeof(si_other);

  if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    FAIL_EXIT("Cannot socket");

  int tmp = 1024*4*7;

  if(setsockopt(s, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp))<0)
    FAIL_EXIT("Cannot setsockopt");

  memset((char *) &si_other, 0, sizeof(si_other));
  si_other.sin_family = AF_INET;
  si_other.sin_port = htons(udpPort);
   
  if (inet_aton(targetHost , &si_other.sin_addr) == 0) 
      FAIL_EXIT("Inet_aton");

  LOG("Starting...\n");

  frame_num = 0;

  while(1)
  {  
    gettimeofday(&now,NULL);

    if(XShmGetImage(dpy, root, image, xOffset, yOffset, AllPlanes) == False)
      FAIL_EXIT("Cannot XShmGetImage");

    gettimeofday(&deltaX11,NULL);

    deltaX11.tv_sec -= now.tv_sec;
    deltaX11.tv_usec -= now.tv_usec;

    // outfile = fopen(outfileName,"w");

    if(outType == 1)
    {
      struct jpeg_compress_struct cinfo;
      struct jpeg_error_mgr jerr;
      JSAMPROW scanline[1];

      cinfo.err = jpeg_std_error(&jerr);
      jpeg_create_compress(&cinfo);

      // jpeg_stdio_dest(&cinfo, outfile);    
      jpeg_mem_dest(&cinfo, outbuffer, &outlen);

      cinfo.image_width = width;
      cinfo.image_height = height;
      cinfo.input_components = 4;
      cinfo.in_color_space = JCS_EXT_BGRX;

      jpeg_set_defaults(&cinfo);
      jpeg_set_quality(&cinfo, jpegQuality, TRUE);
      jpeg_start_compress(&cinfo, TRUE);

      while (cinfo.next_scanline < (unsigned int) height)
      {
        scanline[0] = image->data + 4 * width * cinfo.next_scanline;
        jpeg_write_scanlines(&cinfo, scanline, 1);
      }

      jpeg_finish_compress(&cinfo);
      jpeg_destroy_compress(&cinfo);
    }
    else if(outType == 2)
    {

      //data is a pointer to you RGBX structure
      int srcstride = width*4; //RGBX stride is just 4*width

      gettimeofday(&now,NULL);      

      sws_scale(convertCtx, &image->data, &srcstride, 0, height, pic_in.img.plane, pic_in.img.i_stride);
      pic_in.i_pts = frame_num;
      frame_num++;

      gettimeofday(&deltaFilter,NULL);

      deltaFilter.tv_sec -= now.tv_sec;
      deltaFilter.tv_usec -= now.tv_usec;

      x264_nal_t* nals;
      int i_nals;

      int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, &pic_in, &pic_out);

      if (frame_size >= 0)
      {
        outlen = frame_size;

        // int i;
        // int offset = 0;
 
        // for (i = 0; i < i_nals; i++){
        //   memcpy(outbuffer+offset, nals[i].p_payload, nals[i].i_payload);
        //   offset += nals[i].i_payload;
        // }

        // LOG("%d VS %d\n",outlen,offset);

        memcpy(outbuffer,nals[0].p_payload,frame_size);
        // nals[0].p_payload, frame_size
      }
      else
      {
        LOG("[ERROR] 264 frame size is invalid:%d\n",frame_size);        
      }
    }

    // LOG("pts:%d",pic_out.i_pts++);

    // fclose(outfile);
    // break;

    //send the message
    
    int alreadySent = 0;
    int sent = 0;

    gettimeofday(&now,NULL);

    do
    {
      sent = sendto(
        s,
        outbuffer+alreadySent,
        (outlen-alreadySent) > MAX_UDP_SIZE ? MAX_UDP_SIZE : outlen-alreadySent,
        0,
        (struct sockaddr *) &si_other,
        slen);

      if(sent == -1)
      {
        LOG("[ERROR] Sent eq -1\n");
      }
      else
      {
        LOG("sent %d \n",sent);        
        alreadySent += sent;
      }
    } while (sent != -1 && alreadySent != outlen); 
    
    // while((
    // {
    //   
    // }

    gettimeofday(&deltaSocket,NULL);

    deltaSocket.tv_sec -= now.tv_sec;
    deltaSocket.tv_usec -= now.tv_usec;    

    gettimeofday(&now,NULL);

    LOG("Delta x11/filter/socket/total: %d/%d/%d/%d Image size: %d Kib\n",
      deltaX11.tv_sec*1000+deltaX11.tv_usec/1000,
      deltaFilter.tv_sec*1000+deltaFilter.tv_usec/1000,
      deltaSocket.tv_sec*1000+deltaSocket.tv_usec/1000,
      (now.tv_sec-last.tv_sec)*1000 + (now.tv_usec-last.tv_usec)/1000,
      outlen/1024);

    last.tv_usec = now.tv_usec;
    last.tv_sec = now.tv_sec; 

    usleep(25000);
  }

  XShmDetach(dpy, &shminfo);
  shmdt(shminfo.shmaddr);
  shmctl(shminfo.shmid, IPC_RMID, NULL);

  XDestroyImage(image);

  XCloseDisplay(dpy);
}