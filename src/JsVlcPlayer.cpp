#include "JsVlcPlayer.h"

#include <string.h>

v8::Persistent<v8::Function> JsVlcPlayer::_jsConstructor;

///////////////////////////////////////////////////////////////////////////////
struct JsVlcPlayer::AsyncData
{
    virtual void process( JsVlcPlayer* ) = 0;
};

///////////////////////////////////////////////////////////////////////////////
struct JsVlcPlayer::RV32FrameSetupData : public JsVlcPlayer::AsyncData
{
    RV32FrameSetupData( unsigned width, unsigned height, unsigned size ) :
        width( width ), height( height ), size( size ) {}

    void process( JsVlcPlayer* ) override;

    const unsigned width;
    const unsigned height;
    const unsigned size;
};

void JsVlcPlayer::RV32FrameSetupData::process( JsVlcPlayer* jsPlayer )
{
    jsPlayer->setupBuffer( *this );
}

///////////////////////////////////////////////////////////////////////////////
struct JsVlcPlayer::I420FrameSetupData : public JsVlcPlayer::AsyncData
{
    I420FrameSetupData( unsigned width, unsigned height,
                        unsigned uPlaneOffset, unsigned vPlaneOffset,
                        unsigned size ) :
        width( width ), height( height ),
        uPlaneOffset( uPlaneOffset ), vPlaneOffset( vPlaneOffset ),
        size( size ) {}

    void process( JsVlcPlayer* ) override;

    const unsigned width;
    const unsigned height;
    const unsigned uPlaneOffset;
    const unsigned vPlaneOffset;
    const unsigned size;
};

void JsVlcPlayer::I420FrameSetupData::process( JsVlcPlayer* jsPlayer )
{
    jsPlayer->setupBuffer( *this );
}

///////////////////////////////////////////////////////////////////////////////
struct JsVlcPlayer::FrameUpdated : public JsVlcPlayer::AsyncData
{
    void process( JsVlcPlayer* ) override;
};

void JsVlcPlayer::FrameUpdated::process( JsVlcPlayer* jsPlayer )
{
    jsPlayer->frameUpdated();
}

///////////////////////////////////////////////////////////////////////////////
struct JsVlcPlayer::CallbackData : public JsVlcPlayer::AsyncData
{
    CallbackData( JsVlcPlayer::Callbacks_e callback ) :
        callback( callback ) {}

    void process( JsVlcPlayer* );

    const JsVlcPlayer::Callbacks_e callback;
};

void JsVlcPlayer::CallbackData::process( JsVlcPlayer* jsPlayer )
{
    jsPlayer->callCallback( callback );
}

///////////////////////////////////////////////////////////////////////////////
struct JsVlcPlayer::LibvlcEvent : public JsVlcPlayer::AsyncData
{
    LibvlcEvent( const libvlc_event_t& libvlcEvent ) :
        libvlcEvent( libvlcEvent ) {}

    void process( JsVlcPlayer* );

    const libvlc_event_t libvlcEvent;
};

void JsVlcPlayer::LibvlcEvent::process( JsVlcPlayer* jsPlayer )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Callbacks_e callback = CB_Max;

    std::initializer_list<v8::Local<v8::Value> > list;

    switch( libvlcEvent.type ) {
        case libvlc_MediaPlayerMediaChanged:
            callback = CB_MediaPlayerMediaChanged;
            break;
        case libvlc_MediaPlayerNothingSpecial:
            callback = CB_MediaPlayerNothingSpecial;
            break;
        case libvlc_MediaPlayerOpening:
            callback = CB_MediaPlayerOpening;
            break;
        case libvlc_MediaPlayerBuffering: {
            callback = CB_MediaPlayerBuffering;
            list = { Number::New( isolate, libvlcEvent.u.media_player_buffering.new_cache ) };
            break;
        }
        case libvlc_MediaPlayerPlaying:
            callback = CB_MediaPlayerPlaying;
            break;
        case libvlc_MediaPlayerPaused:
            callback = CB_MediaPlayerPaused;
            break;
        case libvlc_MediaPlayerStopped:
            callback = CB_MediaPlayerStopped;
            break;
        case libvlc_MediaPlayerForward:
            callback = CB_MediaPlayerForward;
            break;
        case libvlc_MediaPlayerBackward:
            callback = CB_MediaPlayerBackward;
            break;
        case libvlc_MediaPlayerEndReached:
            callback = CB_MediaPlayerEndReached;
            break;
        case libvlc_MediaPlayerEncounteredError:
            callback = CB_MediaPlayerEncounteredError;
            break;
        case libvlc_MediaPlayerTimeChanged: {
            callback = CB_MediaPlayerTimeChanged;
            const double new_time =
                static_cast<double>( libvlcEvent.u.media_player_time_changed.new_time );
            list = { Number::New( isolate, static_cast<double>( new_time ) ) };
            break;
        }
        case libvlc_MediaPlayerPositionChanged: {
            callback = CB_MediaPlayerPositionChanged;
            list = { Number::New( isolate, libvlcEvent.u.media_player_position_changed.new_position ) };
            break;
        }
        case libvlc_MediaPlayerSeekableChanged: {
            callback = CB_MediaPlayerSeekableChanged;
            list = { Boolean::New( isolate, libvlcEvent.u.media_player_seekable_changed.new_seekable != 0 ) };
            break;
        }
        case libvlc_MediaPlayerPausableChanged: {
            callback = CB_MediaPlayerPausableChanged;
            list = { Boolean::New( isolate, libvlcEvent.u.media_player_pausable_changed.new_pausable != 0 ) };
            break;
        }
        case libvlc_MediaPlayerLengthChanged: {
            callback = CB_MediaPlayerLengthChanged;
           const double new_length =
               static_cast<double>( libvlcEvent.u.media_player_length_changed.new_length );
            list = { Number::New( isolate, new_length ) };
            break;
        }
    }

    if( callback != CB_Max ) {
        jsPlayer->callCallback( callback, list );
    }
}

///////////////////////////////////////////////////////////////////////////////
JsVlcPlayer::JsVlcPlayer() :
    _libvlc( nullptr ), _jsRawFrameBuffer( nullptr )
{
    _libvlc = libvlc_new( 0, nullptr );
    assert( _libvlc );
    if( _player.open( _libvlc ) ) {
        _player.register_callback( this );
        vlc::basic_vmem_wrapper::open( &_player.basic_player() );
    } else {
        assert( false );
    }

    uv_loop_t* loop = uv_default_loop();

    uv_async_init( loop, &_async,
        [] ( uv_async_t* handle ) {
            if( handle->data )
                reinterpret_cast<JsVlcPlayer*>( handle->data )->handleAsync();
        }
    );
    _async.data = this;
}

JsVlcPlayer::~JsVlcPlayer()
{
    _player.unregister_callback( this );
    vlc::basic_vmem_wrapper::close();

    _async.data = nullptr;
    uv_close( reinterpret_cast<uv_handle_t*>( &_async ), 0 );
}

unsigned JsVlcPlayer::video_format_cb( char* chroma,
                                       unsigned* width, unsigned* height,
                                       unsigned* pitches, unsigned* lines )
{
    const char CHROMA[] = "I420";

    memcpy( chroma, CHROMA, sizeof( CHROMA ) - 1 );

    const unsigned evenWidth = *width + ( *width & 1 );
    const unsigned evenHeight = *height + ( *height & 1 );

    pitches[0] = evenWidth; if( pitches[0] % 4 ) pitches[0] += 4 - pitches[0] % 4;
    pitches[1] = evenWidth / 2; if( pitches[1] % 4 ) pitches[1] += 4 - pitches[1] % 4;
    pitches[2] = pitches[1];

    assert( 0 == pitches[0] % 4 && 0 == pitches[1] % 4 && 0 == pitches[2] % 4 );

    lines[0] = evenHeight;
    lines[1] = evenHeight / 2;
    lines[2] = lines[1];

    _uPlaneOffset = pitches[0] * lines[0];
    _vPlaneOffset = _uPlaneOffset + pitches[1] * lines[1];

    _tmpFrameBuffer.resize( pitches[0] * lines[0] +
                            pitches[1] * lines[1] +
                            pitches[2] * lines[2] );

    _asyncDataGuard.lock();
    _asyncData.push_back(
        std::make_shared<I420FrameSetupData>( *width, *height,
                                              _uPlaneOffset,
                                              _vPlaneOffset,
                                              _tmpFrameBuffer.size() ) );
    _asyncDataGuard.unlock();
    uv_async_send( &_async );

    return 1;
}

void JsVlcPlayer::video_cleanup_cb()
{
    if( !_tmpFrameBuffer.empty() )
        std::vector<char>().swap( _tmpFrameBuffer );

    _asyncDataGuard.lock();
    _asyncData.push_back( std::make_shared<CallbackData>( CB_FrameCleanup ) );
    _asyncDataGuard.unlock();
    uv_async_send( &_async );
}

void* JsVlcPlayer::video_lock_cb( void** planes )
{
    char* buffer;
    if( _tmpFrameBuffer.empty() ) {
        buffer = _jsRawFrameBuffer;
    } else {
        if( _jsRawFrameBuffer ) {
            std::vector<char>().swap( _tmpFrameBuffer );
            buffer = _jsRawFrameBuffer;
        } else {
            buffer = _tmpFrameBuffer.data();
        }
    }

    planes[0] = buffer;
    planes[1] = buffer + _uPlaneOffset;
    planes[2] = buffer + _vPlaneOffset;

    return nullptr;
}

void JsVlcPlayer::video_unlock_cb( void* /*picture*/, void *const * /*planes*/ )
{
}

void JsVlcPlayer::video_display_cb( void* /*picture*/ )
{
    _asyncDataGuard.lock();
    _asyncData.push_back( std::make_shared<FrameUpdated>() );
    _asyncDataGuard.unlock();
    uv_async_send( &_async );
}

void JsVlcPlayer::media_player_event( const libvlc_event_t* e )
{
    _asyncDataGuard.lock();
    _asyncData.push_back( std::make_shared<LibvlcEvent>( *e ) );
    _asyncDataGuard.unlock();
    uv_async_send( &_async );
}

void JsVlcPlayer::handleAsync()
{
    while( !_asyncData.empty() ) {
        std::deque<std::shared_ptr<AsyncData> > tmpData;
        _asyncDataGuard.lock();
        _asyncData.swap( tmpData );
        _asyncDataGuard.unlock();
        for( const auto& i: tmpData ) {
            i->process( this );
        }
    }
}

void JsVlcPlayer::setupBuffer( const RV32FrameSetupData& frameData )
{
    using namespace v8;

    if( 0 == frameData.width || 0 == frameData.height )
        return;

    assert( frameData.size );

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Object> global = isolate->GetCurrentContext()->Global();

    Local<Value> abv =
        global->Get(
            String::NewFromUtf8( isolate,
                                 "Uint8Array",
                                 v8::String::kInternalizedString ) );
    Local<Value> argv[] =
        { Integer::NewFromUnsigned( isolate, frameData.size ) };
    Local<Object> jsArray =
        Handle<Function>::Cast( abv )->NewInstance( 1, argv );

    Local<Integer> jsWidth = Integer::New( isolate, frameData.width );
    Local<Integer> jsHeight = Integer::New( isolate, frameData.height );
    Local<String> jsPixelFormat = String::NewFromUtf8( isolate, vlc::DEF_CHROMA );

    jsArray->Set( String::NewFromUtf8( isolate, "width" ), jsWidth );
    jsArray->Set( String::NewFromUtf8( isolate, "height" ), jsHeight );
    jsArray->Set( String::NewFromUtf8( isolate, "pixelFormat" ), jsPixelFormat );

    _jsFrameBuffer.Reset( isolate, jsArray );

    _jsRawFrameBuffer =
        static_cast<char*>( jsArray->GetIndexedPropertiesExternalArrayData() );

    callCallback( CB_FrameSetup, { jsWidth, jsHeight, jsPixelFormat } );
}

void JsVlcPlayer::setupBuffer( const I420FrameSetupData& frameData )
{
    using namespace v8;

    if( 0 == frameData.width || 0 == frameData.height )
        return;

    assert( frameData.uPlaneOffset && frameData.vPlaneOffset && frameData.size );

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Object> global = isolate->GetCurrentContext()->Global();

    Local<Value> abv =
        global->Get(
            String::NewFromUtf8( isolate,
                                 "Uint8Array",
                                 v8::String::kInternalizedString ) );
    Local<Value> argv[] =
        { Integer::NewFromUnsigned( isolate, frameData.size ) };
    Local<Object> jsArray =
        Handle<Function>::Cast( abv )->NewInstance( 1, argv );

    Local<Integer> jsWidth = Integer::New( isolate, frameData.width );
    Local<Integer> jsHeight = Integer::New( isolate, frameData.height );
    Local<String> jsPixelFormat = String::NewFromUtf8( isolate, "I420" );

    jsArray->Set( String::NewFromUtf8( isolate, "width" ), jsWidth );
    jsArray->Set( String::NewFromUtf8( isolate, "height" ), jsHeight );
    jsArray->Set( String::NewFromUtf8( isolate, "pixelFormat" ), jsPixelFormat );
    jsArray->Set( String::NewFromUtf8( isolate, "uOffset" ),
                  Integer::New( isolate, frameData.uPlaneOffset ) );
    jsArray->Set( String::NewFromUtf8( isolate, "vOffset" ),
                  Integer::New( isolate, frameData.vPlaneOffset ) );

    _jsFrameBuffer.Reset( isolate, jsArray );

    _jsRawFrameBuffer =
        static_cast<char*>( jsArray->GetIndexedPropertiesExternalArrayData() );

    callCallback( CB_FrameSetup, { jsWidth, jsHeight, jsPixelFormat } );
}

void JsVlcPlayer::frameUpdated()
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    callCallback( CB_FrameReady, { Local<Value>::New( Isolate::GetCurrent(), _jsFrameBuffer ) } );
}

#define SET_CALLBACK_PROPERTY( objTemplate, name, callback )                      \
    objTemplate->SetAccessor( String::NewFromUtf8( Isolate::GetCurrent(), name ), \
        [] ( v8::Local<v8::String> property,                                      \
             const v8::PropertyCallbackInfo<v8::Value>& info )                    \
        {                                                                         \
            JsVlcPlayer::getJsCallback( property, info, callback );               \
        },                                                                        \
        [] ( v8::Local<v8::String> property,                                      \
             v8::Local<v8::Value> value,                                          \
             const v8::PropertyCallbackInfo<void>& info )                         \
        {                                                                         \
            JsVlcPlayer::setJsCallback( property, value, info, callback );        \
        } )

void JsVlcPlayer::callCallback( Callbacks_e callback,
                                std::initializer_list<v8::Local<v8::Value> > list )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    if( _jsCallbacks[callback].IsEmpty() )
        return;

    std::vector<v8::Local<v8::Value> > argList = list;

    Local<Function> callbackFunc =
        Local<Function>::New( isolate, _jsCallbacks[callback] );

    callbackFunc->Call( isolate->GetCurrentContext()->Global(),
                        argList.size(), argList.data() );
}

void JsVlcPlayer::initJsApi()
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<FunctionTemplate> ct = FunctionTemplate::New( isolate, jsCreate );
    ct->SetClassName( String::NewFromUtf8( isolate, "VlcPlayer" ) );

    Local<ObjectTemplate> vlcPlayerTemplate = ct->InstanceTemplate();
    vlcPlayerTemplate->SetInternalFieldCount( 1 );

    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "NothingSpecial" ),
                            Number::New( isolate, libvlc_NothingSpecial ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Opening" ),
                            Number::New( isolate, libvlc_Opening ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Buffering" ),
                            Number::New( isolate, libvlc_Buffering ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Playing" ),
                            Number::New( isolate, libvlc_Playing ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Paused" ),
                            Number::New( isolate, libvlc_Paused ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Stopped" ),
                            Number::New( isolate, libvlc_Stopped ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Ended" ),
                            Number::New( isolate, libvlc_Ended ), ReadOnly );
    vlcPlayerTemplate->Set( String::NewFromUtf8( isolate, "Error" ),
                            Number::New( isolate, libvlc_Error ), ReadOnly );

    vlcPlayerTemplate->SetAccessor( String::NewFromUtf8( isolate, "playing" ),
                                    jsPlaying );
    vlcPlayerTemplate->SetAccessor( String::NewFromUtf8( isolate, "length" ),
                                    jsLength );
    vlcPlayerTemplate->SetAccessor( String::NewFromUtf8( isolate, "state" ),
                                    jsState );

    vlcPlayerTemplate->SetAccessor( String::NewFromUtf8( isolate, "position" ),
                                    jsPosition, jsSetPosition );
    vlcPlayerTemplate->SetAccessor( String::NewFromUtf8( isolate, "time" ),
                                    jsTime, jsSetTime );
    vlcPlayerTemplate->SetAccessor( String::NewFromUtf8( isolate, "volume" ),
                                    jsVolume, jsSetVolume );

    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onFrameSetup", CB_FrameSetup );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onFrameReady", CB_FrameReady );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onFrameCleanup", CB_FrameCleanup );

    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onMediaChanged", CB_MediaPlayerMediaChanged );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onNothingSpecial", CB_MediaPlayerNothingSpecial );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onOpening", CB_MediaPlayerOpening );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onBuffering", CB_MediaPlayerBuffering );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onPlaying", CB_MediaPlayerPlaying );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onPaused", CB_MediaPlayerPaused );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onForward", CB_MediaPlayerForward );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onBackward", CB_MediaPlayerBackward );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onEncounteredError", CB_MediaPlayerEncounteredError );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onEndReached", CB_MediaPlayerEndReached );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onStopped", CB_MediaPlayerStopped );

    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onTimeChanged", CB_MediaPlayerTimeChanged );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onPositionChanged", CB_MediaPlayerPositionChanged );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onSeekableChanged", CB_MediaPlayerSeekableChanged );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onPausableChanged", CB_MediaPlayerPausableChanged );
    SET_CALLBACK_PROPERTY( vlcPlayerTemplate, "onLengthChanged", CB_MediaPlayerLengthChanged );

    NODE_SET_PROTOTYPE_METHOD( ct, "play", jsPlay );
    NODE_SET_PROTOTYPE_METHOD( ct, "pause", jsPause );
    NODE_SET_PROTOTYPE_METHOD( ct, "togglePause", jsTogglePause );
    NODE_SET_PROTOTYPE_METHOD( ct, "stop", jsStop );
    NODE_SET_PROTOTYPE_METHOD( ct, "toggleMute", jsToggleMute );

    _jsConstructor.Reset( isolate, ct->GetFunction() );
}

void JsVlcPlayer::jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    if( args.IsConstructCall() ) {
        JsVlcPlayer* jsPlayer = new JsVlcPlayer;
        jsPlayer->Wrap( args.This() );
        args.GetReturnValue().Set( args.This() );
    } else {
        Local<Function> constructor =
            Local<Function>::New( isolate, _jsConstructor );
        args.GetReturnValue().Set( constructor->NewInstance( 0, nullptr ) );
    }
}

void JsVlcPlayer::jsPlaying( v8::Local<v8::String> property,
                             const v8::PropertyCallbackInfo<v8::Value>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    info.GetReturnValue().Set( Boolean::New( isolate, player.is_playing() ) );
}

void JsVlcPlayer::jsLength( v8::Local<v8::String> property,
                            const v8::PropertyCallbackInfo<v8::Value>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    info.GetReturnValue().Set( Number::New( isolate, static_cast<double>( player.get_length() ) ) );
}

void JsVlcPlayer::jsState( v8::Local<v8::String> property,
                           const v8::PropertyCallbackInfo<v8::Value>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    info.GetReturnValue().Set( Number::New( isolate, player.get_state() ) );
}

void JsVlcPlayer::jsPosition( v8::Local<v8::String> property,
                              const v8::PropertyCallbackInfo<v8::Value>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    info.GetReturnValue().Set( Number::New( isolate, player.get_position() ) );
}

void JsVlcPlayer::jsSetPosition( v8::Local<v8::String> property,
                                 v8::Local<v8::Value> value,
                                 const v8::PropertyCallbackInfo<void>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    Local<Number> jsPosition = Local<Number>::Cast( value );
    if( !jsPosition.IsEmpty() )
        player.set_position( static_cast<float>( jsPosition->Value() ) );
}

void JsVlcPlayer::jsTime( v8::Local<v8::String> property,
                          const v8::PropertyCallbackInfo<v8::Value>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    info.GetReturnValue().Set( Number::New( isolate, static_cast<double>( player.get_time() ) ) );
}

void JsVlcPlayer::jsSetTime( v8::Local<v8::String> property,
                             v8::Local<v8::Value> value,
                             const v8::PropertyCallbackInfo<void>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    Local<Number> jsTime = Local<Number>::Cast( value );
    if( !jsTime.IsEmpty() )
        player.set_time( static_cast<libvlc_time_t>( jsTime->Value() ) );
}

void JsVlcPlayer::jsVolume( v8::Local<v8::String> property,
                            const v8::PropertyCallbackInfo<v8::Value>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    info.GetReturnValue().Set( Number::New( isolate, player.audio().get_volume() ) );
}

void JsVlcPlayer::jsSetVolume( v8::Local<v8::String> property,
                               v8::Local<v8::Value> value,
                               const v8::PropertyCallbackInfo<void>& info )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );
    vlc::player& player = jsPlayer->_player;

    Local<Number> jsVolume = Local<Number>::Cast( value );
    if( !jsVolume.IsEmpty() && jsVolume->Value() > 0 )
        player.audio().set_volume( static_cast<unsigned>( jsVolume->Value() ) );
}

void JsVlcPlayer::jsPlay( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    using namespace v8;

    if( args.Length() != 1 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    String::Utf8Value mrl( args[0]->ToString() );
    if( mrl.length() ) {
        player.clear_items();
        const int idx = player.add_media( *mrl );
        if( idx >= 0 ) {
            player.play( idx );
        }
    }
}

void JsVlcPlayer::jsPause( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    if( args.Length() != 0 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    player.pause();
}

void JsVlcPlayer::jsTogglePause( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    if( args.Length() != 0 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    player.togglePause();
}

void JsVlcPlayer::jsStop( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    if( args.Length() != 0 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    player.stop();
}

void JsVlcPlayer::jsToggleMute( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    if( args.Length() != 0 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    player.audio().toggle_mute();
}

void JsVlcPlayer::getJsCallback( v8::Local<v8::String> property,
                                 const v8::PropertyCallbackInfo<v8::Value>& info,
                                 Callbacks_e callback )
{
    using namespace v8;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );

    if( jsPlayer->_jsCallbacks[callback].IsEmpty() )
        return;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> callbackFunc =
        Local<Function>::New( isolate, jsPlayer->_jsCallbacks[callback] );

    info.GetReturnValue().Set( callbackFunc );
}

void JsVlcPlayer::setJsCallback( v8::Local<v8::String> property,
                                 v8::Local<v8::Value> value,
                                 const v8::PropertyCallbackInfo<void>& info,
                                 Callbacks_e callback )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( info.Holder() );

    Local<Function> callbackFunc = Local<Function>::Cast( value );
    if( !callbackFunc.IsEmpty() )
        jsPlayer->_jsCallbacks[callback].Reset( isolate, callbackFunc );
}
