#include "instructor_locator.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

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
	m_flCompassSin = 0.0f;
	m_flCompassCos = 1.0f;
	m_bCompassDirectionValid = false;
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
	m_iEffectAlpha = 255;
	m_flFadeDuration = 0.3f;
	m_flPulse = 0.0f;
	m_flPulseScale = 1.0f;
	m_flShakeX = m_flShakeY = 0.0f;
	m_bRemoveWhenFadedOut = false;
	m_bManagedDisplayCounted = false;
	m_flEndTime = 0.0f;
	m_hOnscreenSprite = m_hOffscreenSprite = 0;
	m_wide = m_tall = 40;
	m_iEdgeMargin = 56;
	m_declutterIndex = m_lastDeclutterIndex = -1;
	m_offsetX = m_offsetY = 0;
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

	m_bVisible = false;
	m_bRemoveWhenFadedOut = true;
	if( !( m_iEffectsFlags & LOCATOR_ICON_FX_FADE_OUT ) )
	{
		if( fadeTime < 0.001f ) fadeTime = 0.001f;
		m_flFadeDuration = fadeTime;
		const float elapsedFade = ( 1.0f - (float)m_alpha / 255.0f ) * fadeTime;
		m_fadeStart = currentTime - elapsedFade;
		m_iEffectsFlags |= LOCATOR_ICON_FX_FADE_OUT;
		m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_IN;
	}
}

void CInstructorLocatorTarget::Update( int frameNumber ) { m_frameLastUpdated = frameNumber; }

bool CInstructorLocatorTarget::IsFadingOut() const
{
	return ( m_iEffectsFlags & LOCATOR_ICON_FX_FADE_OUT ) != 0;
}

bool CInstructorLocatorTarget::ShouldDraw() const
{
	return m_isActive && m_alpha > 0 && ( m_bVisible || IsFadingOut() );
}

int CInstructorLocatorTarget::GetRenderAlpha( int baseAlpha ) const
{
	const int alphaFlags = m_iEffectsFlags & 0x00000038;
	int alpha = baseAlpha;
	if( alphaFlags )
		alpha = alpha * m_iEffectAlpha / 255;
	if( alpha < 0 ) alpha = 0;
	if( alpha > 255 ) alpha = 255;
	alpha = alpha * m_alpha / 255;
	if( alpha < 0 ) alpha = 0;
	if( alpha > 255 ) alpha = 255;
	return alpha;
}

void CInstructorLocatorTarget::UpdateAnimation( float currentTime )
{
	m_bIsDrawing = false;

	float fadeDuration = m_flFadeDuration;
	if( fadeDuration < 0.001f ) fadeDuration = 0.001f;
	float elapsed = currentTime - m_fadeStart;
	if( elapsed < 0.0f ) elapsed = 0.0f;

	if( m_iEffectsFlags & LOCATOR_ICON_FX_FADE_OUT )
	{
		float fraction = elapsed / fadeDuration;
		if( fraction > 1.0f ) fraction = 1.0f;
		m_alpha = (int)( 255.0f * ( 1.0f - fraction ) + 0.5f );
		if( fraction >= 1.0f )
		{
			m_alpha = 0;
			m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_OUT;
			if( m_bRemoveWhenFadedOut )
			{
				ResetState();
				return;
			}
		}
	}
	else if( m_iEffectsFlags & LOCATOR_ICON_FX_FADE_IN )
	{
		float fraction = elapsed / fadeDuration;
		if( fraction > 1.0f ) fraction = 1.0f;
		m_alpha = (int)( 255.0f * fraction + 0.5f );
		if( fraction >= 1.0f )
		{
			m_alpha = 255;
			m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_IN;
		}
	}
	else
	{
		m_alpha = m_bVisible ? 255 : 0;
	}

	// Source CLocatorPanel::AnimateIconSize defaults: 1.0..1.75 and a
	// one-second activation pulse, even when no persistent pulse flag is set.
	const int pulseFlags = m_iEffectsFlags & 0x00000007;
	const float pulseElapsed = currentTime - m_pulseStart;
	if( ( pulseFlags & LOCATOR_ICON_FX_PULSE_SLOW ) || pulseElapsed < 1.0f )
		m_flPulse = fabsf( sinf( pulseElapsed * 3.14159265358979323846f ) );
	else if( pulseFlags & LOCATOR_ICON_FX_PULSE_FAST )
		m_flPulse = fabsf( sinf( currentTime * 2.0f * 3.14159265358979323846f ) );
	else if( pulseFlags & LOCATOR_ICON_FX_PULSE_URGENT )
		m_flPulse = fabsf( sinf( currentTime * 4.0f * 3.14159265358979323846f ) );
	else
		m_flPulse = 0.0f;
	m_flPulseScale = 1.0f + 0.75f * m_flPulse;

	// Source MIN_ICON_ALPHA=0.5 / MAX_ICON_ALPHA=1.0, with absolute-time
	// oscillation frequencies of 3, 7, and 10 radians per second.
	const int alphaFlags = m_iEffectsFlags & 0x00000038;
	float alphaScale = 1.0f;
	if( alphaFlags & LOCATOR_ICON_FX_ALPHA_SLOW )
		alphaScale = 0.5f + 0.5f * fabsf( sinf( currentTime * 3.0f ) );
	else if( alphaFlags & LOCATOR_ICON_FX_ALPHA_FAST )
		alphaScale = 0.5f + 0.5f * fabsf( sinf( currentTime * 7.0f ) );
	else if( alphaFlags & LOCATOR_ICON_FX_ALPHA_URGENT )
		alphaScale = 0.5f + 0.5f * fabsf( sinf( currentTime * 10.0f ) );
	m_iEffectAlpha = (int)( 255.0f * alphaScale );

	// Source uses a fresh integer offset per presentation update, not a
	// smooth sinusoidal vibration.
	const int shakeFlags = m_iEffectsFlags & 0x000000C0;
	if( shakeFlags & LOCATOR_ICON_FX_SHAKE_NARROW )
	{
		m_flShakeX = (float)( rand() % 5 - 2 );
		m_flShakeY = (float)( rand() % 5 - 2 );
	}
	else if( shakeFlags & LOCATOR_ICON_FX_SHAKE_WIDE )
	{
		m_flShakeX = (float)( rand() % 11 - 5 );
		m_flShakeY = (float)( rand() % 11 - 5 );
	}
	else
	{
		m_flShakeX = m_flShakeY = 0.0f;
	}
}

void CInstructorLocatorTarget::UpdateProjection( float screenX, float screenY, bool behind,
	int screenWidth, int screenHeight, float visibilityMargin, float currentTime )
{
	const bool wasOnscreen = m_bOnscreen;
	m_flProjectedX = screenX;
	m_flProjectedY = screenY;
	m_bBehindCamera = behind;
	m_flVisibilityMargin = visibilityMargin;
	m_targetX = (int)( screenX >= 0.0f ? screenX + 0.5f : screenX - 0.5f );
	m_targetY = (int)( screenY >= 0.0f ? screenY + 0.5f : screenY - 0.5f );
	m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_NONE;

	if( screenWidth <= 0 || screenHeight <= 0 )
	{
		m_bOnscreen = false;
		if( wasOnscreen != m_bOnscreen ) m_pulseStart = currentTime;
		return;
	}

	if( visibilityMargin < 0.0f ) visibilityMargin = 0.0f;
	if( visibilityMargin * 2.0f > (float)screenWidth ) visibilityMargin = (float)screenWidth * 0.5f;
	if( visibilityMargin * 2.0f > (float)screenHeight ) visibilityMargin = (float)screenHeight * 0.5f;

	m_bOnscreen = !behind &&
		screenX >= visibilityMargin &&
		screenX <= (float)screenWidth - visibilityMargin &&
		screenY >= visibilityMargin &&
		screenY <= (float)screenHeight - visibilityMargin;

	// Source restarts the one-second presentation pulse on on/offscreen transitions.
	if( wasOnscreen != m_bOnscreen ) m_pulseStart = currentTime;
}

void CInstructorLocatorTarget::CalculateEdgePosition( int screenWidth, int screenHeight, int edgeMargin )
{
	if( screenWidth <= 0 || screenHeight <= 0 ) return;
	m_centerX = screenWidth / 2;
	m_centerY = screenHeight / 2;
	m_iconX = m_targetX;
	m_iconY = m_targetY;
	m_bIsDrawing = ShouldDraw();
	m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_NONE;

	if( edgeMargin < 0 ) edgeMargin = 0;
	int horizontalMargin = edgeMargin, verticalMargin = edgeMargin;
	if( horizontalMargin * 2 > screenWidth ) horizontalMargin = screenWidth / 2;
	if( verticalMargin * 2 > screenHeight ) verticalMargin = screenHeight / 2;

	if( !m_bOnscreen )
	{
		const float dx = m_flProjectedX - (float)m_centerX;
		const float dy = m_flProjectedY - (float)m_centerY;
		const float safe = m_flVisibilityMargin;

		// Source positions offscreen locators on an ellipse around screen center:
		// YRES(100) radius and 0.6 vertical squash.
		float radius = (float)screenHeight * ( 100.0f / 480.0f );
		float maxRadiusX = (float)m_centerX - (float)horizontalMargin;
		float maxRadiusY = ( (float)m_centerY - (float)verticalMargin ) / 0.6f;
		if( maxRadiusX < 0.0f ) maxRadiusX = 0.0f;
		if( maxRadiusY < 0.0f ) maxRadiusY = 0.0f;
		if( radius > maxRadiusX ) radius = maxRadiusX;
		if( radius > maxRadiusY ) radius = maxRadiusY;

		if( m_bCompassDirectionValid )
		{
			// Same horizontal bearing formula as Source GetTargetPosition().
			m_iconX = (int)( (float)m_centerX + radius * m_flCompassSin );
			m_iconY = (int)( (float)m_centerY - radius * 0.6f * m_flCompassCos );
		}
		else
		{
			// Fallback for engines without a usable view-angle callback.
			const float direction = atan2f( dx, -dy );
			m_iconX = (int)( (float)m_centerX + radius * sinf( direction ) );
			m_iconY = (int)( (float)m_centerY - radius * 0.6f * cosf( direction ) );
		}

		// Source tests horizontal title-safe bounds before vertical bounds.
		if( m_flProjectedX < safe )
			m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_LEFT;
		else if( m_flProjectedX > (float)screenWidth - safe )
			m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_RIGHT;
		else if( m_flProjectedY < safe )
			m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_UP;
		else if( m_flProjectedY > (float)screenHeight - safe )
			m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_DOWN;
		else if( m_bBehindCamera )
		{
			if( fabsf( dx ) * (float)screenHeight >= fabsf( dy ) * (float)screenWidth )
				m_drawArrowDirection = dx < 0.0f ?
					INSTRUCTOR_LOCATOR_ARROW_LEFT : INSTRUCTOR_LOCATOR_ARROW_RIGHT;
			else
				m_drawArrowDirection = dy < 0.0f ?
					INSTRUCTOR_LOCATOR_ARROW_UP : INSTRUCTOR_LOCATOR_ARROW_DOWN;
		}
	}
	if( m_iconX < horizontalMargin ) m_iconX = horizontalMargin;
	if( m_iconX > screenWidth - horizontalMargin ) m_iconX = screenWidth - horizontalMargin;
	if( m_iconY < verticalMargin ) m_iconY = verticalMargin;
	if( m_iconY > screenHeight - verticalMargin ) m_iconY = screenHeight - verticalMargin;
}

void CInstructorLocatorTarget::SetCompassDirection( float sinYaw, float cosYaw, bool valid )
{
	m_flCompassSin = sinYaw;
	m_flCompassCos = cosYaw;
	m_bCompassDirectionValid = valid;
}

void CInstructorLocatorTarget::SetVisible( bool visible, float currentTime, float fadeTime )
{
	if( m_bVisible == visible ) return;
	m_bVisible = visible;
	m_bRemoveWhenFadedOut = false;
	if( fadeTime < 0.001f ) fadeTime = 0.001f;
	m_flFadeDuration = fadeTime;

	if( visible )
	{
		m_fadeStart = currentTime - ( (float)m_alpha / 255.0f ) * fadeTime;
		m_iEffectsFlags |= LOCATOR_ICON_FX_FADE_IN;
		m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_OUT;
	}
	else if( m_alpha > 0 )
	{
		m_fadeStart = currentTime - ( 1.0f - (float)m_alpha / 255.0f ) * fadeTime;
		m_iEffectsFlags |= LOCATOR_ICON_FX_FADE_OUT;
		m_iEffectsFlags &= ~LOCATOR_ICON_FX_FADE_IN;
	}
	else
	{
		m_alpha = 0;
		m_iEffectsFlags &= ~( LOCATOR_ICON_FX_FADE_IN | LOCATOR_ICON_FX_FADE_OUT );
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
void CInstructorLocatorTarget::SetOnscreenIconName( const char *iconName, float currentTime )
{
	if( strcmp( m_szOnscreenIcon, iconName ? iconName : "" ) != 0 )
	{
		CopyLocatorString( m_szOnscreenIcon, sizeof( m_szOnscreenIcon ), iconName );
		m_pulseStart = currentTime;
	}
}
void CInstructorLocatorTarget::SetOffscreenIconName( const char *iconName, float currentTime )
{
	if( strcmp( m_szOffscreenIcon, iconName ? iconName : "" ) != 0 )
	{
		CopyLocatorString( m_szOffscreenIcon, sizeof( m_szOffscreenIcon ), iconName );
		m_pulseStart = currentTime;
	}
}
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
		// Managed lesson targets are refreshed every HUD frame. Map-authored
		// hints are message-driven and may intentionally remain static forever;
		// their lifetime is controlled by explicit end messages or timeouts.
		if( target.m_isActive && target.m_bManaged &&
			m_iFrameCount - target.m_frameLastUpdated > INSTRUCTOR_LOCATOR_STALE_FRAMES )
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
void CInstructorLocatorPanel::UpdatePresentation( float currentTime )
{
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		if( m_targets[i].m_isActive )
			m_targets[i].UpdateAnimation( currentTime );
	}
}

void CInstructorLocatorPanel::ResolveDecluttering( int screenWidth, int screenHeight,
	int edgeMargin, int gap )
{
	if( screenWidth <= 0 || screenHeight <= 0 )
		return;
	if( edgeMargin < 0 ) edgeMargin = 0;
	if( gap < 0 ) gap = 0;

	// Higher-priority managed lessons claim the clearest positions first.
	// Ties remain deterministic: older target handles are laid out first.
	int order[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
		order[i] = i;
	for( int i = 1; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		const int key = order[i];
		const CInstructorLocatorTarget &candidate = m_targets[key];
		int j = i - 1;
		while( j >= 0 )
		{
			const CInstructorLocatorTarget &previous = m_targets[order[j]];
			const bool candidateBefore = candidate.m_isActive &&
				( !previous.m_isActive ||
				  candidate.m_iPriority > previous.m_iPriority ||
				  ( candidate.m_iPriority == previous.m_iPriority &&
				    candidate.m_serialNumber < previous.m_serialNumber ) );
			if( !candidateBefore )
				break;
			order[j + 1] = order[j];
			--j;
		}
		order[j + 1] = key;
	}

	int placedX[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedY[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedW[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedH[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedCount = 0;

	for( int orderIndex = 0; orderIndex < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++orderIndex )
	{
		CInstructorLocatorTarget &target = m_targets[order[orderIndex]];
		target.m_offsetX = target.m_offsetY = 0;
		target.m_declutterIndex = -1;
		if( !target.m_isActive || !target.m_bIsDrawing || !target.ShouldDraw() )
			continue;

		const int width = target.m_wide > 0 ? target.m_wide : 40;
		const int height = target.m_tall > 0 ? target.m_tall : 40;
		const int stepX = width + gap;
		const int stepY = height + gap;
		const int candidateX[17] =
			{ 0, 0, 0, -1, 1, -1, 1, -1, 1, 0, 0, -2, 2, -2, 2, -2, 2 };
		const int candidateY[17] =
			{ 0, -1, 1, 0, 0, -1, -1, 1, 1, -2, 2, 0, 0, -2, -2, 2, 2 };
		const int preferred = target.m_lastDeclutterIndex >= 0 &&
			target.m_lastDeclutterIndex < 17 ? target.m_lastDeclutterIndex : 0;

		int chosenX = target.m_iconX;
		int chosenY = target.m_iconY;
		int chosenIndex = preferred;
		int bestOverlap = 0x7fffffff;
		int horizontalMargin = edgeMargin;
		int verticalMargin = edgeMargin;
		if( horizontalMargin * 2 > screenWidth ) horizontalMargin = screenWidth / 2;
		if( verticalMargin * 2 > screenHeight ) verticalMargin = screenHeight / 2;
		int halfWidth = width / 2;
		int halfHeight = height / 2;
		if( halfWidth > screenWidth / 2 - horizontalMargin )
			halfWidth = screenWidth / 2 - horizontalMargin;
		if( halfHeight > screenHeight / 2 - verticalMargin )
			halfHeight = screenHeight / 2 - verticalMargin;
		const int minX = horizontalMargin + halfWidth;
		const int maxX = screenWidth - horizontalMargin - halfWidth;
		const int minY = verticalMargin + halfHeight;
		const int maxY = screenHeight - verticalMargin - halfHeight;

		for( int candidate = 0; candidate < 17; ++candidate )
		{
			const int candidateIndex = ( preferred + candidate ) % 17;
			int x = target.m_iconX + candidateX[candidateIndex] * stepX;
			int y = target.m_iconY + candidateY[candidateIndex] * stepY;
			if( x < minX ) x = minX;
			if( x > maxX ) x = maxX;
			if( y < minY ) y = minY;
			if( y > maxY ) y = maxY;

			int overlapScore = 0;
			for( int p = 0; p < placedCount; ++p )
			{
				const int left = x - width / 2 > placedX[p] - placedW[p] / 2 ?
					x - width / 2 : placedX[p] - placedW[p] / 2;
				const int right = x + width / 2 < placedX[p] + placedW[p] / 2 ?
					x + width / 2 : placedX[p] + placedW[p] / 2;
				const int top = y - height / 2 > placedY[p] - placedH[p] / 2 ?
					y - height / 2 : placedY[p] - placedH[p] / 2;
				const int bottom = y + height / 2 < placedY[p] + placedH[p] / 2 ?
					y + height / 2 : placedY[p] + placedH[p] / 2;
				if( right > left && bottom > top )
					overlapScore += ( right - left ) * ( bottom - top );
			}

			if( overlapScore < bestOverlap )
			{
				bestOverlap = overlapScore;
				chosenX = x;
				chosenY = y;
				chosenIndex = candidateIndex;
			}
			if( overlapScore == 0 )
				break;
		}

		target.m_offsetX = chosenX - target.m_iconX;
		target.m_offsetY = chosenY - target.m_iconY;
		target.m_declutterIndex = chosenIndex;
		target.m_lastDeclutterIndex = chosenIndex;
		placedX[placedCount] = chosenX;
		placedY[placedCount] = chosenY;
		placedW[placedCount] = width;
		placedH[placedCount] = height;
		++placedCount;
	}
}

int CInstructorLocatorPanel::GetActiveTargetCount() const
{
	int count = 0;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i ) if( m_targets[i].m_isActive ) ++count;
	return count;
}
