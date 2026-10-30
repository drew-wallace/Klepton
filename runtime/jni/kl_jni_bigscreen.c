// Bigscreen browser/media adapters. Streams use the system AVPlayer; callback
// registration alone never reports success, and protected media stays explicit.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <CoreVideo/CoreVideo.h>
#include "kl_jni_int.h"
#include "kl_egl.h"
#include "kl_jni_vuplex.h"
#include "kl_jni_bigscreen.h"
#include "kl_glfb.h"
#include "kl_stream.h"

static pthread_mutex_t callback_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char *object, *method; } media_callbacks[12];
static void *media_context;
static void *web_context;
static atomic_int media_debug;
typedef struct bigscreen_player {
    int id, autoplay, maximum_bitrate, maximum_height;
    unsigned texture;
    char *language;
    void *host,*image;
    uint64_t uploaded,notified;
    unsigned generation;
    float volume;
    int destroyed,pending_drm;
    struct bigscreen_player *next;
} bigscreen_player;
static bigscreen_player *players;
static int next_player_id;
typedef struct bigscreen_web_client {
    int id;
    void *view;
    struct bigscreen_web_client *next;
} bigscreen_web_client;
static bigscreen_web_client *web_clients;
static int next_web_client_id;
static unsigned web_texture;

static unsigned create_external_texture(unsigned texture) {
    void (*gen)(int, unsigned *) = kl_egl_sym("glGenTextures");
    if (!texture) gen(1, &texture);
    // Storage and external sampling defaults arrive with the first real frame.
    // Allocating the name must not change Unity's current texture binding.
    return texture;
}

static klj_val create_web_client(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    // BigWebClient's constructor creates an ID and empty state. SetupClient
    // later supplies the WebKit-backed native view.
    bigscreen_web_client *client = calloc(1, sizeof *client);
    if (!client) return (klj_val){.j = (uint64_t)-1};
    pthread_mutex_lock(&callback_lock);
    client->id = next_web_client_id++;
    client->next = web_clients;
    web_clients = client;
    pthread_mutex_unlock(&callback_lock);
    KLJ_LOG("Bigscreen created empty web client %d (no document)", client->id);
    return (klj_val){.j = (uint32_t)client->id};
}

static klj_val render_web_clients(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    pthread_mutex_lock(&callback_lock);
    if (!web_texture) web_texture = create_external_texture(0);
    pthread_mutex_unlock(&callback_lock);
    // This shared texture belongs to Android custom full-screen video views.
    // Normal page pixels are uploaded by Vuplex.render into each view texture.
    return (klj_val){0};
}

static klj_val get_web_texture(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    pthread_mutex_lock(&callback_lock);
    unsigned texture = web_texture;
    pthread_mutex_unlock(&callback_lock);
    return (klj_val){.j = texture ? texture : (uint64_t)-1};
}


static klj_val setup_web_client(void *env,void *self,const klj_val *a,int n) {
    void *view=a[1].l, *previous=NULL;
    if(view)kl_jni_pin_object(view);
    int found=0;
    pthread_mutex_lock(&callback_lock);
    for(bigscreen_web_client *c=web_clients;c;c=c->next) if(c->id==(int)a[0].j) {previous=c->view;c->view=view;found=1;break;}
    pthread_mutex_unlock(&callback_lock);
    if(previous)kl_jni_unpin_object(previous);
    if(!found&&view)kl_jni_unpin_object(view);
    return (klj_val){0};
}
static void *web_client_view(int id) {
    void *view=NULL;
    pthread_mutex_lock(&callback_lock);
    for(bigscreen_web_client *c=web_clients;c;c=c->next) if(c->id==id) {view=c->view;break;}
    pthread_mutex_unlock(&callback_lock);
    return view;
}
// BigWebClient dispatches DOWN/UP as Android touch events, even with its
// DEFAULT_BUTTON_ID=1. These are primary taps, not Vuplex MouseButton.Right.
// Keep the direct Vuplex mouse API's explicit right/middle buttons unchanged.
#define WEB_POINTER(name,type) static klj_val name(void *env,void *self,const klj_val *a,int n) { \
    if(n>=4&&(int)a[n-1].j==2)klj_vuplex_pointer(web_client_view((int)a[0].j),type,(int)a[1].j,(int)a[2].j,0);return (klj_val){0};}
WEB_POINTER(web_pointer_down,"pointerdown")
WEB_POINTER(web_pointer_up,"pointerup")
WEB_POINTER(web_pointer_move,"pointermove")
static klj_val web_scroll(void *env,void *self,const klj_val *a,int n) {
    if((int)a[5].j==2)klj_vuplex_scroll(web_client_view((int)a[0].j),(int)a[1].j,(int)a[2].j,(int)a[3].j,(int)a[4].j);
    return (klj_val){0};
}
static klj_val web_cancel(void *env,void *self,const klj_val *a,int n) {
    klj_vuplex_pointer(web_client_view((int)a[0].j),"pointercancel",(int)a[1].j,(int)a[2].j,0);return (klj_val){0};
}
static klj_val web_fullscreen(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    return (klj_val){.j=klj_vuplex_is_fullscreen(web_client_view((int)args[0].j))};
}
static klj_val web_exit_fullscreen(void *env,void *self,const klj_val *args,int count) {
    klj_vuplex_exit_fullscreen(web_client_view((int)args[0].j));return (klj_val){0};
}

static bigscreen_player *find_player(int id) {
    for (bigscreen_player *p = players; p; p = p->next)
        if (p->id == id&&!p->destroyed) return p;
    return NULL;
}

static klj_val initiate_player(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    pthread_mutex_lock(&callback_lock);
    bigscreen_player *player = find_player((int)args[0].j);
    if (player) player->texture = create_external_texture(player->texture);
    pthread_mutex_unlock(&callback_lock);
    KLJ_LOG("Bigscreen initialized media texture for player %d", (int)args[0].j);
    return (klj_val){0};
}

static klj_val update_players(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    klj_bigscreen_drain_events();
    return (klj_val){0};
}

static klj_val seek_idle_player(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    double seconds=count>1?args[1].d:0;
    pthread_mutex_lock(&callback_lock);bigscreen_player *p=find_player((int)args[0].j);
    if(p&&isfinite(seconds)&&seconds>=0&&seconds<(double)INT64_MAX/1000.0)kl_stream_seek(p->host,(int64_t)(seconds*1000));
    pthread_mutex_unlock(&callback_lock);
    return (klj_val){0};
}

static void stop_player(bigscreen_player *p) {
    p->generation++;
    if(p->host){kl_stream_close(p->host);p->host=NULL;}
    if(p->image){kl_glfb_image_destroy(p->image);p->image=NULL;}
    p->uploaded=p->notified=0;p->pending_drm=0;
}
static klj_val start_stream(void *env,void *self,const klj_val *a,int n) {
    (void)env;(void)self;
    pthread_mutex_lock(&callback_lock);bigscreen_player *p=find_player((int)a[0].j);
    if(p){
        stop_player(p);p->host=kl_stream_new();
        kl_glfb_add_render_service(klj_bigscreen_render);
        const char *mime=n>2?klj_str(a[2].l):NULL;
        kl_stream_play(p->host,p->autoplay);kl_stream_volume(p->host,p->volume);
        if(n>3&&a[3].l)kl_stream_reject(p->host,4001,"External video surfaces are not supported by this player.");
        else kl_stream_open(p->host,klj_str(a[1].l),mime);
        kl_stream_limits(p->host,p->maximum_bitrate,p->maximum_height);
        KLJ_LOG("Bigscreen stream requested for player %d (source omitted)",p->id);
    }
    pthread_mutex_unlock(&callback_lock);return (klj_val){0};
}
static klj_val protected_stream(void *env,void *self,const klj_val *a,int n) {
    (void)env;(void)self;(void)n;
    pthread_mutex_lock(&callback_lock);bigscreen_player *p=find_player((int)a[0].j);
    if(p){stop_player(p);p->pending_drm=1;}
    pthread_mutex_unlock(&callback_lock);return (klj_val){0};
}
#define TRANSPORT(name,command) \
static klj_val name(void *env,void *self,const klj_val *a,int n) { \
    (void)env;(void)self;(void)n;pthread_mutex_lock(&callback_lock);bigscreen_player *p=find_player((int)a[0].j); \
    if(p){command;}pthread_mutex_unlock(&callback_lock);return (klj_val){0};}
TRANSPORT(player_play,kl_stream_play(p->host,1))
TRANSPORT(player_pause,kl_stream_play(p->host,0))
TRANSPORT(player_stop,stop_player(p))
TRANSPORT(player_shutdown,stop_player(p))
TRANSPORT(player_autoplay,p->autoplay=!!a[1].j)
TRANSPORT(player_volume,p->volume=fmaxf(0,fminf(1,a[1].d));kl_stream_volume(p->host,p->volume))
TRANSPORT(player_bitrate,p->maximum_bitrate=(int)a[1].j;kl_stream_limits(p->host,p->maximum_bitrate,p->maximum_height))
TRANSPORT(player_height,p->maximum_height=(int)a[1].j;kl_stream_limits(p->host,p->maximum_bitrate,p->maximum_height))
#undef TRANSPORT
static klj_val stop_all(void *env,void *self,const klj_val *a,int n) {
    (void)env;(void)self;(void)a;(void)n;pthread_mutex_lock(&callback_lock);
    for(bigscreen_player *p=players;p;p=p->next){stop_player(p);p->destroyed=1;}
    pthread_mutex_unlock(&callback_lock);return (klj_val){0};
}
static klj_val frame_available(void *env,void *self,const klj_val *a,int n) {
    (void)env;(void)self;(void)n;pthread_mutex_lock(&callback_lock);
    bigscreen_player *p=find_player((int)a[0].j);kl_stream_state state;kl_stream_snapshot(p?p->host:NULL,&state,0);
    int fresh=p&&state.has_frame&&state.serial!=p->notified;
    if(fresh)p->notified=state.serial;
    pthread_mutex_unlock(&callback_lock);return (klj_val){.j=fresh};
}
enum {INFO_DURATION,INFO_POSITION,INFO_WIDTH,INFO_HEIGHT,INFO_TEXTURE,INFO_VOLUME};
static klj_val player_info(const klj_val *a,int kind) {
    pthread_mutex_lock(&callback_lock);bigscreen_player *p=find_player((int)a[0].j);
    kl_stream_state s;kl_stream_snapshot(p?p->host:NULL,&s,0);klj_val value={.j=(uint64_t)-1};
    if(p){
        switch(kind){
        case INFO_DURATION:value.j=s.duration_ms;break;
        case INFO_POSITION:value.j=s.position_ms;break;
        case INFO_WIDTH:value.j=s.width;break;
        case INFO_HEIGHT:value.j=s.height;break;
        case INFO_VOLUME:value.d=p->volume;break;
        case INFO_TEXTURE:{
            if(s.has_frame&&s.serial!=p->uploaded&&kl_glfb_context_current()){
                uint64_t serial=0;CVPixelBufferRef pixel=kl_stream_copy_frame(p->host,&serial);
                if(pixel){
                    if(!p->texture)p->texture=create_external_texture(0);
                    void *image=kl_glfb_image_from_pixels(pixel,NULL,NULL);
                    if(image&&kl_glfb_image_bind_texture(image,p->texture)){
                        if(p->image)kl_glfb_image_destroy(p->image);p->image=image;p->uploaded=serial;
                    }else if(image)kl_glfb_image_destroy(image);
                    CVPixelBufferRelease(pixel);
                }
            }
            value.j=p->texture?p->texture:(uint64_t)-1;break;
        }}
    }
    pthread_mutex_unlock(&callback_lock);return value;
}
#define INFO(name,kind) static klj_val name(void *e,void *o,const klj_val *a,int n){(void)e;(void)o;(void)n;return player_info(a,kind);}
INFO(player_duration,INFO_DURATION) INFO(player_position,INFO_POSITION)
INFO(player_width,INFO_WIDTH) INFO(player_texture_height,INFO_HEIGHT)
INFO(player_texture,INFO_TEXTURE) INFO(player_get_volume,INFO_VOLUME)
#undef INFO
void klj_bigscreen_render(void) {
    if(!kl_glfb_context_current())return;
    pthread_mutex_lock(&callback_lock);bigscreen_player *next=players;pthread_mutex_unlock(&callback_lock);
    while(next){
        pthread_mutex_lock(&callback_lock);int id=next->id,active=!next->destroyed&&next->host;next=next->next;pthread_mutex_unlock(&callback_lock);
        if(active){klj_val a={.j=id};player_info(&a,INFO_TEXTURE);}
    }
}
static klj_val video_size(void *env,void *self,const klj_val *a,int n) {
    (void)env;(void)self;(void)n;void *array=klj_new_array('I',NULL,2);
    int32_t *v=klj_arr(array)->data;v[0]=(int32_t)player_info(a,INFO_WIDTH).j;v[1]=(int32_t)player_info(a,INFO_HEIGHT).j;
    if(v[0]<0)v[0]=0;if(v[1]<0)v[1]=0;
    return (klj_val){.l=array};
}
static klj_val transform(void *env,void *self,const klj_val *a,int n) {
    (void)env;(void)self;(void)a;(void)n;void *array=klj_new_array('F',NULL,16);float *m=klj_arr(array)->data;
    m[0]=m[10]=m[15]=1;m[5]=-1;m[13]=1;return (klj_val){.l=array};
}

static klj_val create_player(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    // BigMediaPlayer's constructor only allocates an ID and initial state.
    // ExoPlayer and its video surface are created later when a source opens.
    bigscreen_player *player = calloc(1, sizeof *player);
    if (!player) return (klj_val){.j = (uint64_t)-1};
    player->maximum_bitrate = player->maximum_height = -1;
    player->volume=1;
    pthread_mutex_lock(&callback_lock);
    player->id = next_player_id++;
    player->next = players;
    players = player;
    pthread_mutex_unlock(&callback_lock);
    KLJ_LOG("Bigscreen created idle media player %d (no source)", player->id);
    return (klj_val){.j = (uint32_t)player->id};
}

static klj_val set_context(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    void *context = args[0].l;
    if (context) kl_jni_pin_object(context);
    pthread_mutex_lock(&callback_lock);
    void *previous = media_context;
    media_context = context;
    pthread_mutex_unlock(&callback_lock);
    if (previous) kl_jni_unpin_object(previous);
    KLJ_LOG("Bigscreen media context retained for system stream playback");
    return (klj_val){0};
}

static klj_val set_web_context(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    void *context = args[0].l;
    if (context) kl_jni_pin_object(context);
    pthread_mutex_lock(&callback_lock);
    void *previous = web_context;
    web_context = context;
    pthread_mutex_unlock(&callback_lock);
    if (previous) kl_jni_unpin_object(previous);
    return (klj_val){0};
}

static klj_val set_debug(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    atomic_store(&media_debug, !!args[0].j);
    return (klj_val){0};
}

static klj_val drm_info(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    // The Java implementation returns this string when MediaDrm is unavailable.
    KLJ_LOG("Bigscreen DRM info: Android Widevine is not available");
    return (klj_val){.l = kl_jni_new_string("Not available.")};
}

static klj_val register_callback(int index, const klj_val *args) {
    const char *object = klj_str(args[0].l), *method = klj_str(args[1].l);
    char *object_copy = object ? strdup(object) : NULL;
    char *method_copy = method ? strdup(method) : NULL;
    pthread_mutex_lock(&callback_lock);
    free(media_callbacks[index].object);
    free(media_callbacks[index].method);
    media_callbacks[index].object = object_copy;
    media_callbacks[index].method = method_copy;
    pthread_mutex_unlock(&callback_lock);
    KLJ_LOG("Bigscreen registered media event %d", index);
    return (klj_val){0};
}

#define CALLBACK(name, index) \
static klj_val register_##name(void *env, void *self, const klj_val *args, int count) { \
    (void)env; (void)self; (void)count; return register_callback(index, args); \
}
CALLBACK(AudioTracksChanged, 0)
CALLBACK(DRMLicenseError, 1)
CALLBACK(ExpiredDRMLicense, 2)
CALLBACK(MediaInfoFailed, 3)
CALLBACK(MediaInfoSuccess, 4)
CALLBACK(PlaybackBuffering, 5)
CALLBACK(PlaybackEnded, 6)
CALLBACK(PlaybackError, 7)
CALLBACK(PlaybackReady, 8)
CALLBACK(Playing, 9)
CALLBACK(SubtitleCue, 10)
CALLBACK(TextTracksChanged, 11)
#undef CALLBACK

static void send_media_event(bigscreen_player *p,unsigned generation,int index,const char *payload) {
    pthread_mutex_lock(&callback_lock);
    if(p->destroyed||p->generation!=generation){pthread_mutex_unlock(&callback_lock);return;}
    char *target=media_callbacks[index].object?strdup(media_callbacks[index].object):NULL;
    char *method=media_callbacks[index].method?strdup(media_callbacks[index].method):NULL;
    pthread_mutex_unlock(&callback_lock);
    if(target&&method){
        void (*send)(void*,void*,void*,void*,void*)=kl_jni_native("com/unity3d/player/UnityPlayer","nativeUnitySendMessage","(Ljava/lang/String;Ljava/lang/String;[B)V");
        if(send){
            kl_jni_local_frame_push();size_t len=strlen(payload);void *bytes=klj_new_array('B',NULL,(int)len);
            memcpy(klj_arr(bytes)->data,payload,len);
            send(kl_jni_env(),kl_jni_class("com/unity3d/player/UnityPlayer"),kl_jni_new_string(target),kl_jni_new_string(method),bytes);
            kl_jni_local_frame_pop();
        }
    }
    free(target);free(method);
}
void klj_bigscreen_drain_events(void) {
    // Detach the event snapshot before Unity callbacks, which may reenter JNI.
    pthread_mutex_lock(&callback_lock);
    bigscreen_player *next=players;
    pthread_mutex_unlock(&callback_lock);
    while(next){
        pthread_mutex_lock(&callback_lock);bigscreen_player *p=next;next=p->next;
        kl_stream_state s;kl_stream_snapshot(p->destroyed?NULL:p->host,&s,1);
        int drm=p->pending_drm;unsigned generation=p->generation;p->pending_drm=0;pthread_mutex_unlock(&callback_lock);
        if(drm){send_media_event(p,generation,1,"6001");continue;}
        if(s.events&KL_STREAM_ERROR){
            char json[512];snprintf(json,sizeof json,"{\"type\":\"source\",\"errorCode\":%d,\"message\":\"%s\"}",s.error_code,s.error);
            send_media_event(p,generation,7,json);continue;
        }
        if(s.events&KL_STREAM_BUFFERING)send_media_event(p,generation,5,"");
        if(s.events&KL_STREAM_READY)send_media_event(p,generation,8,"");
        if(s.events&KL_STREAM_PLAYING)send_media_event(p,generation,9,"");
        if(s.events&KL_STREAM_ENDED)send_media_event(p,generation,6,"");
    }
}

const klj_binding klj_bind_bigscreen[] = {
    {"com/bigscreenvr/webview/BigWebView", "CreateBigWebClient", "()I", create_web_client},
    {"com/bigscreenvr/webview/BigWebView", "Render", "()V", render_web_clients},
    {"com/bigscreenvr/webview/BigWebView", "GetSharedTextureHandle", "()I", get_web_texture},
    {"com/bigscreenvr/webview/BigWebView", "IsFullScreen", "(I)Z", web_fullscreen},
    {"com/bigscreenvr/webview/BigWebView", "ExitFullScreen", "(I)V", web_exit_fullscreen},
    {"com/bigscreenvr/webview/BigWebView", "SetupClient", "(ILandroid/webkit/WebView;II)V", setup_web_client},
    {"com/bigscreenvr/webview/BigWebView", "PointerDown", "(IIIIII)V", web_pointer_down},
    {"com/bigscreenvr/webview/BigWebView", "PointerUp", "(IIIIII)V", web_pointer_up},
    {"com/bigscreenvr/webview/BigWebView", "PointerMove", "(IIII)V", web_pointer_move},
    {"com/bigscreenvr/webview/BigWebView", "PointerDrag", "(IIIII)V", web_pointer_move},
    {"com/bigscreenvr/webview/BigWebView", "Scroll", "(IIIIII)V", web_scroll},
    {"com/bigscreenvr/webview/BigWebView", "CancelInput", "(III)V", web_cancel},
    {"com/bigscreenvr/webview/BigWebView", "SetContext", "(Landroid/content/Context;)V", set_web_context},
    {"com/bigscreenvr/webview/BigWebView", "SetContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", set_web_context},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "CreateBigMediaPlayer", "()I", create_player},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "InitiateMediaPlayer", "(I)V", initiate_player},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "Update", "()V", update_players},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetCurrentTime", "(IF)V", seek_idle_player},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "Pause", "(I)V", player_pause},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "Play", "(I)V", player_play},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "Resume", "(I)V", player_play},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "Stop", "(I)V", player_stop},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "Shutdown", "(I)V", player_shutdown},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "StopAndReleaseAll", "()V", stop_all},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetVolume", "(IF)V", player_volume},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetVolume", "(I)F", player_get_volume},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetAutoplay", "(IZ)V", player_autoplay},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetMaximumVideoBitrate", "(II)V", player_bitrate},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetMaximumVideoHeight", "(II)V", player_height},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetDuration", "(I)J", player_duration},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetCurrentPosition", "(I)J", player_position},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "IsFrameAvailable", "(I)Z", frame_available},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetVideoSize", "(I)[I", video_size},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetVideoTextureId", "(I)I", player_texture},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetVideoTextureWidth", "(I)I", player_width},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetVideoTextureHeight", "(I)I", player_texture_height},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetTransformMatrix", "(I)[F", transform},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "StartStream", "(ILjava/lang/String;)V", start_stream},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "StartStream", "(ILjava/lang/String;Ljava/lang/String;)V", start_stream},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "StartStream", "(ILjava/lang/String;Ljava/lang/String;Landroid/view/Surface;)V", start_stream},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "StartProtectedMediaSession", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", protected_stream},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "StartProtectedMediaSession", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/view/Surface;)V", protected_stream},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetMediaPlayerContext", "(Landroid/content/Context;)V", set_context},
    // Unity reflection describes its concrete Activity instead of the declared
    // Context argument. That known subclass is assignable to the same method.
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetMediaPlayerContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", set_context},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "SetDebugMode", "(Z)V", set_debug},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI", "GetDRMInfo", "()Ljava/lang/String;", drm_info},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterAudioTracksChangedCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_AudioTracksChanged},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterDRMLicenseErrorCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_DRMLicenseError},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterExpiredDRMLicenseCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_ExpiredDRMLicense},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterMediaInfoFailedCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_MediaInfoFailed},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterMediaInfoSuccessCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_MediaInfoSuccess},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterPlaybackBufferingCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_PlaybackBuffering},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterPlaybackEndedCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_PlaybackEnded},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterPlaybackErrorCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_PlaybackError},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterPlaybackReadyCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_PlaybackReady},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterPlayingCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_Playing},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterSubtitleCueCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_SubtitleCue},
    {"com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks", "RegisterTextTracksChangedCallback", "(Ljava/lang/String;Ljava/lang/String;)V", register_TextTracksChanged},
    {NULL, NULL, NULL, NULL},
};
