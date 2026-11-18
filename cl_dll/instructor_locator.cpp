#include "instructor_locator.h"
#include <string.h>

namespace
{
	const int INSTRUCTOR_LOCATOR_STALE_FRAMES = 20;
	void CopyLocatorString( char *destination, int destinationSize, const char *source )
	{
		if( !destination || destinationSize <= 0 ) return;
		if( !source ) source = "";
		strncpy( destination, source, destinationSize - 1 );
		destination[destinationSize - 1] = '\0';
	}
}

CInstructorLocatorTarget::CInstructorLocatorTarget() { ResetState(); }

void CInstructorLocatorTarget::ResetState()
{
	memset( this, 0, sizeof( *this ) );
	m_serialNumber = -1;
	m_widthScaleOnscreen = 1.0f;
	m_iCaptionColorR = m_iCaptionColorG = m_iCaptionColorB = 255;
}

void CInstructorLocatorTarget::Activate( int serialNumber, int frameNumber,
	float currentTime, int screenWidth, int screenHeight )
{
	ResetState();
	m_serialNumber = serialNumber;
	m_frameLastUpdated = frameNumber;
	m_isActive = true;
	m_bVisible = m_bOnscreen = true;
	m_alpha = 0;
	m_fadeStart = m_lerpStart = m_pulseStart = currentTime;
	m_lastXPos = screenWidth / 2;
	m_lastYPos = screenHeight / 4;
	m_iEffectsFlags = LOCATOR_ICON_FX_FADE_IN;
}

void CInstructorLocatorTarget::Deactivate( bool noFade, float currentTime, float fadeTime )
{
	if( noFade || m_alpha <= 0 ||
		( m_bOccluded && !( m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) ) ||
		( !m_bOnscreen && ( m_iEffectsFlags & LOCATOR_ICON_FX_NO_OFFSCREEN ) ) )
	{
		ResetState();
		return;
	}
	if( !( m_iEffectsFlags & LOCATOR_ICON_FX_FADE_OUT ) )
	{
		if( fadeTime < 0.001f ) fadeTime = 0.001f;
		const float assumedFadeTime = ( 1.0f - (float)m_alpha / 255.0f ) * fadeTime;
		m_fadeStart = currentTime - assumedFadeTime;
		m_iEffectsFlags |= LOCATOR_ICON_FX_FADE_OUT;
		m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_IN;
	}
}

void CInstructorLocatorTarget::Update( int frameNumber ) { m_frameLastUpdated = frameNumber; }

void CInstructorLocatorTarget::SetVisible( bool visible, float currentTime, float fadeTime )
{
	if( m_bVisible == visible ) return;
	m_bVisible = visible;
	if( fadeTime < 0.001f ) fadeTime = 0.001f;
	if( visible )
	{
		m_fadeStart = currentTime - ( (float)m_alpha / 255.0f ) * fadeTime;
		m_iEffectsFlags |= LOCATOR_ICON_FX_FADE_IN;
		m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_OUT;
	}
	else
	{
		m_fadeStart = currentTime - ( 1.0f - (float)m_alpha / 255.0f ) * fadeTime;
		m_iEffectsFlags |= LOCATOR_ICON_FX_FADE_OUT;
		m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_IN;
	}
}

bool CInstructorLocatorTarget::IsStatic() const { return ( m_iEffectsFlags & LOCATOR_ICON_FX_STATIC ) != 0; }
bool CInstructorLocatorTarget::IsPresenting( float currentTime, float restTime ) const { return currentTime - m_lerpStart < restTime; }
void CInstructorLocatorTarget::StartTimedLerp( float currentTime, float restTime ) { if( currentTime - m_lerpStart > restTime ) m_lerpStart = currentTime - restTime; }
void CInstructorLocatorTarget::StartPresent( float currentTime ) { m_lerpStart = currentTime; }
void CInstructorLocatorTarget::EndPresent( float currentTime, float restTime ) { if( currentTime - m_lerpStart < restTime ) m_lerpStart = currentTime - restTime; }
int CInstructorLocatorTarget::GetIconX( int targetOffsetX ) const { return m_iconX + ( m_bOnscreen ? targetOffsetX + m_offsetX : 0 ); }
int CInstructorLocatorTarget::GetIconY( int targetOffsetY ) const { return m_iconY + ( m_bOnscreen ? targetOffsetY + m_offsetY : 0 ); }
int CInstructorLocatorTarget::GetIconCenterX( int targetOffsetX ) const { return m_centerX + targetOffsetX + m_offsetX; }
int CInstructorLocatorTarget::GetIconCenterY( int targetOffsetY ) const { return m_centerY + targetOffsetY + m_offsetY; }

void CInstructorLocatorTarget::SetWorldOrigin( const float *origin, int entityIndex )
{
	if( !origin ) return;
	m_bOriginInScreenspace = false;
	m_iEntityIndex = entityIndex;
	m_vecOrigin[0] = origin[0]; m_vecOrigin[1] = origin[1]; m_vecOrigin[2] = origin[2];
}
void CInstructorLocatorTarget::SetScreenOrigin( float x, float y )
{
	m_bOriginInScreenspace = true; m_iEntityIndex = 0;
	m_vecOrigin[0] = x; m_vecOrigin[1] = y; m_vecOrigin[2] = 0.0f;
}
void CInstructorLocatorTarget::SetCaptionText( const char *caption ) { CopyLocatorString( m_szCaption, sizeof( m_szCaption ), caption ); }
void CInstructorLocatorTarget::SetBinding( const char *binding ) { CopyLocatorString( m_szBinding, sizeof( m_szBinding ), binding ); }
void CInstructorLocatorTarget::SetOnscreenIconName( const char *iconName ) { CopyLocatorString( m_szOnscreenIcon, sizeof( m_szOnscreenIcon ), iconName ); }
void CInstructorLocatorTarget::SetOffscreenIconName( const char *iconName ) { CopyLocatorString( m_szOffscreenIcon, sizeof( m_szOffscreenIcon ), iconName ); }
void CInstructorLocatorTarget::SetCaptionColor( int red, int green, int blue )
{
	m_iCaptionColorR = red < 0 ? 0 : ( red > 255 ? 255 : red );
	m_iCaptionColorG = green < 0 ? 0 : ( green > 255 ? 255 : green );
	m_iCaptionColorB = blue < 0 ? 0 : ( blue > 255 ? 255 : blue );
}

CInstructorLocatorPanel::CInstructorLocatorPanel() : m_iSerializer( 1 ), m_iFrameCount( 0 )
{
	DeactivateAllTargets( true, 0.0f, 0.3f );
}

int CInstructorLocatorPanel::AddTarget( int screenWidth, int screenHeight, float currentTime )
{
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		if( !m_targets[i].m_isActive )
		{
			if( m_iSerializer <= 0 ) m_iSerializer = 1;
			m_targets[i].Activate( m_iSerializer, m_iFrameCount, currentTime, screenWidth, screenHeight );
			const int handle = m_iSerializer++;
			if( m_iSerializer <= 0 ) m_iSerializer = 1;
			return handle;
		}
	}
	return -1;
}

void CInstructorLocatorPanel::RemoveTarget( int handle, float currentTime, float fadeTime )
{
	CInstructorLocatorTarget *target = GetTargetFromHandle( handle );
	if( target ) target->Deactivate( false, currentTime, fadeTime );
}
CInstructorLocatorTarget *CInstructorLocatorPanel::GetTargetFromHandle( int handle )
{
	if( handle < 0 ) return NULL;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
		if( m_targets[i].m_isActive && m_targets[i].m_serialNumber == handle ) return &m_targets[i];
	return NULL;
}
const CInstructorLocatorTarget *CInstructorLocatorPanel::GetTargetFromHandle( int handle ) const
{
	if( handle < 0 ) return NULL;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
		if( m_targets[i].m_isActive && m_targets[i].m_serialNumber == handle ) return &m_targets[i];
	return NULL;
}
CInstructorLocatorTarget *CInstructorLocatorPanel::GetTargetAt( int index )
{
	return index >= 0 && index < MAX_INSTRUCTOR_LOCATOR_TARGETS ? &m_targets[index] : NULL;
}
const CInstructorLocatorTarget *CInstructorLocatorPanel::GetTargetAt( int index ) const
{
	return index >= 0 && index < MAX_INSTRUCTOR_LOCATOR_TARGETS ? &m_targets[index] : NULL;
}
void CInstructorLocatorPanel::DeactivateAllTargets( bool noFade, float currentTime, float fadeTime )
{
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i ) m_targets[i].Deactivate( noFade, currentTime, fadeTime );
}
void CInstructorLocatorPanel::CollectGarbage( float currentTime, float fadeTime )
{
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget &target = m_targets[i];
		if( target.m_isActive && m_iFrameCount - target.m_frameLastUpdated > INSTRUCTOR_LOCATOR_STALE_FRAMES )
			target.Deactivate( false, currentTime, fadeTime );
	}
}
void CInstructorLocatorPanel::BeginFrame( float currentTime )
{
	++m_iFrameCount;
	if( m_iFrameCount < 0 ) m_iFrameCount = 1;
	CollectGarbage( currentTime, 0.3f );
}
int CInstructorLocatorPanel::GetFrameCount() const { return m_iFrameCount; }
int CInstructorLocatorPanel::GetActiveTargetCount() const
{
	int count = 0;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i ) if( m_targets[i].m_isActive ) ++count;
	return count;
}
