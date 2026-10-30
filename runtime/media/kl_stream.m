#import <AVFoundation/AVFoundation.h>
#include <pthread.h>
#include <math.h>
#include "kl_stream.h"
#if __has_feature(objc_arc)
#define RELEASE(x) ((void)0)
#define HANDLE(x) ((__bridge_retained void *)(x))
#define OBJECT(x) ((__bridge KLStream *)(x))
#else
#define RELEASE(x) [(x) release]
#define HANDLE(x) ((void *)(x))
#define OBJECT(x) ((KLStream *)(x))
#endif
@interface KLStream : NSObject {
@public
    pthread_mutex_t lock;
    kl_stream_state state;
    CVPixelBufferRef frame;
    BOOL closed,wantsPlay,wasReady,wasPlaying,wasBuffering,failed;
    BOOL looping;
    float playbackRate;
}
@property(nonatomic,strong) AVPlayer *player;
@property(nonatomic,strong) AVPlayerItemVideoOutput *output;
@property(nonatomic,strong) NSTimer *timer;
@property(nonatomic,strong) id endObserver;
- (void)reset;
- (void)tick;
- (void)fail:(int)code message:(const char *)message;
@end
static int64_t milliseconds(CMTime value) {
    double s=CMTimeGetSeconds(value);
    return isfinite(s)&&s>=0&&s<(double)INT64_MAX/1000.0?(int64_t)(s*1000):0;
}
@implementation KLStream
- (instancetype)init {
    if((self=[super init])){pthread_mutex_init(&lock,NULL);playbackRate=1;}
    return self;
}
- (void)dealloc {
    if(frame)CVPixelBufferRelease(frame);
    pthread_mutex_destroy(&lock);
#if !__has_feature(objc_arc)
    [_player release];[_output release];[_timer release];[_endObserver release];[super dealloc];
#endif
}
- (void)reset {
    [self.player pause];[self.player replaceCurrentItemWithPlayerItem:nil];
    self.output=nil;
    if(self.endObserver)[NSNotificationCenter.defaultCenter removeObserver:self.endObserver];
    self.endObserver=nil;wasReady=wasPlaying=wasBuffering=failed=NO;
    pthread_mutex_lock(&lock);
    uint64_t serial=state.serial+1;state=(kl_stream_state){.serial=serial};
    if(frame){CVPixelBufferRelease(frame);frame=NULL;}
    pthread_mutex_unlock(&lock);
}
- (void)fail:(int)code message:(const char *)message {
    if(failed)return;failed=YES;[self.player pause];
    pthread_mutex_lock(&lock);state.error_code=code;state.events|=KL_STREAM_ERROR;state.playing=0;
    snprintf(state.error,sizeof state.error,"%s",message);pthread_mutex_unlock(&lock);
    fprintf(stderr,"[stream] playback failed, category %d (details contain no stream URL)\n",code);
}
- (void)tick {
    if(closed||failed)return;
    AVPlayerItem *item=self.player.currentItem;if(!item)return;
    if(item.status==AVPlayerItemStatusFailed){
        NSError *error=item.error;
        // Native messages can contain signed URLs. Keep only numeric diagnosis.
        fprintf(stderr,"[stream] AVPlayer failure code %ld, domain %s\n",(long)error.code,
                [error.domain isEqualToString:NSURLErrorDomain]?"URL":
                [error.domain isEqualToString:AVFoundationErrorDomain]?"AVFoundation":"other");
        [self fail:2001 message:"The system player could not open this stream. See the runtime media log."];return;
    }
    BOOL ready=item.status==AVPlayerItemStatusReadyToPlay;
    BOOL playing=ready&&self.player.timeControlStatus==AVPlayerTimeControlStatusPlaying;
    BOOL buffering=self.player.timeControlStatus==AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate;
    CGSize size=item.presentationSize;
    pthread_mutex_lock(&lock);
    state.position_ms=milliseconds(self.player.currentTime);state.duration_ms=milliseconds(item.duration);
    if(size.width>0&&size.height>0){state.width=(int)size.width;state.height=(int)size.height;}
    state.ready=ready;state.playing=playing;
    state.buffering=buffering;
    state.seek_start=state.seek_end=state.buffered_fraction=0;
    if(item.seekableTimeRanges.count){
        CMTimeRange range=item.seekableTimeRanges.lastObject.CMTimeRangeValue;
        state.seek_start=milliseconds(range.start);
        state.seek_end=milliseconds(CMTimeRangeGetEnd(range));
    }
    if(state.duration_ms>0&&item.loadedTimeRanges.count){
        CMTimeRange range=item.loadedTimeRanges.lastObject.CMTimeRangeValue;
        state.buffered_fraction=fminf(1,(float)milliseconds(CMTimeRangeGetEnd(range))/state.duration_ms);
    }
    if(ready&&!wasReady){
        for(AVPlayerItemTrack *track in item.tracks){
            AVAssetTrack *assetTrack=track.assetTrack;
            if([assetTrack.mediaType isEqualToString:AVMediaTypeAudio])state.audio_tracks++;
            if([assetTrack.mediaType isEqualToString:AVMediaTypeVideo]){state.has_video=1;state.source_fps=assetTrack.nominalFrameRate;}
        }
        state.has_audio=state.audio_tracks>0;
    }
    if(ready&&!wasReady)state.events|=KL_STREAM_READY;
    if(playing&&!wasPlaying)state.events|=KL_STREAM_PLAYING;
    if(buffering&&!wasBuffering)state.events|=KL_STREAM_BUFFERING;
    pthread_mutex_unlock(&lock);
    wasReady=ready;wasPlaying=playing;wasBuffering=buffering;
    if(!ready)return;
    CMTime time=self.player.currentTime;
    if([self.output hasNewPixelBufferForItemTime:time]) {
        CVPixelBufferRef pixel=[self.output copyPixelBufferForItemTime:time itemTimeForDisplay:NULL];
        if(pixel){
            pthread_mutex_lock(&lock);if(frame)CVPixelBufferRelease(frame);frame=pixel;
            state.serial++;state.frame_count++;state.has_frame=state.has_video=1;state.width=(int)CVPixelBufferGetWidth(pixel);state.height=(int)CVPixelBufferGetHeight(pixel);
            pthread_mutex_unlock(&lock);
        }
    }
}
@end
void *kl_stream_new(void) {
    @autoreleasepool {
        KLStream *s=[[KLStream alloc]init];
        dispatch_async(dispatch_get_main_queue(),^{
            if(s->closed)return;
            AVPlayer *player=[[AVPlayer alloc]init];s.player=player;RELEASE(player);
            s.timer=[NSTimer scheduledTimerWithTimeInterval:1.0/30 repeats:YES block:^(NSTimer *timer){[s tick];}];
        });return HANDLE(s);
    }
}
void kl_stream_close(void *stream) {
    if(!stream)return;
    KLStream *s=OBJECT(stream);
    dispatch_async(dispatch_get_main_queue(),^{
        s->closed=YES;[s.timer invalidate];s.timer=nil;[s reset];s.player=nil;
#if __has_feature(objc_arc)
        CFRelease(stream);
#else
        [s release];
#endif
    });
}
void kl_stream_open(void *stream,const char *url,const char *mime) {
    if(!stream)return;
    @autoreleasepool {
        KLStream *s=OBJECT(stream);NSString *text=url?[NSString stringWithUTF8String:url]:@"";
        NSString *type=mime?[NSString stringWithUTF8String:mime]:@"";
        dispatch_async(dispatch_get_main_queue(),^{
            if(s->closed)return;[s reset];
            NSURL *source=[text hasPrefix:@"/"]?[NSURL fileURLWithPath:text]:[NSURL URLWithString:text];
            if(!source||![@[@"http",@"https",@"file"] containsObject:source.scheme.lowercaseString]){
                [s fail:1004 message:"The stream address is missing or unsupported."];return;
            }
            if([type.lowercaseString containsString:@"dash"]||[source.pathExtension.lowercaseString isEqualToString:@"mpd"]){
                [s fail:4001 message:"This MPEG-DASH stream is not supported by the system player."];return;
            }
            AVURLAsset *asset=[AVURLAsset URLAssetWithURL:source options:nil];
            AVPlayerItem *item=[AVPlayerItem playerItemWithAsset:asset];
            AVPlayerItemVideoOutput *output=[[AVPlayerItemVideoOutput alloc]initWithPixelBufferAttributes:@{
                (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferIOSurfacePropertiesKey:@{}}];
            output.suppressesPlayerRendering=YES;[item addOutput:output];s.output=output;RELEASE(output);
            s.endObserver=[NSNotificationCenter.defaultCenter addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:item queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){
                if(s->looping){
                    [s.player seekToTime:kCMTimeZero completionHandler:^(BOOL done){if(done&&!s->closed&&s.player.currentItem==item&&s->wantsPlay)[s.player playImmediatelyAtRate:s->playbackRate];}];
                }else{
                    pthread_mutex_lock(&s->lock);s->state.events|=KL_STREAM_ENDED;s->state.ended=1;s->state.playing=0;pthread_mutex_unlock(&s->lock);
                }
            }];
            [s.player replaceCurrentItemWithPlayerItem:item];
            if(s->wantsPlay)[s.player playImmediatelyAtRate:s->playbackRate];
        });
    }
}
void kl_stream_play(void *stream,int playing) {
    if(!stream)return;KLStream *s=OBJECT(stream);
    dispatch_async(dispatch_get_main_queue(),^{if(s->closed)return;s->wantsPlay=!!playing;if(playing)[s.player playImmediatelyAtRate:s->playbackRate];else[s.player pause];});
}
void kl_stream_reject(void *stream,int code,const char *message) {
    if(!stream)return;
    @autoreleasepool {
        KLStream *s=OBJECT(stream);NSString *text=[NSString stringWithUTF8String:message];
        dispatch_async(dispatch_get_main_queue(),^{if(!s->closed){[s reset];[s fail:code message:text.UTF8String];}});
    }
}
void kl_stream_stop(void *stream) {
    if(!stream)return;KLStream *s=OBJECT(stream);dispatch_async(dispatch_get_main_queue(),^{if(!s->closed){s->wantsPlay=NO;[s reset];}});
}
void kl_stream_seek(void *stream,int64_t ms) {
    if(!stream)return;KLStream *s=OBJECT(stream);
    dispatch_async(dispatch_get_main_queue(),^{if(!s->closed){
        AVPlayerItem *item=s.player.currentItem;
        pthread_mutex_lock(&s->lock);s->state.seeking=1;s->state.ended=0;pthread_mutex_unlock(&s->lock);
        [s.player seekToTime:CMTimeMake(ms<0?0:ms,1000) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero completionHandler:^(BOOL done){
            if(s.player.currentItem==item){pthread_mutex_lock(&s->lock);s->state.seeking=0;pthread_mutex_unlock(&s->lock);}
        }];
    }});
}
void kl_stream_loop(void *stream,int looping) {
    if(!stream)return;KLStream *s=OBJECT(stream);dispatch_async(dispatch_get_main_queue(),^{if(!s->closed)s->looping=!!looping;});
}
void kl_stream_rate(void *stream,float rate) {
    if(!stream||!isfinite(rate)||rate<0)return;KLStream *s=OBJECT(stream);
    dispatch_async(dispatch_get_main_queue(),^{if(!s->closed){s->playbackRate=rate;if(s->wantsPlay)[s.player playImmediatelyAtRate:rate];}});
}
void kl_stream_volume(void *stream,float volume) {
    if(!stream)return;KLStream *s=OBJECT(stream);
    dispatch_async(dispatch_get_main_queue(),^{if(!s->closed)s.player.volume=fmaxf(0,fminf(1,volume));});
}
void kl_stream_limits(void *stream,int bitrate,int height) {
    if(!stream)return;KLStream *s=OBJECT(stream);
    dispatch_async(dispatch_get_main_queue(),^{if(!s->closed){s.player.currentItem.preferredPeakBitRate=bitrate>0?bitrate:0;s.player.currentItem.preferredMaximumResolution=height>0?CGSizeMake(CGFLOAT_MAX,height):CGSizeZero;}});
}
void kl_stream_snapshot(void *stream,kl_stream_state *out,int consume) {
    *out=(kl_stream_state){0};if(!stream)return;KLStream *s=OBJECT(stream);
    pthread_mutex_lock(&s->lock);*out=s->state;if(consume)s->state.events=0;pthread_mutex_unlock(&s->lock);
}
void *kl_stream_copy_frame(void *stream,uint64_t *serial) {
    if(!stream)return NULL;KLStream *s=OBJECT(stream);
    pthread_mutex_lock(&s->lock);CVPixelBufferRef pixel=s->frame?CVPixelBufferRetain(s->frame):NULL;
    if(serial)*serial=s->state.serial;pthread_mutex_unlock(&s->lock);return pixel;
}
