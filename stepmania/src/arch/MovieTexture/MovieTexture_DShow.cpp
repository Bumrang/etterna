#include "global.h"
/*
-----------------------------------------------------------------------------
 Class: MovieTexture_DShow

 Desc: See header.

 Copyright (c) 2001-2003 by the person(s) listed below.  All rights reserved.
	Chris Danford
	Glenn Maynard
-----------------------------------------------------------------------------
*/

#pragma comment(lib, "winmm.lib") 
 
// Link with the DirectShow base class libraries
#if defined(DEBUG)
	#pragma comment(lib, "baseclasses/debug/strmbasd.lib") 
#else
	#pragma comment(lib, "baseclasses/release/strmbase.lib") 
#endif

#include "MovieTexture_DShowHelper.h"
#include "MovieTexture_DShow.h"

/* for TEXTUREMAN->GetTextureColorDepth() */
#include "RageTextureManager.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "RageException.h"

#include "archutils/win32/tls.h"

#include <stdio.h>

#include "SDL_utils.h"

//-----------------------------------------------------------------------------
// MovieTexture_DShow constructor
//-----------------------------------------------------------------------------
MovieTexture_DShow::MovieTexture_DShow( RageTextureID ID ) :
	RageMovieTexture( ID )
{
	LOG->Trace( "RageBitmapTexture::RageBitmapTexture()" );

	m_uTexHandle = 0;
	buffer = NULL;
	buffer_lock = SDL_CreateSemaphore(1);
	buffer_finished = SDL_CreateSemaphore(0);
	m_img = NULL;

	Create();
	CreateFrameRects();

	// flip all frame rects because movies are upside down
	for( unsigned i=0; i<m_TextureCoordRects.size(); i++ )
		swap(m_TextureCoordRects[i].top, m_TextureCoordRects[i].bottom);

	m_bLoop = true;
	m_bPlaying = false;
}

/* Hold buffer_lock.  If it's held, then the decoding thread is waiting
 * for us to process a frame; do so. */
void MovieTexture_DShow::SkipUpdates()
{
	while(SDL_SemTryWait(buffer_lock))
		Update(0);
}

void MovieTexture_DShow::StopSkippingUpdates()
{
	SDL_SemPost(buffer_lock);
}

MovieTexture_DShow::~MovieTexture_DShow()
{
	LOG->Trace("MovieTexture_DShow::~MovieTexture_DShow");
	LOG->Flush();

	SkipUpdates();

	/* Shut down the graph.  We can't call Stop() here, since that will
	 * call SkipUpdates again, which will deadlock if we call it twice
	 * in a row. */
    if (m_pGB) {
		LOG->Trace("MovieTexture_DShow: shutdown");
	LOG->Flush();
		CComPtr<IMediaControl> pMC;
		m_pGB.QueryInterface(&pMC);

		HRESULT hr;
		if( FAILED( hr = pMC->Stop() ) )
			RageException::Throw( hr_ssprintf(hr, "Could not stop the DirectShow graph.") );

//		Stop();
		m_pGB.Release();
	}
	LOG->Trace("MovieTexture_DShow: shutdown ok");
	LOG->Flush();
	if(m_uTexHandle)
		DISPLAY->DeleteTexture( m_uTexHandle );
	SDL_FreeSurface( m_img );

	SDL_DestroySemaphore(buffer_lock);
	SDL_DestroySemaphore(buffer_finished);
}

void MovieTexture_DShow::Reload()
{
	// do nothing
}


void MovieTexture_DShow::Update(float fDeltaTime)
{
	VDCHECKPOINT;

	// restart the movie if we reach the end
	if(m_bLoop)
	{
		// Check for completion events
		CComPtr<IMediaEvent>    pME;
		m_pGB.QueryInterface(&pME);

		long lEventCode, lParam1, lParam2;
		pME->GetEvent( &lEventCode, &lParam1, &lParam2, 0 );
		if( lEventCode == EC_COMPLETE )
			SetPosition(0);
	}

	if(buffer == NULL)
		return;

	/* Just in case we were invalidated: */
	CreateTexture();

	// DirectShow feeds us in BGR8
	SDL_Surface *fromDShow = SDL_CreateRGBSurfaceFrom(
		(void*)buffer, m_iSourceWidth, m_iSourceHeight,
		24, 
		m_iSourceWidth*3,
		0xFF0000,
		0x00FF00,
		0x0000FF,
		0x000000 );

	/* We don't want to actually blend the alpha channel over the destination converted
	 * surface; we want to simply blit it, so make sure SDL_SRCALPHA is not on. */
	SDL_SetAlpha( fromDShow, 0, SDL_ALPHA_OPAQUE );

	SDL_Rect area;
	area.x = area.y = 0;
	area.w = short(fromDShow->w);
	area.h = short(fromDShow->h);

	SDL_BlitSurface(fromDShow, &area, m_img, &area);

	DISPLAY->UpdateTexture(
		m_uTexHandle, 
		m_PixelFormat,
		m_img,
		0, 0,
		min(m_iSourceWidth,m_iTextureWidth), min(m_iSourceHeight,m_iTextureHeight) );

	SDL_FreeSurface( fromDShow );

	buffer = NULL;

	VDCHECKPOINT;

	LOG->Trace("processed, signalling");

	/* Start the decoding thread again. */
	SDL_SemPost(buffer_finished);
}

void PrintCodecError(HRESULT hr, CString s)
{
	/* Actually, we might need XviD; we might want to look
	 * at the file and try to figure out if it's something
	 * common: DIV3, DIV4, DIV5, XVID, or maybe even MPEG2. */
	CString err = hr_ssprintf(hr, "%s", s.c_str());
	LOG->Warn( 
		ssprintf(
		"There was an error initializing a movie: %s.\n"
		"Could not locate the DivX video codec.\n"
		"DivX is required to movie textures and must\n"
		"be installed before running the application.\n\n"
		"Please visit http://www.divx.com to download the latest version.",
		err.c_str())
		);
}

void MovieTexture_DShow::Create()
{
	RageTextureID actualID = GetID();

    HRESULT hr;
    
	actualID.iAlphaBits = 0;

    if( FAILED( hr=CoInitialize(NULL) ) )
        RageException::Throw( hr_ssprintf(hr, "Could not CoInitialize") );

    // Create the filter graph
    if( FAILED( hr=m_pGB.CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC) ) )
        RageException::Throw( hr_ssprintf(hr, "Could not create CLSID_FilterGraph!") );
    
    // Create the Texture Renderer object
    CTextureRenderer *pCTR = new CTextureRenderer;
    
    /* Get a pointer to the IBaseFilter on the TextureRenderer, and add it to the
	 * graph.  When m_pGB is released, it will free pFTR. */
    CComPtr<IBaseFilter> pFTR = pCTR;
    if( FAILED( hr = m_pGB->AddFilter(pFTR, L"TEXTURERENDERER" ) ) )
        RageException::Throw( hr_ssprintf(hr, "Could not add renderer filter to graph!") );

    // Add the source filter
    CComPtr<IBaseFilter>    pFSrc;          // Source Filter
    WCHAR wFileName[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, actualID.filename.c_str(), -1, wFileName, MAX_PATH);

	// if this fails, it's probably because the user doesn't have DivX installed
    if( FAILED( hr = m_pGB->AddSourceFilter( wFileName, L"SOURCE", &pFSrc ) ) )
	{
		PrintCodecError(hr, "Could not create source filter to graph!");
		return;	// survive and don't Run the graph.
	}
    
    // Find the source's output and the renderer's input
    CComPtr<IPin>           pFTRPinIn;      // Texture Renderer Input Pin
    if( FAILED( hr = pFTR->FindPin( L"In", &pFTRPinIn ) ) )
        RageException::Throw( hr_ssprintf(hr, "Could not find input pin!") );

    CComPtr<IPin>           pFSrcPinOut;    // Source Filter Output Pin   
    if( FAILED( hr = pFSrc->FindPin( L"Output", &pFSrcPinOut ) ) )
        RageException::Throw( hr_ssprintf(hr, "Could not find output pin!") );
    
    // Connect these two filters
    if( FAILED( hr = m_pGB->Connect( pFSrcPinOut, pFTRPinIn ) ) )
	{
 		PrintCodecError(hr, "Could not connect pins!");
		return;	// survive and don't Run the graph.
	}

	// Pass us to our TextureRenderer.
	pCTR->SetRenderTarget(this);

	/* Cap the max texture size to the hardware max. */
	actualID.iMaxSize = min( actualID.iMaxSize, DISPLAY->GetMaxTextureSize() );

    // The graph is built, now get the set the output video width and height.
	// The source and image width will always be the same since we can't scale the video
	m_iSourceWidth  = pCTR->GetVidWidth();
	m_iSourceHeight = pCTR->GetVidHeight();

	/* image size cannot exceed max size */
	m_iImageWidth = min( m_iSourceWidth, actualID.iMaxSize );
	m_iImageHeight = min( m_iSourceHeight, actualID.iMaxSize );

	/* Texture dimensions need to be a power of two; jump to the next. */
	m_iTextureWidth = power_of_two(m_iImageWidth);
	m_iTextureHeight = power_of_two(m_iImageHeight);

	/* We've set up the movie, so we know the dimensions we need.  Set
	 * up the texture. */
	CreateTexture();

	// Start the graph running
    if( !PlayMovie() )
        RageException::Throw( "Could not run the DirectShow graph." );
}


void MovieTexture_DShow::NewData(const char *data)
{
	ASSERT(data);
	
	/* Try to lock. */
	if(SDL_SemTryWait(buffer_lock))
	{
		/* The main thread is doing something uncommon, such as pausing.
		 * Drop this frame. */
		return;
	}

	buffer = data;

	SDL_SemWait(buffer_finished);

	ASSERT(buffer == NULL);

	SDL_SemPost(buffer_lock);
}


void MovieTexture_DShow::CreateTexture()
{
	if(m_uTexHandle)
		return;

	/* Have to free our frame holder becuase we might need a different 
	 * PixelFormat than before. */
	if( m_img )
		SDL_FreeSurface( m_img );

	/* My test clip (a high-res, MPEG1 video) goes from 12 fps to 14 fps
	 * if I use a 16-bit internalformat instead of a 32-bit one; that's a
	 * 16% jump, which is significant.  (Simply decoding this video is probably
	 * taking 30-40% CPU.)  It means much less bus traffic (sending textures to
	 * the card is slow).
	 *
	 * So, it *might* make sense to make this separately configurable.  However,
	 * that's getting pretty detailed; well beyond what most users will tweak.  
	 * Some way to figure this out dynamically would be nice, but it's probably
	 * impossible.  (For example, 24-bit textures may even be cheaper on pure
	 * AGP cards; 16-bit requires a conversion.) */
	m_PixelFormat = FMT_RGB8;
	if(TEXTUREMAN->GetTextureColorDepth() == 16)
		m_PixelFormat = FMT_RGB5;

	m_img = SDL_CreateRGBSurfaceSane(SDL_SWSURFACE, m_iTextureWidth, m_iTextureHeight, PIXEL_FORMAT_DESC[m_PixelFormat].bpp,
		PIXEL_FORMAT_DESC[m_PixelFormat].masks[0], PIXEL_FORMAT_DESC[m_PixelFormat].masks[1],
		PIXEL_FORMAT_DESC[m_PixelFormat].masks[2], PIXEL_FORMAT_DESC[m_PixelFormat].masks[3]);

	m_uTexHandle = DISPLAY->CreateTexture( m_PixelFormat, m_img );
}

bool MovieTexture_DShow::PlayMovie()
{
	SkipUpdates();

	LOG->Trace("MovieTexture_DShow::PlayMovie()");
	CComPtr<IMediaControl> pMC;
    m_pGB.QueryInterface(&pMC);

    // Start the graph running;
	HRESULT hr;
    if( FAILED( hr = pMC->Run() ) )
        RageException::Throw( hr_ssprintf(hr, "Could not run the DirectShow graph.") );

	m_bPlaying = true;

	StopSkippingUpdates();

	return true;
}


void MovieTexture_DShow::Play()
{
	PlayMovie();
}

void MovieTexture_DShow::Pause()
{
	SkipUpdates();

	CComPtr<IMediaControl> pMC;
    m_pGB.QueryInterface(&pMC);

	HRESULT hr;
	if( FAILED( hr = pMC->Pause() ) )
        RageException::Throw( hr_ssprintf(hr, "Could not pause the DirectShow graph.") );

	StopSkippingUpdates();
}

void MovieTexture_DShow::Stop()
{
	SkipUpdates();

	CComPtr<IMediaControl> pMC;
    m_pGB.QueryInterface(&pMC);

	HRESULT hr;
	if( FAILED( hr = pMC->Stop() ) )
        RageException::Throw( hr_ssprintf(hr, "Could not stop the DirectShow graph.") );

	m_bPlaying = false;

	StopSkippingUpdates();
}

void MovieTexture_DShow::SetPosition( float fSeconds )
{
	SkipUpdates();

	CComPtr<IMediaPosition> pMP;
    m_pGB.QueryInterface(&pMP);
    pMP->put_CurrentPosition(0);

	StopSkippingUpdates();
}

void MovieTexture_DShow::SetPlaybackRate( float fRate )
{
	SkipUpdates();

	CComPtr<IMediaPosition> pMP;
    m_pGB.QueryInterface(&pMP);
    pMP->put_Rate(fRate);

	StopSkippingUpdates();
}

bool MovieTexture_DShow::IsPlaying() const
{
	return m_bPlaying;
}



