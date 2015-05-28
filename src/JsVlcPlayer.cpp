#include "JsVlcPlayer.h"

v8::Persistent<v8::Function> JsVlcPlayer::_jsConstructor;

JsVlcPlayer::JsVlcPlayer( const v8::Local<v8::Function>& renderCallback ) :
    _libvlc( nullptr ), _frameWidth( 0 ), _frameHeight( 0 ),
    _jsRawFrameBuffer( nullptr )
{
    _jsRenderCallback.Reset( v8::Isolate::GetCurrent(), renderCallback );

    // https://wiki.videolan.org/VLC_command-line_help/
    // Find a way to avoid reading for subtitles in the same folder; this behavior should be explicit
    const char * const vlc_args[] = {
        "--no-media-library", // not needed?
        "--no-spu",
        "--no-stats",
        "--no-osd",
        "--sout-mux-caching","3000",
        "--network-caching","3000",
        "--no-drop-late-frames", // This might fix the uglyness when dropping frames
        //"--prefer-system-codecs", // optimizes a bit on OS X but not worth the risk
        // no deinterlace
        "--no-video-title-show", // not needed?
	"--ffmpeg-hw",
	"--avcodec-hw=any"
    };

    _libvlc = libvlc_new( sizeof(vlc_args) / sizeof(vlc_args[0]), vlc_args );
    assert( _libvlc );
    if( _player.open( _libvlc ) ) {
        vlc::basic_vmem_wrapper::open( &_player.basic_player() );
    } else {
        assert( false );
    }

    uv_loop_t* loop = uv_default_loop();

    _formatSetupAsync.data = this;
    uv_async_init( loop, &_formatSetupAsync,
        [] ( uv_async_t* handle ) {
            if( handle->data )
                reinterpret_cast<JsVlcPlayer*>( handle->data )->setupBuffer();
        }
    );

    _frameUpdatedAsync.data = this;
    uv_async_init( loop, &_frameUpdatedAsync,
        [] ( uv_async_t* handle ) {
            if( handle->data )
                reinterpret_cast<JsVlcPlayer*>( handle->data )->frameUpdated();
        }
    );
}

JsVlcPlayer::~JsVlcPlayer()
{
    vlc::basic_vmem_wrapper::close();

    _formatSetupAsync.data = 0;
    uv_close( reinterpret_cast<uv_handle_t*>( &_formatSetupAsync ), 0 );

    _frameUpdatedAsync.data = 0;
    uv_close( reinterpret_cast<uv_handle_t*>( &_frameUpdatedAsync ), 0 );
}

unsigned JsVlcPlayer::video_format_cb( char* chroma,
                                       unsigned* width, unsigned* height,
                                       unsigned* pitches, unsigned* lines )
{
    _frameWidth  = *width;
    _frameHeight = *height;

    memcpy( chroma, "I420", 4 );
    pitches[0] = _frameWidth; if( pitches[0] % 4 ) pitches[0] += 4 - pitches[0] % 4;
    pitches[1] = ( _frameWidth + 1 ) / 2; if( pitches[1] % 4 ) pitches[1] += 4 - pitches[1] % 4;
    pitches[2] = pitches[1];

    lines[0] = _frameHeight;
    lines[1] = ( _frameHeight + 1 ) / 2;
    lines[2] = lines[1];

    m_UPlaneOffset = pitches[0] * lines[0];
    m_VPlaneOffset = m_UPlaneOffset + pitches[1] * lines[1];

    _tmpFrameBuffer.resize( pitches[0] * lines[0] + pitches[1] * lines[1] + pitches[2] * lines[2] );

    uv_async_send( &_formatSetupAsync );

    return 3;
}

void JsVlcPlayer::video_cleanup_cb()
{
    if( !_tmpFrameBuffer.empty() )
        std::vector<char>().swap(_tmpFrameBuffer);

    m_UPlaneOffset = m_VPlaneOffset = 0;

    uv_async_send( &_frameUpdatedAsync );
}

void* JsVlcPlayer::video_lock_cb( void** planes )
{
    if (_jsRawFrameBuffer && !_tmpFrameBuffer.empty()) std::vector<char>().swap(_tmpFrameBuffer);

    char* b = _jsRawFrameBuffer ? _jsRawFrameBuffer : _tmpFrameBuffer.data();
    
    planes[0] = b;
    planes[1] = b + m_UPlaneOffset;
    planes[2] = b + m_VPlaneOffset;

    return 0;
}

void JsVlcPlayer::video_unlock_cb( void* /*picture*/, void *const * /*planes*/ )
{

}

void JsVlcPlayer::video_display_cb( void* /*picture*/ )
{
    uv_async_send( &_frameUpdatedAsync );
}

void JsVlcPlayer::setupBuffer()
{
    using namespace v8;

    if( 0 == _frameWidth || 0 == _frameHeight )
        return;

    const unsigned frameBufferSize =
        _frameWidth * _frameHeight * vlc::DEF_PIXEL_BYTES;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Object> global = isolate->GetCurrentContext()->Global();

    Local<Value> abv =
        global->Get(
            String::NewFromUtf8( isolate,
                                 "Uint8Array",
                                 v8::String::kInternalizedString ) );
    Local<Value> argv[] =
        { Integer::NewFromUnsigned( isolate, frameBufferSize ) };
    Local<Object> array =
        Handle<Function>::Cast( abv )->NewInstance( 1, argv );

    array->Set( String::NewFromUtf8( isolate, "width" ),
                Integer::New( isolate, _frameWidth ) );
    array->Set( String::NewFromUtf8( isolate, "height" ),
                Integer::New( isolate, _frameHeight) );
    array->Set( String::NewFromUtf8( isolate, "UPlaneOffset" ),
                Integer::New( isolate, m_UPlaneOffset ) );
    array->Set( String::NewFromUtf8( isolate, "VPlaneOffset" ),
                Integer::New( isolate, m_VPlaneOffset) );

    _jsFrameBuffer.Reset( isolate, array );

    _jsRawFrameBuffer =
        static_cast<char*>( array->GetIndexedPropertiesExternalArrayData() );
}

void JsVlcPlayer::frameUpdated()
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> renderCallback =
        Local<Function>::New( isolate, _jsRenderCallback );

    Local<Value> argv[] =
        { Local<Object>::New( isolate, _jsFrameBuffer ) };

    renderCallback->Call( isolate->GetCurrentContext()->Global(),
                          sizeof( argv ) / sizeof( argv[0] ), argv );
}

void JsVlcPlayer::initJsApi()
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();

    Local<FunctionTemplate> ct = FunctionTemplate::New( isolate, jsCreate );
    ct->SetClassName( String::NewFromUtf8( isolate, "VlcPlayer" ) );
    ct->InstanceTemplate()->SetInternalFieldCount( 1 );

    NODE_SET_PROTOTYPE_METHOD( ct, "play", jsPlay );
    NODE_SET_PROTOTYPE_METHOD( ct, "stop", jsStop );

    _jsConstructor.Reset( isolate, ct->GetFunction() );
}

void JsVlcPlayer::jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    using namespace v8;

    if( args.Length() != 1 )
        return;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> renderCallback = Local<Function>::Cast( args[0] );

    if( args.IsConstructCall() ) {
        JsVlcPlayer* jsPlayer = new JsVlcPlayer( renderCallback );
        jsPlayer->Wrap( args.This() );
        args.GetReturnValue().Set( args.This() );
    } else {
        Local<Value> argv[] = { renderCallback };
        Local<Function> constructor =
            Local<Function>::New( isolate, _jsConstructor );
        args.GetReturnValue().Set(
            constructor->NewInstance( sizeof( argv ) / sizeof( argv[0] ),
                                      argv ) );
    }
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

void JsVlcPlayer::jsStop( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    if( args.Length() != 0 )
        return;

    JsVlcPlayer* jsPlayer = ObjectWrap::Unwrap<JsVlcPlayer>( args.Holder() );
    vlc::player& player = jsPlayer->_player;

    player.stop();
}
