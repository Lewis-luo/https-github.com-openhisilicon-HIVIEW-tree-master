#include <assert.h>
#include "codec.h"
#include "live.h"
#include "mpp.h"

typedef struct {
  pthread_t tid;
  pthread_mutex_t mutex;

  gsf_layout_t  ly1, ly2;
  struct cfifo_ex* cfifo[GSF_CODEC_NVR_CHN];
}ly_mng_t;

static ly_mng_t gmng;
static void* live_task(void *parm);

#define PIC_WIDTH(w) \
            (w >= 3840)?PIC_3840x2160:\
            (w >= 1920)?PIC_1080P:\
            (w >= 1280)?PIC_720P: \
            PIC_D1_NTSC
            
#define PT_VENC(t) \
            (t == GSF_ENC_H264)? PT_H264:\
            (t == GSF_ENC_H265)? PT_H265:\
            (t == GSF_ENC_JPEG)? PT_JPEG:\
            (t == GSF_ENC_MJPEG)? PT_MJPEG:\
            PT_H264


#define FRAME_MAX_SIZE (800*1024)

static unsigned int cfifo_recsize(unsigned char *p1, unsigned int n1, unsigned char *p2)
{
    unsigned int size = sizeof(gsf_frm_t);

    if(n1 >= size)
    {
        gsf_frm_t *rec = (gsf_frm_t*)p1;
        return  sizeof(gsf_frm_t) + rec->size;
    }
    else
    {
        gsf_frm_t rec;
        char *p = (char*)(&rec);
        memcpy(p, p1, n1);
        memcpy(p+n1, p2, size-n1);
        return  sizeof(gsf_frm_t) + rec.size;
    }
    
    return 0;
}

static unsigned int cfifo_rectag(unsigned char *p1, unsigned int n1, unsigned char *p2)
{
    unsigned int size = sizeof(gsf_frm_t);

    if(n1 >= size)
    {
        gsf_frm_t *rec = (gsf_frm_t*)p1;
        return rec->flag & GSF_FRM_FLAG_IDR;
    }
    else
    {
        gsf_frm_t rec;
        char *p = (char*)(&rec);
        memcpy(p, p1, n1);
        memcpy(p+n1, p2, size-n1);
        return rec.flag & GSF_FRM_FLAG_IDR;
    }
    
    return 0;
}

static unsigned int cfifo_recgut(unsigned char *p1, unsigned int n1, unsigned char *p2, void *u)
{
    unsigned int len = cfifo_recsize(p1, n1, p2);
    unsigned int l = CFIFO_MIN(len, n1);
    
    char *p = (char*)u;
    memcpy(p, p1, l);
    memcpy(p+l, p2, len-l);

    gsf_frm_t *rec = (gsf_frm_t *)u;
  	struct timespec _ts;  
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    int cost = (_ts.tv_sec*1000 + _ts.tv_nsec/1000000) - rec->utc;
    if(cost > 30*2)
      printf("u:%p, get rec ok [delay:%d ms].\n", u, cost);
      
    assert(rec->data[0] == 00 && rec->data[1] == 00 && rec->data[2] == 00 && rec->data[3] == 01);

    return len;
}


int live_mon()
{
  int i = 0;
  memset(&gmng, 0, sizeof(gmng));
  for(i = 0; i < GSF_CODEC_NVR_CHN; i++)
  {
    gmng.ly1.shmid[i].video_shmid = gmng.ly1.shmid[i].audio_shmid = -1;
    gmng.ly2.shmid[i].video_shmid = gmng.ly2.shmid[i].audio_shmid = -1;
  }
  
  pthread_mutex_init(&gmng.mutex, NULL);
  return pthread_create(&gmng.tid, NULL, live_task, (void*)NULL);
}

int live_ly(gsf_layout_t *ly)
{
  int i = 0;
  pthread_mutex_lock(&gmng.mutex);
  gmng.ly1 = *ly;
  for(i = 0; i < gmng.ly1.layout; i++)
    printf("ly1.layout:%d, ly1.shmid[%d].video_shmid:%d\n"
          , gmng.ly1.layout, i, gmng.ly1.shmid[i].video_shmid);
          
  pthread_mutex_unlock(&gmng.mutex);
}

static void* live_task(void *parm)
{
  int i = 0, ret = 0;
  int ep = cfifo_ep_alloc(1);
  gsf_frm_t *frm = (gsf_frm_t*)malloc(FRAME_MAX_SIZE);
  
  while(1)
  {
    
    pthread_mutex_lock(&gmng.mutex);
    //setp 1: close ly2;
    for(i = 0; i < gmng.ly2.layout; i++)
    {
      if( gmng.ly2.shmid[i].video_shmid != gmng.ly1.shmid[i].video_shmid)
      {
        if(gmng.ly2.shmid[i].video_shmid >= 0 && gmng.cfifo[i])
        {
          printf("close >>>>> i:%d, ly2.video_shmid:%d\n"
              , i, gmng.ly2.shmid[i].video_shmid);
              
          void* ch = cfifo_get_u(gmng.cfifo[i]);
          gsf_mpp_vo_clear(VOLAYER_HD0, (int)ch);
          
          gsf_mpp_venc_ctl((int)ch*GSF_CODEC_VENC_NUM, GSF_MPP_VENC_CTL_RST, NULL);
          
          cfifo_ep_ctl(ep, CFIFO_EP_DEL, gmng.cfifo[i]);
          cfifo_free(gmng.cfifo[i]);
          gmng.cfifo[i] = NULL;
          gmng.ly2.shmid[i].video_shmid = -1;
        }
      }
    }
    //setp 2: do ly1;
    if(gmng.ly2.layout != gmng.ly1.layout)
    {
      gmng.ly2.layout = gmng.ly1.layout;
      gsf_mpp_vo_layout(VOLAYER_HD0, gmng.ly2.layout, NULL);
    }
    //setp 3: open ly1;
    for(i = 0; i < gmng.ly2.layout; i++)
    {
      if(gmng.ly2.shmid[i].video_shmid != gmng.ly1.shmid[i].video_shmid)
      {
        gmng.ly2.shmid[i].video_shmid = gmng.ly1.shmid[i].video_shmid;
        
        if(gmng.ly2.shmid[i].video_shmid >= 0)
        {
          gmng.cfifo[i] = cfifo_shmat(cfifo_recsize
                                    , cfifo_rectag
                                    , gmng.ly2.shmid[i].video_shmid);
                                    
          printf("open >>>>> i:%d, ly2.video_shmid:%d, cfifo:%p\n"
              , i, gmng.ly2.shmid[i].video_shmid, gmng.cfifo[i]);
              
          if(gmng.cfifo[i] == NULL)
          {
            continue;
          }
          cfifo_set_u(gmng.cfifo[i], (void*)i);
          cfifo_newest(gmng.cfifo[i], 1);
          cfifo_ep_ctl(ep, CFIFO_EP_ADD, gmng.cfifo[i]);
        } 
      }
    }
    pthread_mutex_unlock(&gmng.mutex);

    struct cfifo_ex* result[255] = {0};
    int fds = cfifo_ep_wait(ep, 2000, result, 255);
    if(fds <= 0)
    {
       printf("cfifo_ep_wait err fds:%d\n", fds);
    }
    
    //printf("cfifo_ep_wait ok fds:%d\n", fds);
    
    for(i = 0; i < fds; i++)
    {
      do{
          struct cfifo_ex* fifo = result[i];
          ret = cfifo_get(fifo, cfifo_recgut, (void*)frm);
          if(ret <= 0)
          {
            break;
          }
          
          void* ch = cfifo_get_u(fifo);
          char *data = frm->data;
          gsf_mpp_frm_attr_t attr;
          attr.size   = frm->size;          // data size;
          attr.ftype  = frm->flag;          // frame type;
          attr.etype  = PT_VENC(frm->video.encode);// PAYLOAD_TYPE_E;
          attr.width  = frm->video.width;   // width;
          attr.height = frm->video.height;  // height;
          attr.pts    = frm->pts*1000; // pts ms*1000;
          ret = gsf_mpp_vo_vsend(VOLAYER_HD0, (int)ch, data, &attr);
      }while(1);
    }
  }
  
  return NULL;
}