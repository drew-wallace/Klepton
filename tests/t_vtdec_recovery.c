// Real decoder recovery with authored frames. Including the implementation
// lets this test invalidate the private VT session without a production hook.
#include <assert.h>
#include <unistd.h>
#include "../runtime/media/kl_vtdec.c"

typedef struct { uint8_t *data; size_t length; } access_unit;
static access_unit avc[4];
static void append(access_unit *au, const uint8_t *data, size_t size) {
    uint8_t *p = realloc(au->data, au->length + size + 4); assert(p);
    au->data = p;
    memcpy(p + au->length, "\0\0\0\1", 4);
    memcpy(p + au->length + 4, data, size); au->length += size + 4;
}
static void encoded(void *context, void *frame, OSStatus status,
                    VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    assert(!status && sample);
    int index = (int)(uintptr_t)frame - 1; assert(index >= 0 && index < 4);
    access_unit *au = &avc[index];
    if (!index) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        for (size_t i = 0; i < 2; i++) {
            const uint8_t *p; size_t size;
            assert(!CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format,i,&p,&size,NULL,NULL));
            append(au,p,size);
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t size = CMBlockBufferGetDataLength(block);
    uint8_t *bytes = malloc(size); assert(bytes);
    assert(!CMBlockBufferCopyDataBytes(block,0,size,bytes));
    for (size_t at = 0; at + 4 <= size;) {
        size_t n = (size_t)bytes[at]<<24 | (size_t)bytes[at+1]<<16 |
                   (size_t)bytes[at+2]<<8 | bytes[at+3];
        at += 4; assert(n && at + n <= size); append(au,bytes+at,n); at += n;
    }
    free(bytes);
}
static void make_avc(void) {
    VTCompressionSessionRef session = NULL;
    assert(!VTCompressionSessionCreate(NULL,64,64,kCMVideoCodecType_H264,NULL,NULL,
                                      NULL,encoded,NULL,&session));
    assert(!VTSessionSetProperty(session,kVTCompressionPropertyKey_RealTime,kCFBooleanTrue));
    assert(!VTSessionSetProperty(session,kVTCompressionPropertyKey_AllowFrameReordering,kCFBooleanFalse));
    assert(!VTCompressionSessionPrepareToEncodeFrames(session));
    for (int i = 0; i < 4; i++) {
        CVPixelBufferRef image = NULL;
        assert(!CVPixelBufferCreate(NULL,64,64,kCVPixelFormatType_32BGRA,NULL,&image));
        CVPixelBufferLockBaseAddress(image,0);
        uint8_t *p = CVPixelBufferGetBaseAddress(image);
        size_t stride = CVPixelBufferGetBytesPerRow(image);
        for (int y=0;y<64;y++) for (int x=0;x<64;x++) {
            uint8_t *c=p+y*stride+x*4;
            c[0]=x*3;c[1]=y*3;c[2]=255-x*3;c[3]=255;
        }
        CVPixelBufferUnlockBaseAddress(image,0);
        CFDictionaryRef options = NULL;
        if (i==0 || i==3) {
            const void *key=kVTEncodeFrameOptionKey_ForceKeyFrame,*value=kCFBooleanTrue;
            options=CFDictionaryCreate(NULL,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        }
        assert(!VTCompressionSessionEncodeFrame(session,image,CMTimeMake(i,30),
            CMTimeMake(1,30),options,(void*)(uintptr_t)(i+1),NULL));
        if(options)CFRelease(options);CVPixelBufferRelease(image);
    }
    assert(!VTCompressionSessionCompleteFrames(session,kCMTimeInvalid));
    VTCompressionSessionInvalidate(session);CFRelease(session);
    for(int i=0;i<4;i++)assert(avc[i].length);
}
static CVPixelBufferRef frame_at(kl_vtdec *decoder,int64_t expected) {
    for(int i=0;i<2000;i++) {
        int64_t pts=-1;CVPixelBufferRef image=kl_vtdec_pull(decoder,&pts);
        if(image){
            assert(pts==expected);
            assert(CVPixelBufferGetPixelFormatType(image)==kCVPixelFormatType_32BGRA);
            CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
            const uint8_t *bytes=CVPixelBufferGetBaseAddress(image);
            size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
            size_t stride=CVPixelBufferGetBytesPerRow(image);int varied=0;
            for(size_t y=0;y<height;y++)for(size_t x=0;x<width;x++)
                varied|=memcmp(bytes,bytes+y*stride+x*4,3)!=0;
            CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
            assert(varied);return image;
        }
        usleep(1000);
    }
    assert(!"Recovered decoder did not produce a frame");return NULL;
}
static void check(const char *mime,access_unit key,access_unit dependent) {
    kl_vtdec *d=kl_vtdec_create(mime);assert(d);
    assert(!kl_vtdec_submit(d,key.data,key.length,0));
    CVPixelBufferRef first=frame_at(d,0);
    assert(CVPixelBufferGetWidth(first)>0);CVPixelBufferRelease(first);
    // Invalidate the actual OS decoder, exactly the permanent failure observed
    // after the headset's asynchronous malfunction. No fake decoded output.
    VTDecompressionSessionInvalidate(d->sess);
    assert(kl_vtdec_submit(d,dependent.data,dependent.length,100000)<0);
    assert(d->session_failure==kVTInvalidSessionErr);
    unsigned submitted=d->n_submitted;
    for(int i=0;i<20;i++)assert(!kl_vtdec_submit(d,dependent.data,dependent.length,200000+i));
    assert(d->n_submitted==submitted&&d->n_sess_created==1);

    // Recovery must work when the host does not resend codec configuration.
    access_unit random_access={0},config={0};size_t pos=0,n=0;const uint8_t *nal;
    while((nal=next_nal(key.data,key.length,&pos,&n))) {
        if(param_slot(d->codec,nal_type(d->codec,nal[0]))<0)append(&random_access,nal,n);
        else append(&config,nal,n);
    }
    assert(!kl_vtdec_submit(d,config.data,config.length,300000));
    assert(d->n_sess_created==1); // Config alone cannot restart inter-frame decode.
    assert(!kl_vtdec_submit(d,random_access.data,random_access.length,1000000));
    CVPixelBufferRef recovered=frame_at(d,1000000);
    assert(CVPixelBufferGetWidth(recovered)>0);CVPixelBufferRelease(recovered);
    assert(d->n_sess_created==2&&d->n_recoveries==1);

    // The async failure callback schedules recovery; it must not wait on or
    // destroy its own session. Bad-packet errors leave a healthy session alone.
    VTDecompressionSessionRef previous=d->sess;
    klvt_output(d,NULL,kVTVideoDecoderMalfunctionErr,0,NULL,kCMTimeInvalid,kCMTimeInvalid);
    assert(d->sess==previous&&d->n_sess_created==2);
    assert(!kl_vtdec_submit(d,dependent.data,dependent.length,1100000));
    assert(d->n_sess_created==2);
    assert(!kl_vtdec_submit(d,random_access.data,random_access.length,2000000));
    recovered=frame_at(d,2000000);CVPixelBufferRelease(recovered);
    assert(d->n_sess_created==3&&d->n_recoveries==2);
    klvt_output(d,NULL,kVTVideoDecoderBadDataErr,0,NULL,kCMTimeInvalid,kCMTimeInvalid);
    assert(!d->session_failure);
    assert(!kl_vtdec_submit(d,random_access.data,random_access.length,3000000));
    recovered=frame_at(d,3000000);CVPixelBufferRelease(recovered);
    assert(d->n_sess_created==3);
    printf("%s: real invalid-session recovery, async malfunction, cached configuration, keyframe gating and timestamps passed\n",mime);
    kl_vtdec_destroy(d);free(random_access.data);free(config.data);
}
int main(void) {
    setenv("KL_VTDEC_BGRA","1",1);
    make_avc();check("video/avc",avc[0],avc[1]);
    for(int i=0;i<4;i++)free(avc[i].data);
    FILE *f=fopen("tests/data/t_hevc.h265","rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long length=ftell(f);rewind(f);
    uint8_t *bytes=malloc((size_t)length);assert(bytes);
    assert(fread(bytes,1,(size_t)length,f)==(size_t)length);fclose(f);
    access_unit hevc[2]={0};size_t pos=0,n=0;const uint8_t *nal;int index=0;
    while(index<2&&(nal=next_nal(bytes,(size_t)length,&pos,&n))) {
        append(&hevc[index],nal,n);if(nal_type(KLVT_HEVC,nal[0])<=31)index++;
    }
    assert(index==2);check("video/hevc",hevc[0],hevc[1]);
    free(bytes);free(hevc[0].data);free(hevc[1].data);
}
