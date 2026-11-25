#include "global.h"
#include "MovieTexture_Generic.h"
#include "PrefsManager.h"
#include "RageDisplay.h"
#include "RageLog.h"
#include "RageSurface.h"
#include "RageTextureManager.h"
#include "RageTextureRenderTarget.h"
#include "RageUtil.h"
#include "Sprite.h"

#if defined(WIN32)
#include "archutils/Win32/ErrorStrings.h"
#include <windows.h>
#endif


static Preference<bool> g_bMovieTextureDirectUpdates( "MovieTextureDirectUpdates", true );

MovieTexture_Generic::MovieTexture_Generic( RageTextureID ID, MovieDecoder *pDecoder ):
	RageMovieTexture( ID ),
	m_DecodeEvent( "MovieDecodeEvent" )
{
	LOG->Trace( "MovieTexture_Generic::MovieTexture_Generic(%s)", ID.filename.c_str() );

	m_pDecoder = pDecoder;

	m_uTexHandle = 0;
	m_pRenderTarget = nullptr;
	m_pTextureIntermediate = nullptr;
	m_bLoop = true;
	m_pSurface = nullptr;
	m_pDecodeSurface = nullptr;
	m_pTextureLock = nullptr;
	m_ImageWaiting = FRAME_NONE;
	m_fRate = 1;
	m_bWantRewind = false;
	m_fClock = 0;
	m_bFrameSkipMode = false;
	m_bThreaded = false;
	m_bQuitThread = false;
	m_bThreadWantRewind = false;
	m_bThreadEOF = false;
	m_bFrameReady = false;
	m_bLoopResetPending = false;
	m_fLoopResetDelay = 0;
	m_fTargetClock = 0;
	m_fDecodedTimestamp = 0;
	m_fDecodedFrameDelay = 0;
	m_fLastDisplayedTimestamp = 0;
	m_pSprite = new Sprite;
}

RString MovieTexture_Generic::Init()
{
	RString sError = m_pDecoder->Open( GetID().filename );
	if( sError != "" )
		return sError;

	m_bThreaded = PREFSMAN->m_bThreadedMovieDecode;

	CreateTexture();
	CreateFrameRects();

	/* Decode one frame, to guarantee that the texture is drawn when this function returns. */
	int ret = m_pDecoder->DecodeFrame( -1 );
	if( ret == -1 )
		return ssprintf( "%s: error getting first frame", GetID().filename.c_str() );
	if( ret == 0 )
	{
		/* There's nothing there. */
		return ssprintf( "%s: EOF getting first frame", GetID().filename.c_str() );
	}

	m_ImageWaiting = FRAME_DECODED;

	LOG->Trace( "Resolution: %ix%i (%ix%i, %ix%i)",
			m_iSourceWidth, m_iSourceHeight,
			m_iImageWidth, m_iImageHeight, m_iTextureWidth, m_iTextureHeight );

	UpdateFrame();
	m_ImageWaiting = FRAME_NONE;
	m_fLastDisplayedTimestamp = m_pDecoder->GetTimestamp();
	m_fDecodedTimestamp = m_fLastDisplayedTimestamp;
	m_fDecodedFrameDelay = m_pDecoder->GetFrameDuration();

	if( m_bThreaded )
	{
		m_DecodeThread.SetName( ssprintf("Movie (%s)", Basename(GetID().filename).c_str()) );
		m_DecodeThread.Create( DecoderThread_start, this );
	}

	CHECKPOINT_M("Generic initialization completed. No errors found.");

	return RString();
}

MovieTexture_Generic::~MovieTexture_Generic()
{
	if( m_DecodeThread.IsCreated() )
	{
		m_DecodeEvent.Lock();
		m_bQuitThread = true;
		m_DecodeEvent.Broadcast();
		m_DecodeEvent.Unlock();
		m_DecodeThread.Wait();
	}

	if( m_pDecoder )
		m_pDecoder->Close();

	/* m_pSprite may reference the texture; delete it before DestroyTexture. */
	delete m_pSprite;
	
	DestroyTexture();

	delete m_pDecoder;
}

/* Delete the surface and texture.  The decoding thread must be stopped, and this
 * is normally done after destroying the decoder. */
void MovieTexture_Generic::DestroyTexture()
{
	delete m_pSurface;
	m_pSurface = nullptr;

	delete m_pDecodeSurface;
	m_pDecodeSurface = nullptr;

	delete m_pTextureLock;
	m_pTextureLock = nullptr;

	if( m_uTexHandle )
	{
		DISPLAY->DeleteTexture( m_uTexHandle );
		m_uTexHandle = 0;
	}

	delete m_pRenderTarget;
	m_pRenderTarget = nullptr;
	delete m_pTextureIntermediate;
	m_pTextureIntermediate = nullptr;
}

class RageMovieTexture_Generic_Intermediate : public RageTexture
{
public:
	RageMovieTexture_Generic_Intermediate( RageTextureID ID, int iWidth, int iHeight, 
		int iImageWidth, int iImageHeight, int iTextureWidth, int iTextureHeight,
		RageSurfaceFormat SurfaceFormat, RagePixelFormat pixfmt ):
		RageTexture(ID),
		m_SurfaceFormat( SurfaceFormat )
	{
		m_PixFmt = pixfmt;
		m_iSourceWidth = iWidth;
		m_iSourceHeight = iHeight;
/*		int iMaxSize = min( GetID().iMaxSize, DISPLAY->GetMaxTextureSize() );
		m_iImageWidth = min( m_iSourceWidth, iMaxSize );
		m_iImageHeight = min( m_iSourceHeight, iMaxSize );
		m_iTextureWidth = power_of_two( m_iImageWidth );
		m_iTextureHeight = power_of_two( m_iImageHeight );
*/
		
		m_iImageWidth = iImageWidth;
		m_iImageHeight = iImageHeight;
		m_iTextureWidth = iTextureWidth;
		m_iTextureHeight = iTextureHeight;

		CreateFrameRects();

		m_uTexHandle = 0;
		CreateTexture();
	}
	virtual ~RageMovieTexture_Generic_Intermediate()
	{
		if( m_uTexHandle )
		{
			DISPLAY->DeleteTexture( m_uTexHandle );
			m_uTexHandle = 0;
		}
	}

	virtual void Invalidate() { m_uTexHandle = 0; }
	virtual void Reload() { }
	virtual uintptr_t GetTexHandle() const
	{
		return m_uTexHandle;
	}

	bool IsAMovie() const { return true; }
private:
	void CreateTexture()
	{
		if( m_uTexHandle )
			return;

		RageSurface *pSurface = CreateSurfaceFrom( m_iImageWidth, m_iImageHeight,
			m_SurfaceFormat.BitsPerPixel,
			m_SurfaceFormat.Mask[0],
			m_SurfaceFormat.Mask[1],
			m_SurfaceFormat.Mask[2],
			m_SurfaceFormat.Mask[3], nullptr, 1 );

		m_uTexHandle = DISPLAY->CreateTexture( m_PixFmt, pSurface, false );
		delete pSurface;
	}

	uintptr_t m_uTexHandle;
	RageSurfaceFormat m_SurfaceFormat;
	RagePixelFormat m_PixFmt;
};

void MovieTexture_Generic::Invalidate()
{
	m_uTexHandle = 0;
	if( m_pTextureIntermediate != nullptr )
		m_pTextureIntermediate->Invalidate();
}

void MovieTexture_Generic::CreateTexture()
{
	if( m_uTexHandle || m_pRenderTarget != nullptr )
		return;

	CHECKPOINT;

	m_iSourceWidth  = m_pDecoder->GetWidth();
	m_iSourceHeight = m_pDecoder->GetHeight();

	/* Adjust m_iSourceWidth to support different source aspect ratios. */
	float fSourceAspectRatio = m_pDecoder->GetSourceAspectRatio();
	if( fSourceAspectRatio < 1 )
		m_iSourceHeight = lrintf( m_iSourceHeight / fSourceAspectRatio );
	else if( fSourceAspectRatio > 1 )
		m_iSourceWidth = lrintf( m_iSourceWidth * fSourceAspectRatio );

	/* HACK: Don't cap movie textures to the max texture size, since we
	 * render them onto the texture at the source dimensions.  If we find a
	 * fast way to resize movies, we can change this back. */
	m_iImageWidth = m_iSourceWidth;
	m_iImageHeight = m_iSourceHeight;

#if defined(__SWITCH__)
	const int iMaxMovieWidth = 640;
	const int iMaxMovieHeight = 480;
	if( m_iImageWidth > iMaxMovieWidth || m_iImageHeight > iMaxMovieHeight )
	{
		const float fScale = min( (float)iMaxMovieWidth / m_iImageWidth, (float)iMaxMovieHeight / m_iImageHeight );
		m_iImageWidth = max( 2, (int)(m_iImageWidth * fScale) & ~1 );
		m_iImageHeight = max( 2, (int)(m_iImageHeight * fScale) & ~1 );
	}
#endif

	/* Texture dimensions need to be a power of two; jump to the next. */
	m_iTextureWidth = power_of_two( m_iImageWidth );
	m_iTextureHeight = power_of_two( m_iImageHeight );
	MovieDecoderPixelFormatYCbCr fmt = PixelFormatYCbCr_Invalid;
	if( m_pSurface == nullptr )
	{
		ASSERT( m_pTextureLock == nullptr );
		const bool bHighColor = TEXTUREMAN->GetPrefs().m_iMovieColorDepth == 32;
		if( !m_bThreaded && g_bMovieTextureDirectUpdates )
			m_pTextureLock = DISPLAY->CreateTextureLock();

		m_pSurface = m_pDecoder->CreateCompatibleSurface( m_iImageWidth, m_iImageHeight,
			bHighColor, fmt );
		if( m_bThreaded )
		{
			m_pDecodeSurface = m_pDecoder->CreateCompatibleSurface( m_iImageWidth, m_iImageHeight,
				bHighColor, fmt );
		}
		if( m_pTextureLock != nullptr )
		{
			delete [] m_pSurface->pixels;
			m_pSurface->pixels = nullptr;
		}

	}

	RagePixelFormat pixfmt = DISPLAY->FindPixelFormat( m_pSurface->format->BitsPerPixel,
			m_pSurface->format->Mask[0],
			m_pSurface->format->Mask[1],
			m_pSurface->format->Mask[2],
			m_pSurface->format->Mask[3] );

	if( pixfmt == RagePixelFormat_Invalid )
	{
		/* We weren't given a natively-supported pixel format.  Pick a supported
		 * one.  This is a fallback case, and implies a second conversion. */
		int depth = TEXTUREMAN->GetPrefs().m_iMovieColorDepth;
		switch( depth )
		{
		default:
			FAIL_M(ssprintf("Unsupported movie color depth: %i", depth));
		case 16:
			if( DISPLAY->SupportsTextureFormat(RagePixelFormat_RGB5) )
				pixfmt = RagePixelFormat_RGB5;
			else
				pixfmt = RagePixelFormat_RGBA4;

			break;

		case 32:
			if( DISPLAY->SupportsTextureFormat(RagePixelFormat_RGB8) )
				pixfmt = RagePixelFormat_RGB8;
			else if( DISPLAY->SupportsTextureFormat(RagePixelFormat_RGBA8) )
				pixfmt = RagePixelFormat_RGBA8;
			else if( DISPLAY->SupportsTextureFormat(RagePixelFormat_RGB5) )
				pixfmt = RagePixelFormat_RGB5;
			else
				pixfmt = RagePixelFormat_RGBA4;
			break;
		}
	}

	if( fmt != PixelFormatYCbCr_Invalid )
	{
		SAFE_DELETE( m_pTextureIntermediate );
		m_pSprite->UnloadTexture();

		/* Create the render target.  This will receive the final, converted texture. */
		RenderTargetParam param;
		param.iWidth = m_iImageWidth;
		param.iHeight = m_iImageHeight;

		RageTextureID TargetID( GetID() );
		TargetID.filename += " target";
		m_pRenderTarget = new RageTextureRenderTarget( TargetID, param );

		/* Create the intermediate texture.  This receives the YUV image. */
		RageTextureID IntermedID( GetID() );
		IntermedID.filename += " intermediate";

		m_pTextureIntermediate = new RageMovieTexture_Generic_Intermediate( IntermedID,
			m_iImageWidth, m_iImageHeight,
			m_pSurface->w, m_pSurface->h,
			power_of_two(m_pSurface->w), power_of_two(m_pSurface->h),
			*m_pSurface->format, pixfmt );

		/* Configure the sprite.  This blits the intermediate onto the ifnal render target. */
		m_pSprite->SetHorizAlign( align_left );
		m_pSprite->SetVertAlign( align_top );

		/* Hack: Sprite wants to take ownership of the texture, and will decrement the refcount
		 * when it unloads the texture.  Normally we'd make a "copy", but we can't access
		 * RageTextureManager from here.  Just increment the refcount. */
		++m_pTextureIntermediate->m_iRefCount;
		m_pSprite->SetTexture( m_pTextureIntermediate );
		m_pSprite->SetEffectMode( GetEffectMode(fmt) );

		return;
	}

	m_uTexHandle = DISPLAY->CreateTexture( pixfmt, m_pSurface, false );
}

/* Handle decoding for a frame.  Return true if a frame was decoded, false if not
 * (due to quit, error, EOF, etc).  If true is returned, we'll be in FRAME_DECODED. */
bool MovieTexture_Generic::DecodeFrame()
{
	bool bTriedRewind = false;
	do
	{
		if( m_bWantRewind )
		{
			if( bTriedRewind )
			{
				LOG->Trace( "File \"%s\" looped more than once in one frame", GetID().filename.c_str() );
				return false;
			}
			m_bWantRewind = false;
			bTriedRewind = true;

			/* When resetting the clock, set it back by the length of the last frame,
			 * so it has a proper delay. */
			float fDelay = m_pDecoder->GetFrameDuration();

			/* Restart. */
			m_pDecoder->Rewind();

			m_fClock = -fDelay;
		}

		/* Read a frame. */
		float fTargetTime = -1;
		if( m_bFrameSkipMode && m_fClock > m_pDecoder->GetTimestamp() )
			fTargetTime = m_fClock;

		int ret = m_pDecoder->DecodeFrame( fTargetTime );
		if( ret == -1 )
			return false;

		if( m_bWantRewind && m_pDecoder->GetTimestamp() == 0 )
			m_bWantRewind = false; /* ignore */

		if( ret == 0 )
		{
			/* EOF. */
			if( !m_bLoop )
				return false;

			LOG->Trace( "File \"%s\" looping", GetID().filename.c_str() );
			m_bWantRewind = true;
			continue;
		}

		/* We got a frame. */
	} while( m_bWantRewind );

	return true;
}

/*
 * Returns:
 *  == 0 if the currently decoded frame is ready to be displayed
 *   > 0 (seconds) if it's not yet time to display;
 */
float MovieTexture_Generic::CheckFrameTime()
{
	if( m_fRate == 0 )
		return 1;	// "a long time until the next frame"

	const float fOffset = (m_pDecoder->GetTimestamp() - m_fClock) / m_fRate;

	/* If we're ahead, we're decoding too fast; delay. */
	if( fOffset > 0.00001f )
	{
		if( m_bFrameSkipMode )
		{
			/* We're caught up; stop skipping frames. */
			LOG->Trace( "stopped skipping frames" );
			m_bFrameSkipMode = false;
		}
		return fOffset;
	}

	/*
	 * We're behind by -Offset seconds.  
	 *
	 * If we're just slightly behind, don't worry about it; we'll simply
	 * not sleep, so we'll move as fast as we can to catch up.
	 *
	 * If we're far behind, we're short on CPU.  Skip texture updates; this
	 * is a big bottleneck on many systems.
	 *
	 * If we hit a threshold, start skipping frames via #1.  If we do that,
	 * don't stop once we hit the threshold; keep doing it until we're fully
	 * caught up.
	 *
	 * We should try to notice if we simply don't have enough CPU for the video;
	 * it's better to just stay in frame skip mode than to enter and exit it
	 * constantly, but we don't want to do that due to a single timing glitch.
	 */
	const float FrameSkipThreshold = 0.08f;

	if( -fOffset >= FrameSkipThreshold && !m_bFrameSkipMode )
	{
		LOG->Trace( "(%s) Time is %f, and the movie is at %f.  Entering frame skip mode.",
			GetID().filename.c_str(), m_fClock, m_pDecoder->GetTimestamp() );
		m_bFrameSkipMode = true;
	}

	return 0;
}

int MovieTexture_Generic::DecoderThread_start( void *p )
{
	((MovieTexture_Generic *)p)->DecoderThread();
	return 0;
}

void MovieTexture_Generic::DecoderThread()
{
	m_DecodeEvent.Lock();
	while( !m_bQuitThread )
	{
		if( m_bThreadWantRewind )
		{
			m_bThreadWantRewind = false;
			m_bThreadEOF = false;
			m_bFrameReady = false;
			m_bLoopResetPending = false;
			m_DecodeEvent.Unlock();

			m_pDecoder->Rewind();

			m_DecodeEvent.Lock();
			continue;
		}

		const float fMinFrameWindow = max( m_fDecodedFrameDelay, 1.0f / 60.0f );
		const bool bQueuedFrameStillCurrent =
			m_bFrameReady &&
			(m_fRate == 0 || m_fTargetClock <= m_fDecodedTimestamp + fMinFrameWindow);
		if( m_bThreadEOF || m_bLoopResetPending || bQueuedFrameStillCurrent )
		{
			m_DecodeEvent.Wait();
			continue;
		}

		float fTargetTime = -1;
		if( m_bFrameSkipMode && m_fTargetClock > m_pDecoder->GetTimestamp() )
			fTargetTime = m_fTargetClock;
		else if( m_bFrameReady && m_fTargetClock > m_fDecodedTimestamp + fMinFrameWindow )
			fTargetTime = m_fTargetClock;

		const bool bLoop = m_bLoop;
		m_DecodeEvent.Unlock();

		int ret = m_pDecoder->DecodeFrame( fTargetTime );
		if( ret == 1 )
			m_pDecoder->GetFrame( m_pDecodeSurface );

		m_DecodeEvent.Lock();
		if( m_bQuitThread || m_bThreadWantRewind )
			continue;

		if( ret == -1 )
		{
			m_bThreadEOF = true;
			continue;
		}
		if( ret == 0 )
		{
			if( !bLoop )
			{
				m_bThreadEOF = true;
				continue;
			}

			LOG->Trace( "File \"%s\" looping", GetID().filename.c_str() );
			const float fDelay = m_pDecoder->GetFrameDuration();
			m_DecodeEvent.Unlock();

			m_pDecoder->Rewind();
			int rewindRet = m_pDecoder->DecodeFrame( -1 );
			if( rewindRet == 1 )
				m_pDecoder->GetFrame( m_pDecodeSurface );

			m_DecodeEvent.Lock();
			if( m_bQuitThread || m_bThreadWantRewind )
				continue;

			if( rewindRet != 1 )
			{
				m_bThreadEOF = true;
				continue;
			}

			m_bLoopResetPending = true;
			m_fLoopResetDelay = fDelay;
			m_fDecodedTimestamp = m_pDecoder->GetTimestamp();
			m_fDecodedFrameDelay = m_pDecoder->GetFrameDuration();
			m_bFrameReady = true;
			continue;
		}

		m_fDecodedTimestamp = m_pDecoder->GetTimestamp();
		m_fDecodedFrameDelay = m_pDecoder->GetFrameDuration();
		m_bFrameReady = true;
	}
	m_DecodeEvent.Unlock();
}

/* Decode data. */
void MovieTexture_Generic::DecodeSeconds( float fSeconds )
{
	if( m_bThreaded )
	{
		bool bUpload = false;
		{
			LockMut( m_DecodeEvent );
			m_fClock += fSeconds * m_fRate;
			m_fTargetClock = m_fClock;

			if( m_bLoopResetPending )
			{
				m_fClock = -m_fLoopResetDelay;
				m_fTargetClock = m_fClock;
				m_bLoopResetPending = false;
				m_bFrameSkipMode = false;
			}

			if( m_bFrameReady )
			{
				const float fOffset = (m_fRate != 0) ? (m_fDecodedTimestamp - m_fClock) / m_fRate : 1.0f;
				if( fOffset > 0.00001f )
				{
					if( m_bFrameSkipMode )
					{
						LOG->Trace( "stopped skipping frames" );
						m_bFrameSkipMode = false;
					}
				}
				else
				{
					const float FrameSkipThreshold = 0.08f;
					if( -fOffset >= FrameSkipThreshold && !m_bFrameSkipMode )
					{
						LOG->Trace( "(%s) Time is %f, and the movie is at %f.  Entering frame skip mode.",
							GetID().filename.c_str(), m_fClock, m_fDecodedTimestamp );
						m_bFrameSkipMode = true;
					}

					std::swap( m_pSurface, m_pDecodeSurface );
					m_fLastDisplayedTimestamp = m_fDecodedTimestamp;
					m_bFrameReady = false;
					bUpload = true;
					m_DecodeEvent.Signal();
				}
			}
			else if( !m_bThreadEOF && m_fRate != 0 )
			{
				const float FrameSkipThreshold = 0.08f;
				if( (m_fClock - m_fLastDisplayedTimestamp) / m_fRate >= FrameSkipThreshold && !m_bFrameSkipMode )
				{
					LOG->Trace( "(%s) Time is %f, and the movie is at %f.  Entering frame skip mode.",
						GetID().filename.c_str(), m_fClock, m_fLastDisplayedTimestamp );
					m_bFrameSkipMode = true;
				}
				m_DecodeEvent.Signal();
			}
		}

		if( bUpload )
			UploadFrame();
		return;
	}

	m_fClock += fSeconds * m_fRate;

	/* We might need to decode more than one frame per update.  However, there
	 * have been bugs in ffmpeg that cause it to not handle EOF properly, which
	 * could make this never return, so let's play it safe. */
	int iMax = 8;
	while( --iMax )
	{
		/* If we don't have a frame decoded, decode one. */
		if( m_ImageWaiting == FRAME_NONE )
		{
			if( !DecodeFrame() )
				break;

			m_ImageWaiting = FRAME_DECODED;
		}

		/* If we have a frame decoded, see if it's time to display it. */
		float fTime = CheckFrameTime();
		if ( fTime > 0 )
			return;

		/* If this frame is already expired and we have iterations left, drop it
		 * before sws_scale/upload so we catch up to the audio clock. */
		if( iMax > 1 && m_fRate > 0 && m_fClock > m_pDecoder->GetTimestamp() + m_pDecoder->GetFrameDuration() )
		{
			m_ImageWaiting = FRAME_NONE;
			continue;
		}

		UpdateFrame();
		m_ImageWaiting = FRAME_NONE;
		return;
	}

	LOG->MapLog( "movie_looping", "MovieTexture_Generic::Update looping" );
}

void MovieTexture_Generic::UpdateFrame()
{
	/* Just in case we were invalidated: */
	CreateTexture();

	if( m_pTextureLock != nullptr )
	{
		uintptr_t iHandle = m_pTextureIntermediate != nullptr? m_pTextureIntermediate->GetTexHandle(): this->GetTexHandle();
		m_pTextureLock->Lock( iHandle, m_pSurface );
	}

	m_pDecoder->GetFrame( m_pSurface );
	if( m_pTextureLock != nullptr )
		m_pTextureLock->Unlock( m_pSurface, true );

	UploadFrame();
}

void MovieTexture_Generic::UploadFrame()
{
	/* Just in case we were invalidated: */
	CreateTexture();

	if( m_pRenderTarget != nullptr )
	{
		CHECKPOINT_M( "About to upload the texture.");

		/* If we have no m_pTextureLock, we still have to upload the texture. */
		if( m_pTextureLock == nullptr )
		{
			DISPLAY->UpdateTexture(
				m_pTextureIntermediate->GetTexHandle(),
				m_pSurface,
				0, 0,
				m_pSurface->w, m_pSurface->h );
		}
		m_pRenderTarget->BeginRenderingTo( false );
		m_pSprite->Draw();
		m_pRenderTarget->FinishRenderingTo();
	}
	else
	{
		if( m_pTextureLock == nullptr )
		{
			DISPLAY->UpdateTexture(
				m_uTexHandle,
				m_pSurface,
				0, 0,
				m_iImageWidth, m_iImageHeight );
		}
	}
}

static EffectMode EffectModes[] = 
{
	EffectMode_YUYV422,
};
COMPILE_ASSERT( ARRAYLEN(EffectModes) == NUM_PixelFormatYCbCr );

EffectMode MovieTexture_Generic::GetEffectMode( MovieDecoderPixelFormatYCbCr fmt )
{
	ASSERT( fmt != PixelFormatYCbCr_Invalid );
	return EffectModes[fmt];
}

void MovieTexture_Generic::Reload()
{
}

void MovieTexture_Generic::SetPlaybackRate( float fRate )
{
	if( m_bThreaded )
	{
		LockMut( m_DecodeEvent );
		m_fRate = fRate;
		m_DecodeEvent.Signal();
	}
	else
	{
		m_fRate = fRate;
	}
}

void MovieTexture_Generic::SetPosition( float fSeconds )
{
	/* We can reset to 0, but I don't think this API supports fast seeking
	 * yet.  I don't think we ever actually seek except to 0 right now,
	 * anyway. XXX */
	if( fSeconds != 0 )
	{
		LOG->Warn( "MovieTexture_Generic::SetPosition(%f): non-0 seeking unsupported; ignored", fSeconds );
		return;
	}

	LOG->Trace( "Seek to %f", fSeconds );
	if( m_bThreaded )
	{
		LockMut( m_DecodeEvent );
		m_bThreadWantRewind = true;
		m_bFrameReady = false;
		m_bLoopResetPending = false;
		m_bThreadEOF = false;
		m_fClock = 0;
		m_fTargetClock = 0;
		m_bFrameSkipMode = false;
		m_fLastDisplayedTimestamp = 0;
		m_DecodeEvent.Signal();
	}
	else
	{
		m_bWantRewind = true;
	}
}

uintptr_t MovieTexture_Generic::GetTexHandle() const
{
	if( m_pRenderTarget != nullptr )
		return m_pRenderTarget->GetTexHandle();

	return m_uTexHandle;
}

/*
 * (c) 2003-2005 Glenn Maynard
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
