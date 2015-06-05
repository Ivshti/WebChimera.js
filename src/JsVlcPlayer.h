#pragma once

#include <memory>
#include <deque>

#include <v8.h>
#include <node.h>
#include <node_object_wrap.h>
#include <uv.h>

#include <libvlc_wrapper/vlc_player.h>
#include <libvlc_wrapper/vlc_vmem.h>

class JsVlcPlayer :
    public node::ObjectWrap,
    private vlc::basic_vmem_wrapper,
    private vlc::media_player_events_callback
{
    enum Callbacks_e {
        CB_FrameSetup = 0,
        CB_FrameReady,
        CB_FrameCleanup,

        CB_MediaPlayerMediaChanged,
        CB_MediaPlayerNothingSpecial,
        CB_MediaPlayerOpening,
        CB_MediaPlayerBuffering,
        CB_MediaPlayerPlaying,
        CB_MediaPlayerPaused,
        CB_MediaPlayerStopped,
        CB_MediaPlayerForward,
        CB_MediaPlayerBackward,
        CB_MediaPlayerEndReached,
        CB_MediaPlayerEncounteredError,

        CB_MediaPlayerTimeChanged,
        CB_MediaPlayerPositionChanged,
        CB_MediaPlayerSeekableChanged,
        CB_MediaPlayerPausableChanged,
        CB_MediaPlayerLengthChanged,

        CB_Max,
    };

    enum class PixelFormat {
        RV32 = 0,
        I420,
    };

public:
    static void initJsApi();
    static void jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args );

    static void jsPixelFormat( v8::Local<v8::String> property,
                               const v8::PropertyCallbackInfo<v8::Value>& info );
    static void jsSetPixelFormat( v8::Local<v8::String> property,
                                  v8::Local<v8::Value> value,
                                  const v8::PropertyCallbackInfo<void>& info );

    static void jsPlaying( v8::Local<v8::String> property,
                           const v8::PropertyCallbackInfo<v8::Value>& info );
    static void jsLength( v8::Local<v8::String> property,
                          const v8::PropertyCallbackInfo<v8::Value>& info );
    static void jsState( v8::Local<v8::String> property,
                         const v8::PropertyCallbackInfo<v8::Value>& info );

    static void jsPosition( v8::Local<v8::String> property,
                            const v8::PropertyCallbackInfo<v8::Value>& info );
    static void jsSetPosition( v8::Local<v8::String> property,
                               v8::Local<v8::Value> value,
                               const v8::PropertyCallbackInfo<void>& info );

    static void jsTime( v8::Local<v8::String> property,
                        const v8::PropertyCallbackInfo<v8::Value>& info );
    static void jsSetTime( v8::Local<v8::String> property,
                           v8::Local<v8::Value> value,
                           const v8::PropertyCallbackInfo<void>& info );

    static void jsVolume( v8::Local<v8::String> property,
                          const v8::PropertyCallbackInfo<v8::Value>& info );
    static void jsSetVolume( v8::Local<v8::String> property,
                             v8::Local<v8::Value> value,
                             const v8::PropertyCallbackInfo<void>& info );

    static void jsPlay( const v8::FunctionCallbackInfo<v8::Value>& args );
    static void jsPause( const v8::FunctionCallbackInfo<v8::Value>& args );
    static void jsTogglePause( const v8::FunctionCallbackInfo<v8::Value>& args );
    static void jsStop( const v8::FunctionCallbackInfo<v8::Value>& args );
    static void jsToggleMute( const v8::FunctionCallbackInfo<v8::Value>& args );

    static void getJsCallback( v8::Local<v8::String> property,
                               const v8::PropertyCallbackInfo<v8::Value>& info,
                               Callbacks_e callback );
    static void setJsCallback( v8::Local<v8::String> property,
                               v8::Local<v8::Value> value,
                               const v8::PropertyCallbackInfo<void>& info,
                               Callbacks_e callback );

private:
    JsVlcPlayer();
    ~JsVlcPlayer();

    struct AsyncData;
    struct RV32FrameSetupData;
    struct I420FrameSetupData;
    struct FrameUpdated;
    struct CallbackData;
    struct LibvlcEvent;

    class VideoFrame;
    class RV32VideoFrame;
    class I420VideoFrame;

    void handleAsync();
    void setupBuffer( const RV32FrameSetupData& );
    void setupBuffer( const I420FrameSetupData& );
    void frameUpdated();

    void media_player_event( const libvlc_event_t* e );

    void callCallback( Callbacks_e callback,
                       std::initializer_list<v8::Local<v8::Value> > list = std::initializer_list<v8::Local<v8::Value> >() );

private:
    unsigned video_format_cb( char* chroma,
                              unsigned* width, unsigned* height,
                              unsigned* pitches, unsigned* lines ) override;
    void video_cleanup_cb() override;

    void* video_lock_cb( void** planes ) override;
    void video_unlock_cb( void* picture, void *const * planes ) override;
    void video_display_cb( void* picture ) override;

private:
    static v8::Persistent<v8::Function> _jsConstructor;
    static std::vector<std::string> _libvlcArgs;

    libvlc_instance_t* _libvlc;
    vlc::player _player;

    uv_async_t _async;
    std::mutex _asyncDataGuard;
    std::deque<std::unique_ptr<AsyncData> > _asyncData;

    PixelFormat _pixelFormat;
    std::unique_ptr<VideoFrame> _videoFrame;

    v8::Persistent<v8::Value> _jsFrameBuffer;
    char* _jsRawFrameBuffer;

    v8::Persistent<v8::Function> _jsCallbacks[CB_Max];
};
