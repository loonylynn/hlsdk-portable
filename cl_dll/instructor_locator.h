/*
 * Source-style Game Instructor locator target/panel model.
 * GoldSrc-native adaptation of CLocatorTarget / CLocatorPanel responsibilities.
 * VGUI2 painting and Source-only handles are intentionally not dependencies.
 */
#ifndef INSTRUCTOR_LOCATOR_H
#define INSTRUCTOR_LOCATOR_H

#define MAX_INSTRUCTOR_LOCATOR_TARGETS 10
#define MAX_INSTRUCTOR_LOCATOR_BINDINGS 8
#define MAX_INSTRUCTOR_LOCATOR_CAPTION 192
#define MAX_INSTRUCTOR_LOCATOR_BINDING 64
#define MAX_INSTRUCTOR_LOCATOR_ICON 64

enum InstructorLocatorEffectFlags
{
	LOCATOR_ICON_FX_NONE             = 0x00000000,
	LOCATOR_ICON_FX_PULSE_SLOW       = 0x00000001,
	LOCATOR_ICON_FX_PULSE_FAST       = 0x00000002,
	LOCATOR_ICON_FX_PULSE_URGENT     = 0x00000004,
	LOCATOR_ICON_FX_ALPHA_SLOW       = 0x00000008,
	LOCATOR_ICON_FX_ALPHA_FAST       = 0x00000010,
	LOCATOR_ICON_FX_ALPHA_URGENT     = 0x00000020,
	LOCATOR_ICON_FX_SHAKE_NARROW     = 0x00000040,
	LOCATOR_ICON_FX_SHAKE_WIDE       = 0x00000080,
	LOCATOR_ICON_FX_STATIC           = 0x00000100,
	LOCATOR_ICON_FX_NO_OFFSCREEN     = 0x00000200,
	LOCATOR_ICON_FX_FORCE_CAPTION    = 0x00000400,
	LOCATOR_ICON_FX_FADE_OUT         = 0x00000800,
	LOCATOR_ICON_FX_FADE_IN          = 0x00001000
};

enum InstructorLocatorArrowDirection
{
	INSTRUCTOR_LOCATOR_ARROW_NONE = 0,
	INSTRUCTOR_LOCATOR_ARROW_LEFT,
	INSTRUCTOR_LOCATOR_ARROW_RIGHT,
	INSTRUCTOR_LOCATOR_ARROW_UP,
	INSTRUCTOR_LOCATOR_ARROW_DOWN
};

class CInstructorLocatorTarget
{
public:
	bool m_bOriginInScreenspace;
	float m_vecOrigin[3];
	int m_iEntityIndex;
	bool m_bFixedPosition;
	int m_iHintId;
	int m_iPriority; // managed lesson priority; map-authored hints default to zero
	int m_iIconMode;
	bool m_bManaged;
	float m_flVisibleSince;
	float m_flIconOffset;
	bool m_isActive;
	int m_serialNumber;
	int m_frameLastUpdated;
	bool m_bOnscreen;
	bool m_bBehindCamera;
	float m_flProjectedX, m_flProjectedY;
	bool m_bOccluded;
	bool m_bVisible;
	bool m_bIsDrawing;
	bool m_bRemoveWhenFadedOut;
	bool m_bManagedDisplayCounted;
	float m_flEndTime;
	int m_hOnscreenSprite, m_hOffscreenSprite;
	float m_flDistFromPlayer;
	int m_targetX, m_targetY;
	int m_iconX, m_iconY;
	int m_centerX, m_centerY;
	int m_wide, m_tall;
	float m_widthScaleOnscreen;
	int m_alpha;
	int m_iEffectAlpha;
	float m_fadeStart, m_lerpStart, m_pulseStart;
	float m_flFadeDuration;
	float m_flPulse, m_flPulseScale;
	float m_flShakeX, m_flShakeY, m_flManagedAlpha;
	int m_declutterIndex, m_lastDeclutterIndex, m_drawArrowDirection;
	int m_captionWide;
	bool m_bDrawControllerButton, m_bDrawControllerButtonOffscreen;
	int m_offsetX, m_offsetY;
	float m_flTimeLerpDone;
	int m_lastXPos, m_lastYPos;
	int m_iEffectsFlags;
	float m_flRange;
	int m_iCaptionColorR, m_iCaptionColorG, m_iCaptionColorB;
	int m_iBindingTick;
	float m_flNextBindingTick, m_flNextOcclusionTest;
	int m_iBindingChoicesCount;
	char m_szBindingChoices[MAX_INSTRUCTOR_LOCATOR_BINDINGS][MAX_INSTRUCTOR_LOCATOR_BINDING];
	char m_szCaption[MAX_INSTRUCTOR_LOCATOR_CAPTION];
	char m_szName[64], m_szReplaceKey[64];
	char m_szBinding[MAX_INSTRUCTOR_LOCATOR_BINDING];
	char m_szBindingCache[MAX_INSTRUCTOR_LOCATOR_BINDING];
	char m_szOnscreenIcon[MAX_INSTRUCTOR_LOCATOR_ICON];
	char m_szOffscreenIcon[MAX_INSTRUCTOR_LOCATOR_ICON];
	char m_szVguiTargetName[64], m_szVguiTargetLookup[64];
	int m_iVguiTargetEdge;

	CInstructorLocatorTarget();
	void Activate( int serialNumber, int frameNumber, float currentTime, int screenWidth, int screenHeight );
	void Deactivate( bool noFade, float currentTime, float fadeTime );
	void Update( int frameNumber );
	void UpdateAnimation( float currentTime );
	bool IsFadingOut() const;
	bool ShouldDraw() const;
	int GetRenderAlpha( int baseAlpha ) const;
	void UpdateProjection( float screenX, float screenY, bool behind,
		int screenWidth, int screenHeight, int visibilityMargin );
	void CalculateEdgePosition( int screenWidth, int screenHeight, int edgeMargin );
	void SetVisible( bool visible, float currentTime, float fadeTime );
	bool IsStatic() const;
	bool IsPresenting( float currentTime, float restTime ) const;
	void StartTimedLerp( float currentTime, float restTime );
	void StartPresent( float currentTime );
	void EndPresent( float currentTime, float restTime );
	int GetIconX( int targetOffsetX = 0 ) const;
	int GetIconY( int targetOffsetY = 0 ) const;
	int GetIconCenterX( int targetOffsetX = 0 ) const;
	int GetIconCenterY( int targetOffsetY = 0 ) const;
	void SetWorldOrigin( const float *origin, int entityIndex );
	void SetScreenOrigin( float x, float y );
	void SetCaptionText( const char *caption );
	void SetBinding( const char *binding );
	void SetOnscreenIconName( const char *iconName );
	void SetOffscreenIconName( const char *iconName );
	void SetCaptionColor( int red, int green, int blue );

private:
	void ResetState();
};

class CInstructorLocatorPanel
{
public:
	CInstructorLocatorPanel();
	int AddTarget( int screenWidth, int screenHeight, float currentTime );
	void RemoveTarget( int handle, float currentTime, float fadeTime );
	CInstructorLocatorTarget *GetTargetFromHandle( int handle );
	const CInstructorLocatorTarget *GetTargetFromHandle( int handle ) const;
	CInstructorLocatorTarget *GetTargetAt( int index );
	const CInstructorLocatorTarget *GetTargetAt( int index ) const;
	void DeactivateAllTargets( bool noFade, float currentTime, float fadeTime );
	void CollectGarbage( float currentTime, float fadeTime );
	void UpdatePresentation( float currentTime );
	void ResolveDecluttering( int screenWidth, int screenHeight, int edgeMargin, int gap );
	void BeginFrame( float currentTime );
	int GetFrameCount() const;
	int GetActiveTargetCount() const;

private:
	CInstructorLocatorTarget m_targets[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int m_iSerializer;
	int m_iFrameCount;
};

#endif
