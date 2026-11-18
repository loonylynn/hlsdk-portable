#include "instructor_locator.h"
#include <string.h>
#include <math.h>

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
	m_iEffectAlpha = 255;
	m_flFadeDuration = 0.18f;
	m_flPulse = 0.0f;
	m_flPulseScale = 1.0f;
	m_flShakeX = m_flShakeY = 0.0f;
	m_flManagedAlpha = 1.0f;
	m_bRemoveWhenFadedOut = false;
	m_wide = m_tall = 40;
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
	int alpha = alphaFlags ? m_iEffectAlpha : baseAlpha;
	if( alpha < 0 ) alpha = 0;
	if( alpha > 255 ) alpha = 255;
	alpha = alpha * m_alpha / 255;
	alpha = (int)( (float)alpha * m_flManagedAlpha + 0.5f );
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

	const int pulseMode = m_iEffectsFlags & 0x00000007;
	const float pulseRate = ( pulseMode & 0x4 ) ? 5.0f :
		( ( pulseMode & 0x2 ) ? 2.5f : 1.0f );
	m_flPulse = pulseMode ?
		( 0.5f + 0.5f * sinf( ( currentTime - m_pulseStart ) * pulseRate * 6.28318f ) ) : 0.0f;
	m_flPulseScale = 1.0f + 0.08f * m_flPulse;

	const int alphaFlags = m_iEffectsFlags & 0x00000038;
	if( alphaFlags )
	{
		const float alphaRate = ( alphaFlags & 0x20 ) ? 5.0f :
			( ( alphaFlags & 0x10 ) ? 2.5f : 1.0f );
		const float alphaPulse = 0.5f + 0.5f *
			sinf( ( currentTime - m_pulseStart ) * alphaRate * 6.28318f );
		m_iEffectAlpha = 150 + (int)( 105.0f * alphaPulse );
	}
	else
	{
		m_iEffectAlpha = 255;
	}

	const int shakeFlags = m_iEffectsFlags & 0x000000C0;
	if( shakeFlags )
	{
		const float shakeRate = ( shakeFlags & 0x80 ) ? 22.0f : 10.0f;
		const float shakeTime = ( currentTime - m_pulseStart ) * shakeRate;
		m_flShakeX = sinf( shakeTime ) * 2.0f;
		m_flShakeY = cosf( shakeTime * 0.83f ) * 1.5f;
	}
	else
	{
		m_flShakeX = m_flShakeY = 0.0f;
	}

	m_flManagedAlpha = 1.0f;
	if( m_bManaged && m_flVisibleSince > 0.0f )
	{
		float fade = ( currentTime - m_flVisibleSince ) / 0.18f;
		if( fade < 0.0f ) fade = 0.0f;
		if( fade > 1.0f ) fade = 1.0f;
		m_flManagedAlpha = 0.25f + 0.75f * fade;
	}
}

void CInstructorLocatorTarget::UpdateProjection( float screenX, float screenY, bool behind,
	int screenWidth, int screenHeight, int visibilityMargin )
{
	m_flProjectedX = screenX;
	m_flProjectedY = screenY;
	m_bBehindCamera = behind;
	m_targetX = (int)( screenX >= 0.0f ? screenX + 0.5f : screenX - 0.5f );
	m_targetY = (int)( screenY >= 0.0f ? screenY + 0.5f : screenY - 0.5f );
	m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_NONE;

	if( screenWidth <= 0 || screenHeight <= 0 )
	{
		m_bOnscreen = false;
		return;
	}

	if( visibilityMargin < 0 ) visibilityMargin = 0;
	if( visibilityMargin * 2 > screenWidth ) visibilityMargin = screenWidth / 2;
	if( visibilityMargin * 2 > screenHeight ) visibilityMargin = screenHeight / 2;

	m_bOnscreen = !behind &&
		screenX >= (float)visibilityMargin &&
		screenX <= (float)( screenWidth - visibilityMargin ) &&
		screenY >= (float)visibilityMargin &&
		screenY <= (float)( screenHeight - visibilityMargin );
}

void CInstructorLocatorTarget::CalculateEdgePosition( int screenWidth, int screenHeight, int edgeMargin )
{
	if( screenWidth <= 0 || screenHeight <= 0 )
		return;

	m_centerX = screenWidth / 2;
	m_centerY = screenHeight / 2;
	m_iconX = m_targetX;
	m_bIsDrawing = ShouldDraw();
	m_iconY = m_targetY;
	m_drawArrowDirection = INSTRUCTOR_LOCATOR_ARROW_NONE;

	if( edgeMargin < 0 ) edgeMargin = 0;
	int horizontalMargin = edgeMargin;
	int verticalMargin = edgeMargin;
	if( horizontalMargin * 2 > screenWidth ) horizontalMargin = screenWidth / 2;
	if( verticalMargin * 2 > screenHeight ) verticalMargin = screenHeight / 2;

	if( !m_bOnscreen )
	{
		const float dx = m_flProjectedX - (float)m_centerX;
		float dy = m_flProjectedY - (float)m_centerY;
		if( fabsf( dx ) < 1.0f && fabsf( dy ) < 1.0f )
			dy = 1.0f;

		const float left = (float)horizontalMargin;
		const float right = (float)( screenWidth - horizontalMargin );
		const float top = (float)verticalMargin;
		const float bottom = (float)( screenHeight - verticalMargin );
		float tx = 1000000.0f;
		float ty = 1000000.0f;

		if( dx > 0.001f )
			tx = ( right - (float)m_centerX ) / dx;
		else if( dx < -0.001f )
			tx = ( left - (float)m_centerX ) / dx;

		if( dy > 0.001f )
			ty = ( bottom - (float)m_centerY ) / dy;
		else if( dy < -0.001f )
			ty = ( top - (float)m_centerY ) / dy;

		float t = tx < ty ? tx : ty;
		if( t < 0.0f || t > 1000000.0f )
			t = 0.0f;

		m_iconX = (int)( (float)m_centerX + dx * t );
		m_iconY = (int)( (float)m_centerY + dy * t );

		if( tx <= ty )
			m_drawArrowDirection = dx < 0.0f ?
				INSTRUCTOR_LOCATOR_ARROW_LEFT : INSTRUCTOR_LOCATOR_ARROW_RIGHT;
		else
			m_drawArrowDirection = dy < 0.0f ?
				INSTRUCTOR_LOCATOR_ARROW_UP : INSTRUCTOR_LOCATOR_ARROW_DOWN;
	}

	if( m_iconX < horizontalMargin ) m_iconX = horizontalMargin;
	if( m_iconX > screenWidth - horizontalMargin ) m_iconX = screenWidth - horizontalMargin;
	if( m_iconY < verticalMargin ) m_iconY = verticalMargin;
	if( m_iconY > screenHeight - verticalMargin ) m_iconY = screenHeight - verticalMargin;
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

	int placedX[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedY[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedW[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedH[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int placedCount = 0;

	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget &target = m_targets[i];
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
		const int minX = horizontalMargin;
		const int maxX = screenWidth - horizontalMargin;
		const int minY = verticalMargin;
		const int maxY = screenHeight - verticalMargin;

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
