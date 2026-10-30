// AVPro 1.9 Java contract from the APK, backed by system media decoding.
// Keep the original native plugin: Unity's render events and native getters
// call back through these Java methods after its real JNI bootstrap.
#include <CoreVideo/CoreVideo.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kl_jni_int.h"
#include "klepton.h"
#include "kl_glfb.h"
#include "kl_egl.h"
#include "kl_stream.h"

#define MANAGER "com/RenderHeads/AVProVideo/AVProMobileVideo"
#define PLAYER "com/RenderHeads/AVProVideo/AVProVideoPlayer"
typedef struct {
    int index,closed,playing,loop,muted,last_error,error_seen;
    float volume,rate,pan;
    void *host,*image,*object;
    unsigned texture;
    uint64_t uploaded,frames;
} avpro_player;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static avpro_player *players[256];
static pthread_once_t bootstrap_once=PTHREAD_ONCE_INIT;
static int bootstrapped;

static void bootstrap(void) {
    klj_val arg={.l=kl_jni_new_string("AVProLocal")};
    if(!klj_UnityPlayer_loadLibrary(kl_jni_env(),NULL,&arg,1).j)return;
    kl_image *image=kl_find_image("libAVProLocal.so");
    void (*init)(void *,void *)=image?kl_sym(image,"Java_com_RenderHeads_AVProVideo_AVProMobileVideo_nativeInit"):NULL;
    if(init){init(kl_jni_env(),kl_jni_class(MANAGER));bootstrapped=1;}
    KLJ_LOG("AVPro native bootstrap %s",bootstrapped?"ready":"unavailable");
}
static avpro_player *player(void *self) {
    klj_object *o=klj_as_object(self);
    return o&&!strcmp(o->cls,PLAYER)?o->data:NULL;
}
static void close_video(avpro_player *p) {
    if(p->host)kl_stream_close(p->host);
    p->host=NULL;p->playing=0;p->uploaded=p->frames=0;
    // Images and GL names are retired on the rendering thread.
}
static void render_one(avpro_player *p) {
    if(!kl_glfb_context_current())return;
    if(!p->host){
        if(p->image){kl_glfb_image_destroy(p->image);p->image=NULL;}
        if(p->texture){void(*del)(int,const unsigned*)=kl_egl_sym("glDeleteTextures");del(1,&p->texture);p->texture=0;}
        return;
    }
    uint64_t serial=0;CVPixelBufferRef frame=kl_stream_copy_frame(p->host,&serial);
    if(frame&&serial!=p->uploaded){
        if(!p->texture){void(*gen)(int,unsigned*)=kl_egl_sym("glGenTextures");gen(1,&p->texture);}
        void *image=kl_glfb_image_from_pixels(frame,NULL,NULL);
        if(image&&kl_glfb_image_bind_texture(image,p->texture)){
            if(p->image)kl_glfb_image_destroy(p->image);
            p->image=image;p->uploaded=serial;p->frames++;
        }else if(image)kl_glfb_image_destroy(image);
    }
    if(frame)CVPixelBufferRelease(frame);
}
static void render_all(void) {
    void *retired[256];int count=0;
    if(!kl_glfb_context_current())return;
    pthread_mutex_lock(&lock);
    for(int i=0;i<256;i++)if(players[i]){
        render_one(players[i]);
        if(players[i]->closed){retired[count++]=players[i]->object;players[i]=NULL;}
    }
    pthread_mutex_unlock(&lock);
    for(int i=0;i<count;i++)kl_jni_unpin_object(retired[i]);
}
static klj_val constructor(void *env,void *self,const klj_val *a,int n) {
    pthread_once(&bootstrap_once,bootstrap);
    return (klj_val){.l=kl_jni_new_object(MANAGER)};
}
static klj_val version(void *env,void *self,const klj_val *a,int n) {
    return (klj_val){.l=kl_jni_new_string("1.9.17")}; // Original Java literal.
}
static klj_val context(void *env,void *self,const klj_val *a,int n) {
    // AVPlayer doesn't require an Android Activity; no guest context escapes.
    return (klj_val){0};
}
static klj_val create(void *env,void *self,const klj_val *a,int n) {
    if(n!=5||(a[0].j!=1&&a[0].j!=2))return (klj_val){0};
    avpro_player *p=calloc(1,sizeof *p);if(!p)return (klj_val){0};
    p->volume=p->rate=1;
    void *object=klj_own(klj_new_object_data(PLAYER,p),free);kl_jni_pin_object(object);p->object=object;
    pthread_mutex_lock(&lock);int i;for(i=0;i<256&&players[i];i++);
    if(i<256){p->index=i;players[i]=p;}
    pthread_mutex_unlock(&lock);
    if(i==256){klj_as_object(object)->data=NULL;kl_jni_unpin_object(object);free(p);return (klj_val){0};}
    kl_glfb_add_render_service(render_all);
    KLJ_LOG("AVPro created system media player %d",i);
    return (klj_val){.l=object};
}
static klj_val destroy(void *env,void *self,const klj_val *a,int n) {
    int i=(int)a[0].j;
    pthread_mutex_lock(&lock);
    if(i>=0&&i<256&&players[i]){close_video(players[i]);players[i]->closed=1;}
    pthread_mutex_unlock(&lock);
    return (klj_val){0};
}
static klj_val destroy_all(void *env,void *self,const klj_val *a,int n) {
    pthread_mutex_lock(&lock);for(int i=0;i<256;i++)if(players[i]){close_video(players[i]);players[i]->closed=1;}
    pthread_mutex_unlock(&lock);return (klj_val){0};
}
static klj_val render_static(void *env,void *self,const klj_val *a,int n) {
    int i=n?(int)a[0].j:-1;pthread_mutex_lock(&lock);
    if(i>=0&&i<256&&players[i])render_one(players[i]);
    pthread_mutex_unlock(&lock);return (klj_val){0};
}
static klj_val render_destroyed(void *env,void *self,const klj_val *a,int n) {
    render_all();return (klj_val){0};
}
static klj_val open_video(void *env,void *self,const klj_val *a,int n) {
    const char *url=n?klj_str(a[0].l):NULL;char path[4096];
    // StreamingAssets is a jar:file URL on Android. The installed app already
    // stages these exact APK assets; AVPlayer needs their native file paths.
    const char *asset=url&&!strncmp(url,"jar:file:",9)?strstr(url,"!/assets/"):NULL;
    if(asset){
        asset+=9;
        if(!*asset||strstr(asset,"../")||strchr(asset,'%')||strchr(asset,'?')||strchr(asset,'#'))url=NULL;
        else {int len=snprintf(path,sizeof path,"%s/%s",kl_jni_assets_dir(),asset);url=len>0&&len<sizeof path?path:NULL;}
    }else if(url&&(url[0]=='/'||!strncmp(url,"file:///android_asset/",22)))url=kl_guest_path(url,path,sizeof path);
    pthread_mutex_lock(&lock);avpro_player *p=player(self);int accepted=0;
    if(p&&!p->closed){
        close_video(p);p->last_error=p->error_seen=0;p->host=kl_stream_new();
        if(!url||!url[0]||n<4||a[1].j||(a[2].l&&klj_str(a[2].l)&&*klj_str(a[2].l))){
            p->last_error=100;
            kl_stream_reject(p->host,4001,"This video source requires unsupported byte ranges or HTTP headers.");
        }else{
            kl_stream_volume(p->host,p->muted?0:p->volume);kl_stream_loop(p->host,p->loop);kl_stream_rate(p->host,p->rate);
            kl_stream_open(p->host,url,NULL);accepted=1;
        }
        KLJ_LOG("AVPro open player %d, accepted %d (source omitted)",p->index,accepted);
    }
    pthread_mutex_unlock(&lock);return (klj_val){.j=accepted};
}
#define ACTION(name,body) static klj_val name(void *env,void *self,const klj_val *a,int n) { \
    pthread_mutex_lock(&lock);avpro_player *p=player(self);if(p&&!p->closed){body;} \
    pthread_mutex_unlock(&lock);return (klj_val){0};}
ACTION(close_player,close_video(p))
ACTION(play,p->playing=1;kl_stream_play(p->host,1))
ACTION(pause_player,p->playing=0;kl_stream_play(p->host,0))
ACTION(stop,p->playing=0;kl_stream_play(p->host,0);kl_stream_seek(p->host,0))
ACTION(seek,kl_stream_seek(p->host,(int)a[0].j))
ACTION(volume,p->volume=fmaxf(0,fminf(1,a[0].d));kl_stream_volume(p->host,p->muted?0:p->volume))
ACTION(mute,p->muted=!!a[0].j;kl_stream_volume(p->host,p->muted?0:p->volume))
ACTION(loop,p->loop=!!a[0].j;kl_stream_loop(p->host,p->loop))
ACTION(rate,if(isfinite(a[0].d)&&a[0].d>=0){p->rate=a[0].d;kl_stream_rate(p->host,p->rate);})
ACTION(audio_track,if(a[0].j!=0)p->last_error=100)
ACTION(audio_pan,if(a[0].d!=0)p->last_error=100)
ACTION(enable_unsupported,if(a[0].j)p->last_error=100)
ACTION(update,/* State is polled from the system media backend. */)
#undef ACTION
static klj_val render_player(void *env,void *self,const klj_val *a,int n){
    pthread_mutex_lock(&lock);avpro_player *p=player(self);int rendered=0;
    if(p&&!p->closed){uint64_t before=p->frames;render_one(p);rendered=p->frames!=before;}
    pthread_mutex_unlock(&lock);return (klj_val){.j=rendered};
}

enum { READY,WIDTH,HEIGHT,DURATION,POSITION,TEXTURE,FRAMES,FPS,ERROR,PLAYING,PAUSED,ENDED,BUFFERING,SEEKING,AUDIO,VIDEO,TRACKS,INDEX,CLOSED,VOLUME,RATE,LOOP,MUTED,PAN,TIMESTAMP };
static klj_val info(void *self,const klj_val *a,int n,int kind,int is_static) {
    pthread_mutex_lock(&lock);int i=n?(int)a[0].j:-1;
    avpro_player *p=is_static?(i>=0&&i<256?players[i]:NULL):player(self);
    kl_stream_state s;kl_stream_snapshot(p?p->host:NULL,&s,0);klj_val v={0};
    if(p){
        switch(kind){
        case READY:v.j=s.ready;break;case WIDTH:v.j=s.width;break;case HEIGHT:v.j=s.height;break;
        case DURATION:v.j=s.duration_ms;break;case POSITION:v.j=s.position_ms;break;
        case TEXTURE:v.j=p->texture;break;case FRAMES:v.j=p->frames;break;case FPS:v.d=s.source_fps;break;
        case ERROR:if(s.error_code&&!p->error_seen){p->last_error=100;p->error_seen=1;}v.j=p->last_error;p->last_error=0;break;
        case PLAYING:v.j=s.playing;break;case PAUSED:v.j=s.ready&&!p->playing;break;case ENDED:v.j=s.ended;break;
        case BUFFERING:v.j=s.buffering;break;case SEEKING:v.j=s.seeking;break;
        case AUDIO:v.j=s.has_audio;break;case VIDEO:v.j=s.has_video;break;case TRACKS:v.j=s.audio_tracks;break;
        case INDEX:v.j=p->index;break;case CLOSED:v.j=p->closed;break;
        case VOLUME:v.d=p->volume;break;case RATE:v.d=p->rate;break;case LOOP:v.j=p->loop;break;case MUTED:v.j=p->muted;break;
        case PAN:v.d=p->pan;break;case TIMESTAMP:v.j=s.position_ms*10000;break;
        }
    }
    pthread_mutex_unlock(&lock);return v;
}
#define INFO(name,kind) static klj_val name(void *env,void *self,const klj_val *a,int n){return info(self,a,n,kind,0);}
INFO(ready,READY) INFO(width,WIDTH) INFO(height,HEIGHT) INFO(duration,DURATION) INFO(position,POSITION)
INFO(texture,TEXTURE) INFO(frames,FRAMES) INFO(fps,FPS) INFO(error,ERROR) INFO(playing,PLAYING) INFO(paused,PAUSED)
INFO(ended,ENDED) INFO(buffering,BUFFERING) INFO(seeking,SEEKING) INFO(audio,AUDIO) INFO(video,VIDEO) INFO(tracks,TRACKS)
INFO(player_index,INDEX) INFO(closed,CLOSED) INFO(get_volume,VOLUME) INFO(get_rate,RATE) INFO(get_loop,LOOP) INFO(get_mute,MUTED)
INFO(get_pan,PAN) INFO(timestamp,TIMESTAMP)
#undef INFO
#define STATIC_INFO(name,kind) static klj_val static_##name(void *env,void *self,const klj_val *a,int n){return info(self,a,n,kind,1);}
STATIC_INFO(ready,READY) STATIC_INFO(width,WIDTH) STATIC_INFO(height,HEIGHT) STATIC_INFO(duration,DURATION)
STATIC_INFO(texture,TEXTURE) STATIC_INFO(frames,FRAMES) STATIC_INFO(fps,FPS) STATIC_INFO(error,ERROR)
#undef STATIC_INFO
static klj_val zero(void *env,void *self,const klj_val *a,int n){return (klj_val){0};}
static klj_val transform(void *env,void *self,const klj_val *a,int n){
    void *array=klj_new_array('F',NULL,16);float *m=klj_arr(array)->data;
    m[0]=m[10]=m[15]=1;m[5]=-1;m[13]=1;return (klj_val){.l=array};
}
static klj_val seek_range(void *env,void *self,const klj_val *a,int n){
    kl_stream_state s;pthread_mutex_lock(&lock);avpro_player *p=player(self);
    kl_stream_snapshot(p?p->host:NULL,&s,0);pthread_mutex_unlock(&lock);
    void *array=klj_new_array('F',NULL,2);float *v=klj_arr(array)->data;v[0]=s.seek_start;v[1]=s.seek_end;
    return (klj_val){.l=array};
}
static klj_val buffered(void *env,void *self,const klj_val *a,int n){
    kl_stream_state s;pthread_mutex_lock(&lock);avpro_player *p=player(self);
    kl_stream_snapshot(p?p->host:NULL,&s,0);pthread_mutex_unlock(&lock);
    return (klj_val){.d=s.buffered_fraction*100};
}
#define M(name,sig,fn) {MANAGER,name,sig,fn}
#define P(name,sig,fn) {PLAYER,name,sig,fn}
const klj_binding klj_bind_avpro[]={
    M("<init>","()V",constructor),M("GetPluginVersion","()Ljava/lang/String;",version),
    M("SetContext","(Landroid/content/Context;)V",context),M("SetContext","(Lcom/unity3d/player/UnityPlayerActivity;)V",context),
    M("CreatePlayer","(IZZIZ)Lcom/RenderHeads/AVProVideo/AVProVideoPlayer;",create),M("DestroyPlayer","(I)V",destroy),
    M("Deinitialise","()V",destroy_all),M("RendererDestroyPlayers","()V",render_destroyed),
    M("RendererSetupPlayer","(II)V",render_static),M("RendererReset","(I)V",render_static),M("RenderPlayer","(I)V",render_static),
    M("WaitForNewFramePlayer","(I)V",zero),
    M("_CanPlay","(I)Z",static_ready),M("_GetWidth","(I)I",static_width),M("_GetHeight","(I)I",static_height),
    M("_GetDuration","(I)J",static_duration),M("_GetTextureHandle","(I)I",static_texture),M("_GetFrameCount","(I)I",static_frames),
    M("_GetVideoDisplayRate","(I)F",static_fps),M("_GetLastErrorCode","(I)I",static_error),
    P("OpenVideoFromFile","(Ljava/lang/String;JLjava/lang/String;I)Z",open_video),
    P("CloseVideo","()V",close_player),P("Deinitialise","()V",close_player),P("DeinitialiseRender","()V",close_player),
    P("SetDeinitialiseFlagged","()V",close_player),P("Update","()V",update),P("RendererSetup","(I)V",update),P("Render","()Z",render_player),
    P("Play","()V",play),P("Pause","()V",pause_player),P("Stop","()V",stop),P("Seek","(I)V",seek),P("SeekFast","(I)V",seek),
    P("SetVolume","(F)V",volume),P("MuteAudio","(Z)V",mute),P("SetLooping","(Z)V",loop),P("SetPlaybackRate","(F)V",rate),
    P("SetAudioTrack","(I)V",audio_track),P("SetAudioPan","(F)V",audio_pan),P("SetPlayerOptions","(ZZ)V",update),
    P("SetFocusEnabled","(Z)V",enable_unsupported),P("SetFocusProps","(FF)V",update),P("SetFocusRotation","(FFFF)V",update),
    P("SetHeadRotation","(FFFF)V",update),P("SetPositionTrackingEnabled","(Z)V",enable_unsupported),
    P("CanPlay","()Z",ready),P("GetWidth","()I",width),P("GetHeight","()I",height),P("GetDurationMs","()J",duration),
    P("GetCurrentTimeMs","()J",position),P("GetTextureHandle","()I",texture),P("GetFrameCount","()I",frames),
    P("GetSourceVideoFrameRate","()F",fps),P("GetDisplayRate","()F",fps),P("GetLastErrorCode","()I",error),
    P("IsPlaying","()Z",playing),P("IsPaused","()Z",paused),P("IsFinished","()Z",ended),P("IsBuffering","()Z",buffering),P("IsSeeking","()Z",seeking),
    P("HasAudio","()Z",audio),P("HasVideo","()Z",video),P("GetNumberAudioTracks","()I",tracks),P("GetPlayerIndex","()I",player_index),
    P("GetDeinitialised","()Z",closed),P("GetVolume","()F",get_volume),P("GetPlaybackRate","()F",get_rate),
    P("IsLooping","()Z",get_loop),P("IsMuted","()Z",get_mute),P("GetAudioPan","()F",get_pan),
    P("GetTextureTimeStamp","()J",timestamp),P("GetTextureTransform","()[F",transform),P("GetSeekableTimeRange","()[F",seek_range),
    P("GetEstimatedBandwidthUsed","()J",zero),P("GetCurrentAudioTrackIndex","()I",zero),P("GetCurrentAbsoluteTimestamp","()D",zero),
    P("HasSubtitles","()Z",zero),P("HasTimedText","()Z",zero),P("GetBufferingProgressPercent","()F",buffered),
    P("StartExtractFrame","()Z",zero),P("WaitForExtract","()V",zero),P("WaitForNewFrame","()V",zero),
    {0}
};
