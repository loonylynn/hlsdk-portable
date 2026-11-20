// GoldSrc adaptation of Valve's Source C_GameInstructor lesson manager.
// The public Source lesson lifecycle is retained; Source-only handles/events/UI
// are represented by GoldSrc entity indices, InstructorEvent usermessages and SPR HUD panels.
#ifndef GAME_INSTRUCTOR_H
#define GAME_INSTRUCTOR_H

#include <string>
#include <vector>
#include <utility>

class CBaseLesson;
class CTextLesson;
class CIconLesson;
class CScriptedIconLesson;
class C_GameInstructor;

struct InstructorEventData
{
	char name[64];
	int userId;
	int entityIndex;
	char className[64];
	char targetName[64];
	char fieldName[64];
	char stringValue[128];
	int integerValue;
	float floatValue;
};

struct InstructorLessonElement
{
	std::string expression;
	std::string parameter;
};

struct InstructorLessonEvent
{
	std::string name;
	std::vector<InstructorLessonElement> elements;
};

enum LessonInstanceType
{
	LESSON_INSTANCE_MULTIPLE = 0,
	LESSON_INSTANCE_SINGLE_OPEN = 1,
	LESSON_INSTANCE_FIXED_REPLACE = 2,
	LESSON_INSTANCE_SINGLE_ACTIVE = 3,
	LESSON_INSTANCE_TYPE_TOTAL = 4
};

class CBaseLesson
{
public:
	CBaseLesson(const char *name, bool definition);
	virtual ~CBaseLesson();

	bool IsLearned() const;
	bool PrerequisitesHaveBeenMet() const;
	bool IsTimedOut(float now);
	bool IsLocked(float now) const;
	bool IsInstructing() const { return m_bStarted; }
	bool IsOpenOpportunity() const { return !m_bDefinition; }
	bool IsDefaultHolder() const { return false; }
	bool WasDisplayed() const { return m_bWasDisplayed; }
	void MarkSucceeded();
	void MarkDisplayed();
	void SetCloseReason(const char *reason);

	std::string m_Name;
	std::string m_ReplaceKey;
	std::string m_Group;
	std::vector<std::string> m_Prerequisites;
	CBaseLesson *m_pRoot;

	LessonInstanceType m_InstanceType;
	int m_iPriority;
	int m_iFixedInstancesMax;
	bool m_bReplaceOnlyWhenStopped;
	int m_iDisplayLimit;
	int m_iSuccessLimit;
	int m_iDisplayCount;
	int m_iSuccessCount;
	float m_flTimeout;
	float m_flLockDuration;
	float m_flUpdateInterval;
	float m_flInitTime;
	float m_flStartTime;
	float m_flLockTime;
	bool m_bCanOpenWhenDead;
	bool m_bBumpWithTimeoutWhenLearned;
	bool m_bCanTimeoutWhileInactive;
	bool m_bDisabled;
	bool m_bWasDisplayed;
	bool m_bSuccessCounted;
	bool m_bCloseRequested;
	bool m_bStarted;
	std::string m_CloseReason;

protected:
	bool m_bDefinition;
};

class CTextLesson : public CBaseLesson
{
public:
	CTextLesson(const char *name, bool definition);
	std::string m_Caption;
	std::string m_CaptionParam;
	std::string m_Binding;
	std::string m_GamepadBinding;
};

class CIconLesson : public CTextLesson
{
public:
	CIconLesson(const char *name, bool definition);

	int m_iIconTarget;
	std::string m_OnscreenIcon;
	std::string m_OffscreenIcon;
	std::string m_CaptionColor;
	std::string m_VguiTargetName;
	std::string m_VguiTargetLookup;
	int m_iVguiTargetEdge;
	int m_iFlags;
	float m_flRange;
	float m_flIconOffset;
	float m_flRelativeIconOffset;
	bool m_bNoIconTarget;
	bool m_bAllowNodrawTarget;
	bool m_bVisible;
	bool m_bShowWhenOccluded;
	bool m_bNoOffscreen;
	bool m_bForceCaption;
	bool m_bFixedPosition;
	float m_flFixedX;
	float m_flFixedY;
	std::string m_StartSound;
};

class CScriptedIconLesson : public CIconLesson
{
public:
	CScriptedIconLesson(const char *name, bool definition);
	CScriptedIconLesson(const CScriptedIconLesson &definition);
	virtual ~CScriptedIconLesson();

	void InitFromKeys(const std::string &text);
	void InitPrerequisites();
	void OnOpen();
	void Update(float now);
	void UpdateInactive(float now);
	bool ValidateIconTarget(float now);
	void ProcessEvent(const InstructorEventData &event, int eventType);
	bool ProcessElements(const InstructorEventData *event, const std::vector<InstructorLessonElement> &elements);
	CScriptedIconLesson *CreateOpportunity(float now) const;

	std::vector<InstructorLessonEvent> m_OpenEvents;
	std::vector<InstructorLessonEvent> m_CloseEvents;
	std::vector<InstructorLessonEvent> m_SuccessEvents;
	std::vector<InstructorLessonEvent> m_OnOpenEvents;
	std::vector<InstructorLessonEvent> m_UpdateEvents;

	// Unique identity for this open opportunity (definitions keep zero).
	int m_iInstanceSerial;

	// Source LESSON_VARIABLE_FACTORY state, adapted to GoldSrc entity indices.
	int m_iLocalPlayer;
	int m_iEntity1;
	int m_iEntity2;
	int m_iInteger1;
	int m_iInteger2;
	int m_iIconTargetVariable;
	float m_flOutput;
	float m_flFloat1;
	float m_flFloat2;
	std::string m_String1;
	std::string m_String2;

private:
	const CScriptedIconLesson *m_pDefinition;
	float m_flNextUpdateEvent;
	float m_flNextTargetValidation;
};

class C_GameInstructor
{
public:
	C_GameInstructor();
	~C_GameInstructor();

	bool Init();
	void Shutdown();
	void Update(float now);
	void FireGameEvent(const InstructorEventData &event);
	void DefineLesson(CScriptedIconLesson *lesson);
	const CScriptedIconLesson *GetLesson(const char *name) const;
	bool IsLessonOfSameTypeOpen(const CScriptedIconLesson *lesson) const;
	bool OpenOpportunity(CScriptedIconLesson *lesson);
	void CloseOpportunity(CScriptedIconLesson *lesson, const char *reason);
	void CloseAllOpenOpportunities();
	void StopAllLessons();
	void MarkDisplayed(const char *name);
	void MarkSucceeded(const char *name);
	void MarkSucceeded(CScriptedIconLesson *lesson);
	void EvaluateLessonsForGameRules();
	void SetLessonGroupEnabled(const char *group, bool enabled);
	void DumpOpenOpportunities() const;

private:
	void ReadLessonsFromFile(const char *fileName);
	void InitLessonPrerequisites();
	void UpdateActiveLesson(CScriptedIconLesson *lesson);
	void UpdateInactiveLesson(CScriptedIconLesson *lesson, float now);
	void LoadCounts(CScriptedIconLesson *lesson);
	void SaveCounts(const CScriptedIconLesson *lesson);
	void DisplayLesson(const CScriptedIconLesson *lesson, float now);
	void HideManagedLesson();
	void ProcessDefinitionEvent(CScriptedIconLesson *definition, const InstructorEventData &event, int eventType);
	void ProcessOpportunityEvent(CScriptedIconLesson *opportunity, const InstructorEventData &event, int eventType);

	std::vector<CScriptedIconLesson*> m_Lessons;
	std::vector<CScriptedIconLesson*> m_OpenOpportunities;
	std::vector<std::pair<std::string, std::string> > m_GroupConVarToggles;
	bool m_bInitialized;
	bool m_bNoDraw;
	bool m_bHiddenDueToOtherElements;
	bool m_bManagedLessonVisible;
	std::string m_ManagedLessonName;
	int m_iCurrentPriority;
	float m_flLastUpdate;
	std::string m_PreviousStartSound;
	float m_flNextStartSoundTime;
};

C_GameInstructor &GetGameInstructor();
void GameInstructor_Init();
void GameInstructor_Shutdown();
void GameInstructor_Update(float now);
void GameInstructor_FireEvent(const InstructorEventData &event);
void GameInstructor_MarkDisplayed(const char *name);
void GameInstructor_FireEvent(const char *name, int entityIndex, const char *className,
	const char *targetName, const char *stringValue, int integerValue, float floatValue);

// Implemented by the GoldSrc HUD. Each active lesson owns an independent locator
// target; the panel caps the combined map/managed target pool at ten entries.
void HUD_SetManagedInstructorHint(const char *lessonName, int instanceSerial, const char *caption,
	const char *binding, const char *onscreenIcon, const char *offscreenIcon,
	int targetIndex, int priority, float timeout, int flags, int colorR, int colorG, int colorB,
	float range, float iconOffset, bool forceCaption,
	bool fixedPosition, float fixedX, float fixedY);
void HUD_PruneManagedInstructorHints();
void HUD_ClearManagedInstructorHint();
void HUD_PlayInstructorSound(const char *soundName);
float HUD_GetInstructorPresentationRestTime();
bool HUD_ApplyManagedInstructorPresentation(const char *lessonName, int instanceSerial, bool start);
bool HUD_IsManagedInstructorPresentationComplete(const char *lessonName, int instanceSerial);

#endif
