/*
 * Generated from loonylynn/private ragdoll.smd and ragdoll.qci.
 * Vertices are transformed from SMD model space into bone-local bind space
 * using Valve's AngleMatrix / AngleQuaternion convention (Z * Y * X).
 * Regenerate this file if the private source rig changes.
 */
#ifndef NPC_ZOMBIE_RAGDOLL_DATA_H
#define NPC_ZOMBIE_RAGDOLL_DATA_H
#include <cfloat>
struct NPCZombieRagdollPoint { float x, y, z; };
struct NPCZombieRagdollBodyDefinition {
 const char *boneName;
 int parentBody;
 NPCZombieRagdollPoint localShapeCenter;
 const NPCZombieRagdollPoint *points;
 int pointCount;
 float jointMinDegrees[3];
 float jointMaxDegrees[3];
 float angularDampingOverride;
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints0[8] = {
 { -2.330021f, -5.416637f, -5.999986f },
 { 4.958348f, -3.191053f, -5.999992f },
 { 4.958348f, -3.191022f, 6.000008f },
 { -2.330021f, -5.416605f, 6.000014f },
 { -4.958348f, 3.191072f, -6.000008f },
 { -4.958348f, 3.191103f, 5.999992f },
 { 2.330021f, 5.416587f, 5.999986f },
 { 2.330021f, 5.416555f, -6.000014f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints1[8] = {
 { 6.299042f, -4.268851f, -3.634965f },
 { -7.287992f, -2.187750f, 3.634956f },
 { -7.766420f, -2.114461f, -2.515044f },
 { 6.777470f, -4.342140f, 2.515034f },
 { 7.287992f, 2.187750f, -3.634956f },
 { -6.299042f, 4.268851f, 3.634965f },
 { 7.766420f, 2.114461f, 2.515044f },
 { -6.777470f, 4.342140f, -2.515034f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints2[8] = {
 { 7.142175f, -2.259524f, -2.661165f },
 { -6.730231f, -3.289425f, 2.661158f },
 { -6.730241f, -3.289419f, -2.661142f },
 { 7.142185f, -2.259530f, 2.661135f },
 { 6.730183f, 3.289414f, -2.661158f },
 { -7.142126f, 2.259535f, 2.661165f },
 { 6.730192f, 3.289408f, 2.661142f },
 { -7.142136f, 2.259541f, -2.661135f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints3[8] = {
 { 1.392774f, -4.999517f, -1.650009f },
 { -1.389321f, -5.000482f, 1.649995f },
 { -1.389326f, -5.000478f, -1.650005f },
 { 1.392779f, -4.999522f, 1.649991f },
 { 1.389321f, 5.000482f, -1.649995f },
 { -1.392774f, 4.999517f, 1.650009f },
 { 1.389326f, 5.000478f, 1.650005f },
 { -1.392779f, 4.999522f, -1.649991f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints4[8] = {
 { 6.560007f, -4.308832f, 3.084485f },
 { -7.548950f, -2.147779f, 3.084506f },
 { -7.548957f, -2.147769f, -3.084494f },
 { 6.559999f, -4.308822f, -3.084515f },
 { 7.548957f, 2.147769f, 3.084494f },
 { 7.548950f, 2.147779f, -3.084506f },
 { -6.560007f, 4.308832f, -3.084485f },
 { -6.559999f, 4.308822f, 3.084515f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints5[8] = {
 { 7.142160f, -2.259535f, 2.661135f },
 { -6.730256f, -3.289430f, 2.661158f },
 { -6.730265f, -3.289425f, -2.661142f },
 { 7.142151f, -2.259530f, -2.661165f },
 { 6.730265f, 3.289425f, 2.661142f },
 { 6.730256f, 3.289430f, -2.661158f },
 { -7.142160f, 2.259535f, -2.661135f },
 { -7.142151f, 2.259530f, 2.661165f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints6[8] = {
 { 1.392779f, -4.999522f, 1.649991f },
 { -1.389321f, -5.000482f, 1.649995f },
 { -1.389326f, -5.000478f, -1.650005f },
 { 1.392774f, -4.999517f, -1.650009f },
 { 1.389326f, 5.000478f, 1.650005f },
 { 1.389321f, 5.000482f, -1.649995f },
 { -1.392779f, 4.999522f, -1.649991f },
 { -1.392774f, 4.999517f, 1.650009f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints7[8] = {
 { -8.380633f, -4.032955f, -7.999996f },
 { 6.365472f, -6.780875f, -8.000028f },
 { 6.365499f, -6.780915f, 7.999972f },
 { -8.380606f, -4.032995f, 8.000004f },
 { -6.365499f, 6.780915f, -7.999972f },
 { -6.365472f, 6.780875f, 8.000028f },
 { 8.380633f, 4.032955f, 7.999996f },
 { 8.380606f, 4.032995f, -8.000004f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints8[8] = {
 { -4.207995f, -5.856003f, -5.443308f },
 { 3.787055f, -6.136575f, -5.443322f },
 { 3.787073f, -6.136603f, 5.443278f },
 { -4.207976f, -5.856031f, 5.443292f },
 { -3.787104f, 6.136666f, -5.443278f },
 { -3.787086f, 6.136639f, 5.443322f },
 { 4.208026f, 5.855940f, 5.443308f },
 { 4.208007f, 5.855967f, -5.443292f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints9[8] = {
 { 7.558819f, 2.342640f, 1.718447f },
 { -7.483731f, 2.655096f, -1.587664f },
 { -7.438238f, 2.562653f, 1.918634f },
 { 7.513301f, 2.435085f, -1.787948f },
 { 7.483710f, -2.655054f, 1.587722f },
 { -7.558794f, -2.342730f, -1.718404f },
 { 7.438280f, -2.562650f, -1.918696f },
 { -7.513347f, -2.435040f, 1.787909f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints10[8] = {
 { 6.760554f, 1.381055f, 1.399019f },
 { -6.656833f, 1.607940f, -1.634015f },
 { -6.658216f, 1.843103f, 1.356759f },
 { 6.761840f, 1.145891f, -1.591732f },
 { 6.656870f, -1.607944f, 1.633955f },
 { -6.760494f, -1.381050f, -1.398982f },
 { 6.658156f, -1.843108f, -1.356795f },
 { -6.761877f, -1.145887f, 1.591792f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints11[8] = {
 { 4.047028f, 6.293937f, 4.169371f },
 { -7.717048f, -2.927679f, -2.291554f },
 { -2.756117f, -3.000649f, 7.534936f },
 { -0.913903f, 6.366907f, -5.657119f },
 { 7.717048f, 2.927679f, 2.291554f },
 { -4.047028f, -6.293937f, -4.169371f },
 { 2.756117f, 3.000649f, -7.534936f },
 { 0.913903f, -6.366907f, 5.657119f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints12[8] = {
 { 7.614233f, 2.343248f, -1.452217f },
 { -7.366562f, 2.561994f, -2.178464f },
 { -7.535061f, 2.654472f, 1.324175f },
 { 7.445762f, 2.435724f, 2.050326f },
 { 7.535004f, -2.654450f, -1.324140f },
 { 7.366619f, -2.562016f, 2.178430f },
 { -7.614204f, -2.343268f, 1.452278f },
 { -7.445791f, -2.435705f, -2.050387f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints13[8] = {
 { 6.805931f, 1.375853f, -1.164405f },
 { -6.605828f, 1.890501f, -1.535750f },
 { -6.699313f, 1.608672f, 1.449501f },
 { 6.712447f, 1.094025f, 1.820847f },
 { 6.699302f, -1.608668f, -1.449556f },
 { 6.605817f, -1.890496f, 1.535695f },
 { -6.805872f, -1.375858f, 1.164473f },
 { -6.712484f, -1.094030f, -1.820805f },
};

static const NPCZombieRagdollPoint s_npcZombieRagdollPoints14[8] = {
 { 5.070766f, 5.028103f, -4.730885f },
 { -4.204866f, -2.196580f, -7.132341f },
 { -7.927763f, -0.830143f, 3.136493f },
 { 1.347869f, 6.394540f, 5.537948f },
 { 7.927720f, 0.830161f, -3.136511f },
 { 4.204823f, 2.196598f, 7.132323f },
 { -5.070723f, -5.028121f, 4.730903f },
 { -1.347826f, -6.394558f, -5.537930f },
};

static const NPCZombieRagdollBodyDefinition g_npcZombieRagdollBodies[] = {
 { "Bip01 Pelvis", -1, { 1.213749f, 1.456706f, -0.096833f }, s_npcZombieRagdollPoints0, 8,
   { -FLT_MAX, -FLT_MAX, -FLT_MAX }, { FLT_MAX, FLT_MAX, FLT_MAX }, 0.000000f },
 { "Bip01 L Leg", 0, { 8.947076f, -0.994123f, 0.368176f }, s_npcZombieRagdollPoints1, 8,
   { -25.0000f, -20.0000f, -37.0000f }, { 20.0000f, 40.0000f, 63.0000f }, 0.000000f },
 { "Bip01 L Leg1", 1, { 7.659767f, -1.782532f, -0.325246f }, s_npcZombieRagdollPoints2, 8,
   { -FLT_MAX, -FLT_MAX, -95.0000f }, { FLT_MAX, FLT_MAX, 4.0000f }, 0.000000f },
 { "Bip01 L Foot", 2, { 0.434421f, 2.366664f, -0.651707f }, s_npcZombieRagdollPoints3, 8,
   { -30.0000f, -30.0000f, -15.0000f }, { 30.0000f, 30.0000f, 35.0000f }, 0.000000f },
 { "Bip01 R Leg", 0, { 9.055915f, -1.010802f, -0.545795f }, s_npcZombieRagdollPoints4, 8,
   { -25.0000f, -40.0000f, -37.0000f }, { 20.0000f, 20.0000f, 63.0000f }, 0.000000f },
 { "Bip01 R Leg1", 4, { 7.707207f, -1.771811f, -0.243468f }, s_npcZombieRagdollPoints5, 8,
   { -FLT_MAX, -FLT_MAX, -95.0000f }, { FLT_MAX, FLT_MAX, 4.0000f }, 0.000000f },
 { "Bip01 R Foot", 5, { 0.464632f, 2.366664f, -0.227728f }, s_npcZombieRagdollPoints6, 8,
   { -30.0000f, -30.0000f, -15.0000f }, { 30.0000f, 30.0000f, 35.0000f }, 0.000000f },
 { "Bip01 Spine2", 0, { 3.583276f, 0.450017f, -0.217851f }, s_npcZombieRagdollPoints7, 8,
   { -60.0000f, -25.0000f, -25.0000f }, { 60.0000f, 25.0000f, 40.0000f }, 0.000000f },
 { "Bip01 Head", 7, { 3.658694f, 0.437345f, 1.415130f }, s_npcZombieRagdollPoints8, 8,
   { -20.0000f, -25.0000f, -13.0000f }, { 20.0000f, 25.0000f, 30.0000f }, 5.000000f },
 { "Bip01 L Arm1", 7, { 7.344553f, -0.462527f, 0.411777f }, s_npcZombieRagdollPoints9, 8,
   { -30.0000f, -67.0000f, -73.0000f }, { 15.0000f, 30.0000f, 12.0000f }, 0.000000f },
 { "Bip01 L Arm2", 9, { 6.536763f, 0.164811f, 0.285512f }, s_npcZombieRagdollPoints10, 8,
   { -FLT_MAX, -10.0000f, -100.0000f }, { FLT_MAX, 40.0000f, 20.0000f }, 0.000000f },
 { "Bip01 L Hand", 10, { 6.332715f, 3.541425f, -1.645154f }, s_npcZombieRagdollPoints11, 8,
   { -60.0000f, -30.0000f, -90.0000f }, { 60.0000f, 30.0000f, 70.0000f }, 0.000000f },
 { "Bip01 R Arm1", 7, { 7.395545f, -0.463598f, -0.319403f }, s_npcZombieRagdollPoints12, 8,
   { -15.0000f, -45.0000f, -73.0000f }, { 30.0000f, 64.0000f, 12.0000f }, 0.000000f },
 { "Bip01 R Arm2", 12, { 6.571430f, 0.156963f, 0.298149f }, s_npcZombieRagdollPoints13, 8,
   { -FLT_MAX, -40.0000f, -100.0000f }, { FLT_MAX, 10.0000f, 20.0000f }, 0.000000f },
 { "Bip01 R Hand", 13, { 6.445609f, 2.139584f, 1.606837f }, s_npcZombieRagdollPoints14, 8,
   { -60.0000f, -30.0000f, -90.0000f }, { 60.0000f, 30.0000f, 70.0000f }, 0.000000f },
};
static const int NPC_ZOMBIE_RAGDOLL_BODY_COUNT = 15;
static const float NPC_ZOMBIE_RAGDOLL_TOTAL_MASS = 115.0f;
static const float NPC_ZOMBIE_RAGDOLL_INERTIA_MULTIPLIER = 10.0f;
static const float NPC_ZOMBIE_RAGDOLL_LINEAR_DAMPING = 0.01f;
static const float NPC_ZOMBIE_RAGDOLL_ANGULAR_DAMPING = 1.50f;
#endif
