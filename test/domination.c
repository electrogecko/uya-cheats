#include <tamtypes.h>

#include <libuya/stdio.h>
#include <libuya/string.h>
#include <libuya/player.h>
#include <libuya/utils.h>
#include <libuya/game.h>
#include <libuya/pad.h>
#include <libuya/uya.h>
#include <libuya/weapon.h>
#include <libuya/interop.h>
#include <libuya/moby.h>
#include <libuya/graphics.h>
#include <libuya/gamesettings.h>
#include <libuya/spawnpoint.h>
#include <libuya/team.h>
#include <libuya/ui.h>
#include <libuya/time.h>
#include <libuya/camera.h>
#include <libuya/gameplay.h>
#include <libuya/guber.h>
#include <libuya/sound.h>

#define MAX_SEGMENTS                        (64)
#define MIN_SEGMENTS                        (8)
#define BASE_RADIUS                         (20.0f)
#define DOMINATION_BASE_ALPHA               (64)    // ~25% alpha keeps ring visible without bloom
#define DOMINATION_CAPTURE_STEP_BASE        (0.01f)
#define DOMINATION_CAPTURE_STEP_MAX         (0.05f)
#define DOMINATION_OWNER_DECAY_STEP         (0.0025f)
#define DOMINATION_NEUTRAL_DECAY_STEP       (0.0010f)
#define DOMINATION_NEUTRAL_RETURN_TO_CENTER (1)     // 1 => drift to 50%, 0 => hold current bias when neutral
#define DOMINATION_CAPTURE_COMPLETE_LOW     (0.02f)
#define DOMINATION_CAPTURE_COMPLETE_HIGH    (0.98f)
#define DOMINATION_RING_ALPHA_SCALE               (1.0f) // 1 for default 
#define DOMINATION_RING_HEIGHT                    (1.0f) // 2 for defualt
#define DOMINATION_RING_ALPHA_SCALE_NEUTRAL       (1.0f)
#define DOMINATION_RING_HEIGHT_NEUTRAL            (2.0f)
#define DOMINATION_NODE_Z                         (10.0f)

static inline int playerIsLocal(Player *player)
{
    return player && player->isLocal;
}

static float clamp01(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void approachFloat(float *value, float target, float step)
{
    if (*value < target) {
        *value += step;
        if (*value > target)
            *value = target;
    } else if (*value > target) {
        *value -= step;
        if (*value < target)
            *value = target;
    }
}

static u32 lerpColor(u32 a, u32 b, float t)
{
    t = clamp01(t);
    int aA = (a >> 24) & 0xFF;
    int aR = (a >> 16) & 0xFF;
    int aG = (a >> 8) & 0xFF;
    int aB = a & 0xFF;

    int bA = (b >> 24) & 0xFF;
    int bR = (b >> 16) & 0xFF;
    int bG = (b >> 8) & 0xFF;
    int bB = b & 0xFF;

    int rA = aA + (int)((bA - aA) * t);
    int rR = aR + (int)((bR - aR) * t);
    int rG = aG + (int)((bG - aG) * t);
    int rB = aB + (int)((bB - aB) * t);

    return (rA << 24) | (rR << 16) | (rG << 8) | (rB & 0xFF);
}

static inline u32 applyBaseAlpha(u32 color)
{
    return (DOMINATION_BASE_ALPHA << 24) | (color & 0x00FFFFFF);
}

static u32 getBoltCrankTextColor(float percent01)
{
    const u32 redColor = applyBaseAlpha(0x00FF5050);
    const u32 whiteColor = applyBaseAlpha(0x00FFFFFF);
    const u32 blueColor = applyBaseAlpha(0x003060FF);

    percent01 = clamp01(percent01);
    if (percent01 <= 0.5f) {
        float localT = percent01 / 0.5f;
        return lerpColor(redColor, whiteColor, localT);
    }

    float localT = (percent01 - 0.5f) / 0.5f;
    return lerpColor(whiteColor, blueColor, localT);
}

typedef struct DominationBase {
	int state;
    int owner;
    Moby *node;
	Moby *boltCrank;
    Player *players[8];
	int color;
    float scrolling;
    float boltCrankPercent;
    int localPlayerInside;
    int nodeAdjusted;
} DominationBase_t;

typedef struct DominationInfo {
	int gameState;
	float baseRaddius;
    int baseCount;
    Moby *bases[8];
} DominationInfo_t;
DominationInfo_t domInfo;

Moby *spawnBaseMobies(Moby *node, Moby *boltCrank);

void vector_rodrigues(VECTOR output, VECTOR v, VECTOR axis, float angle)
{
    VECTOR k, v_cross, term1, term2, term3;
    float cosTheta = cosf(angle);
    float sinTheta = sinf(angle);

    // normalize axis into k
    vector_normalize(k, axis);

    // term1 = v * cos(theta)
    vector_scale(term1, v, cosTheta);

    // term2 = (k x v) * sin(theta)
    vector_outerproduct(v_cross, k, v);  // cross product
    vector_scale(term2, v_cross, sinTheta);

    // term3 = k * (k . v) * (1 - cos(theta))
    float dot = vector_innerproduct(k, v);
    vector_scale(term3, k, dot * (1.0f - cosTheta));

    // output = term1 + term2 + term3
    vector_add(output, term1, term2);
    vector_add(output, output, term3);

    // preserve homogeneous component
    output[3] = v[3];
}

void getBases(void)
{
	Moby* moby = mobyListGetStart();
	Moby* mobyEnd = mobyListGetEnd();
	int i = 0;
	while (moby < mobyEnd) {
		if (moby->oClass == MOBY_ID_SIEGE_NODE) {
			// grab associated bolt crank from bridge peice
			int boltCrankInstanceNum = *(int*)(moby->pVar + 0x3c);
			Moby* list = mobyListGetStart();
			Moby* boltCrank = list + boltCrankInstanceNum;
			if (boltCrank->oClass == MOBY_ID_BOLT_CRANK) {
				Moby *base = spawnBaseMobies(moby, boltCrank);
                domInfo.bases[i] = (Moby*)base;
                ++domInfo.baseCount;
                printf("\nn: %d, base: %08x", domInfo.baseCount, &domInfo.bases[i]);
            }
			++i;
		}
		++moby;
	}
}

void drawBase(Moby *base)
{
    DominationBase_t *pvar = base->pVar;
    float scrollQuad = pvar->scrolling;
    u32 baseColor = pvar->color;

    float percent01 = clamp01(pvar->boltCrankPercent * 0.01f);
    float captureBlend = fabsf(percent01 - 0.5f) * 2.0f;
    float alphaScale = (DOMINATION_RING_ALPHA_SCALE_NEUTRAL * (1.0f - captureBlend)) + (DOMINATION_RING_ALPHA_SCALE * captureBlend);
    float ringHeight = (DOMINATION_RING_HEIGHT_NEUTRAL * (1.0f - captureBlend)) + (DOMINATION_RING_HEIGHT * captureBlend);

    int i, k, j, s;
    QuadDef quad[3];
    // get texture info (tex0, tex1, clamp, alpha)
    //gfxSetupEffectTex(&quad[0], FX_TIRE_TRACKS + 1, 0, 0x80);
    //gfxSetupEffectTex(&quad[0], FX_CIRCLE_OUTLINE_6, 0, 0x80);
    //gfxSetupEffectTex(&quad[0], FX_RETICLE_4, 0, 0x80);
    gfxSetupEffectTex(&quad[0], FX_UNK_2, 0, 0x80); // nice bars  - my choice
    //gfxSetupEffectTex(&quad[0], FX_SQUARE_WHITE_WITH_TRANSPARENT_DOTS, 0, 0x80);
    //gfxSetupEffectTex(&quad[0], FX_VISIBOMB_HORIZONTAL_LINES, 0, 0x80); //circless
    //gfxSetupEffectTex(&quad[0], FX_RETICLE_5, 0, 0x80);

    gfxSetupEffectTex(&quad[2], FX_CIRLCE_NO_FADED_EDGE, 0, 0x80);

    

    quad[0].uv[0] = (UV_t){0, 0}; // bottom left (-, -)
    quad[0].uv[1] = (UV_t){0, 1}; // top left (-, +)
    quad[0].uv[2] = (UV_t){1, 0}; // bottom right (+, -)
    quad[0].uv[3] = (UV_t){1, 1}; // top right (+, +)

    // copy quad 0 to quad 2
    memcpy(quad[2].uv, &quad[0].uv, sizeof(quad[0].uv));

    // modify top and bottom level UVs Y.  (uv is turned 90 degrees)
    float uvOffset = 0; // .04;
    quad[0].uv[0].y += uvOffset;
    quad[0].uv[1].y -= uvOffset;
    quad[0].uv[2].y += uvOffset;
    quad[0].uv[3].y -= uvOffset;

    // copy quad 0 uv to quad 1;
    quad[1] = quad[0];

    // set seperate rgbas
    int alphaOuterNear = (int)(0x00 * alphaScale) & 0xFF;
    int alphaOuterFar = (int)(0x30 * alphaScale);
    if (alphaOuterFar > 0xFF) alphaOuterFar = 0xFF;
    int alphaMidNear = (int)(0x50 * alphaScale);
    if (alphaMidNear > 0xFF) alphaMidNear = 0xFF;
    int alphaMidFar = (int)(0x20 * alphaScale);
    if (alphaMidFar > 0xFF) alphaMidFar = 0xFF;
    int alphaCenter = (int)(0x30 * alphaScale);
    if (alphaCenter > 0xFF) alphaCenter = 0xFF;

    u32 baseRgb = baseColor & 0x00FFFFFF;

    quad[0].rgba[0] = quad[0].rgba[1] = (alphaOuterNear << 24) | baseRgb;
    quad[0].rgba[2] = quad[0].rgba[3] = (alphaOuterFar << 24) | baseRgb;
    quad[1].rgba[0] = quad[1].rgba[1] = (alphaMidNear << 24) | baseRgb;
    quad[1].rgba[2] = quad[1].rgba[3] = (alphaMidFar << 24) | baseRgb;
    quad[2].rgba[0] = quad[2].rgba[1] = quad[2].rgba[2] = quad[2].rgba[3] = (alphaCenter << 24) | baseRgb;

    VECTOR center, tempCenter, tempRight, tempUp, halfX, halfZ, vRadius;
    vector_copy(center, base->position);
    VECTOR xAxis = {domInfo.baseRaddius, 0, 0, 0};
    VECTOR zAxis = {0, domInfo.baseRaddius, 0, 0};
    VECTOR yAxis = {0, 0, ringHeight, 0};
    
    vector_scale(halfX, xAxis, .5);
    vector_scale(halfZ, zAxis, .5);
    float fRadius = vector_length(halfX);
    int signs[4][2] = {{1, -1}, {-1, -1}, {1, 1}, {-1, 1}};
    vector_normalize(yAxis, yAxis);

    // get tangent
    vector_outerproduct(tempRight, yAxis, halfX);
    vector_normalize(tempRight, tempRight);

    // scale x, y of texture
    vector_scale(tempRight, tempRight, 1);
    vector_scale(tempUp, yAxis, ringHeight * 0.5f);

    float segmentSize = 1;
    int segments = (int)((2 * MATH_PI * fRadius) / segmentSize);
    float thetaStep = 2 * MATH_PI / clamp((float)segments, MIN_SEGMENTS, MAX_SEGMENTS);

    for (k = 0; k < 2; ++k) {
		// draw top and botom quad
		// copy vRadius into r
		vector_copy(vRadius, halfX);
		for (i = 0; i < segments; ++i) {
			vector_add(tempCenter, center, vRadius);
            // offset quad[1] by configured height
            tempCenter[2] += k * ringHeight;
			// create vector for each point.
			for (j = 0; j < 4; ++j) {
				quad[k].point[j][0] = tempCenter[0] + signs[j][0] * tempRight[0] + signs[j][1] * tempUp[0];
				quad[k].point[j][1] = tempCenter[1] + signs[j][0] * tempRight[1] + signs[j][1] * tempUp[1];
				quad[k].point[j][2] = tempCenter[2] + signs[j][0] * tempRight[2] + signs[j][1] * tempUp[2];
				quad[k].point[j][3] = 1;
			}

			quad[k].uv[0].x = quad[k].uv[1].x = 0 - scrollQuad;
			quad[k].uv[2].x = quad[k].uv[3].x = 1 - scrollQuad;
			
			// maybe for later:  put points in array.
			// quadPos[k][i] = quad[k].point;
			
			gfxDrawQuad(quad[k], NULL);

			// rotate radius and tangent
			vector_rodrigues(vRadius, vRadius, yAxis, thetaStep);
			vector_outerproduct(tempRight, yAxis, vRadius);
			vector_normalize(tempRight, tempRight);
		}
    }
    // scroll quad to animate
    pvar->scrolling += .007f;

    // draw floor quad
    VECTOR offset = {0, 0, -1, 0};
    VECTOR corners[4];

	vector_copy(corners[0], (VECTOR){center[0] - fRadius, center[1] - fRadius, center[2], 0});
    vector_copy(corners[1], (VECTOR){center[0] + fRadius, center[1] - fRadius, center[2], 0});
    vector_copy(corners[2], (VECTOR){center[0] + fRadius, center[1] + fRadius, center[2], 0});
    vector_copy(corners[3], (VECTOR){center[0] - fRadius, center[1] + fRadius, center[2], 0});

    vector_copy(quad[2].point[0], corners[1]);
    vector_copy(quad[2].point[1], corners[0]);
    vector_copy(quad[2].point[2], corners[2]);
    vector_copy(quad[2].point[3], corners[3]);

    gfxDrawQuad(quad[2], NULL);

    // show capture progress when a local player is inside the base radius
    if (pvar->localPlayerInside) {
        char text[32];
        int percentRounded = (int)(pvar->boltCrankPercent + 0.5f);
        if (percentRounded < 0)
            percentRounded = 0;
        if (percentRounded > 100)
            percentRounded = 100;

        snprintf(text, sizeof(text), "Bolt Crank %d%%", percentRounded);
        u32 textColor = (0x80 << 24) | (pvar->color & 0x00ffffff);
        gfxScreenSpaceText(SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.85f, 1, 1, textColor, text, -1, TEXT_ALIGN_MIDDLECENTER, FONT_BOLD);
    }
}

int baseCheckIfInside(VECTOR basePos, VECTOR playerPos)
{
    VECTOR delta;
    vector_subtract(delta, playerPos, basePos);

    // check Y axis
    if (delta[2] < -1.25 || delta[2] > basePos[2] + 6) {
        return 0;
    }
    // check radius
    float radius = domInfo.baseRaddius / 2;
    float distSq = delta[0] * delta[0] + delta[1] * delta[1];

    return (distSq <= radius * radius);
}

void basePlayerUpdate(Moby *this)
{
    DominationBase_t *pvar = (DominationBase_t*)this->pVar;
    GameSettings *gs = gameGetSettings();
    int i, j;
    int localPlayerInside = 0;
    
    for (i = 0; i < GAME_MAX_PLAYERS; ++i) {
        Player *player = playerGetFromSlot(i);
        if (!player || playerIsDead(player))
            continue;

        int in = baseCheckIfInside(this->position, player->playerPosition);
        if (in) {
            if (playerIsLocal(player))
                localPlayerInside = 1;

            // Check if player is already in the array
            int alreadyIn = 0;
            for (j = 0; j < 8; ++j) {
                if (pvar->players[j] == player) {
                    alreadyIn = 1;
                    break;
                }
            }
            
            // Add player if not already in array
            if (!alreadyIn) {
                for (j = 0; j < 8; ++j) {
                    if (!pvar->players[j]) {
                        pvar->players[j] = player;
                        break;
                    }
                }
            }
        } else {
            // Remove player from array if they left the radius
            for (j = 0; j < 8; ++j) {
                if (pvar->players[j] == player) {
                    pvar->players[j] = NULL;
                    break;
                }
            }
        }
    }

    pvar->localPlayerInside = localPlayerInside;
    
    // Set color based on first player in array
    // pvar->color = 0x00ffffff; // Default white
    // for (j = 0; j < 8; ++j) {
    //     if (pvar->players[j]) {
    //         pvar->color = TEAM_COLORS[pvar->players[j]->mpTeam];
    //         break;
    //     }
    // }

    // cache capture percentage for HUD drawing and color
    pvar->boltCrankPercent = 0;
    if (pvar->boltCrank && pvar->boltCrank->pVar) {
        M6695_BoltCrank_t *boltVars = (M6695_BoltCrank_t*)pvar->boltCrank->pVar;
        if (boltVars) {
            float normalized = clamp01(boltVars->bias);
            pvar->boltCrankPercent = normalized * 100.0f;
        }
    }

    // set color based purely on capture progress
    float percent01 = pvar->boltCrankPercent * 0.01f;
    pvar->color = getBoltCrankTextColor(percent01);

    // Move the Siege Moby node collision out of the way
    // Player *debug = playerGetFromSlot(0); //DBUG
    // if (debug && playerPadGetButtonDown(debug, PAD_UP) > 0 && pvar->node && !pvar->nodeAdjusted) { //DBUG
    if (!pvar->nodeAdjusted && pvar->node) {
        pvar->node->collData = NULL;
        pvar->nodeAdjusted = 1;
        printf("\n[Dom] disabled node collision for testing.");
    }
}

void baseHandleCapture(Moby* this)
{
    int capturingTeam = -1;
    int isContested = 0;
    int capturingCount = 0;
    int defendingCount = 0;
    Player *capturingPlayer[8] = {0};  // Initialize to NULL
    Player *defendingPlayer[8] = {0};  // Initialize to NULL
    int i;
    DominationBase_t *pvars = (DominationBase_t*)this->pVar;

    // Safety check
    if (!pvars || !pvars->boltCrank || !pvars->boltCrank->pVar)
        return;
    M6695_BoltCrank_t *boltVars = (M6695_BoltCrank_t*)pvars->boltCrank->pVar;

    // Get first capturing player and team
    for (i = 0; i < 8; ++i) {
        if (pvars->players[i] && !playerIsDead(pvars->players[i])) {
            if (capturingTeam == -1) {
                // First player found - they become the capturing team
                capturingTeam = pvars->players[i]->mpTeam;
                capturingPlayer[capturingCount] = pvars->players[i];
                ++capturingCount;
            } else if (pvars->players[i]->mpTeam == capturingTeam) {
                // Same team as capturing team
                capturingPlayer[capturingCount] = pvars->players[i];
                ++capturingCount;
            } else {
                // Different team - it's contested!
                defendingPlayer[defendingCount] = pvars->players[i];
                ++defendingCount;
                isContested = 1;
            }
        }
    }

    float targetBias = boltVars->bias;
    float step = DOMINATION_NEUTRAL_DECAY_STEP; 
    if (!isContested && capturingTeam != -1) {

        step = DOMINATION_CAPTURE_STEP_BASE;
        if (capturingCount > 1) {
            float scaledStep = step * capturingCount;
            if (scaledStep > DOMINATION_CAPTURE_STEP_MAX)
                scaledStep = DOMINATION_CAPTURE_STEP_MAX;
            step = scaledStep;
        }

        if (capturingTeam == TEAM_BLUE) {
            targetBias = 0.0f;
        } else if (capturingTeam == TEAM_RED) {
            targetBias = 1.0f;
        } else {
            targetBias = 0.5f;
        }
    } else {
        pvars->state = 5;
        if (pvars->owner == TEAM_BLUE) {
            targetBias = 0.0f;
            step = DOMINATION_OWNER_DECAY_STEP;
        } else if (pvars->owner == TEAM_RED) {
            targetBias = 1.0f;
            step = DOMINATION_OWNER_DECAY_STEP;
        } else {
            targetBias = DOMINATION_NEUTRAL_RETURN_TO_CENTER ? 0.5f : boltVars->bias;
            step = DOMINATION_NEUTRAL_DECAY_STEP;
        }
    }

    approachFloat(&boltVars->bias, targetBias, step);
    boltVars->bias = clamp01(boltVars->bias);
    *(float*)(pvars->boltCrank->pVar) = boltVars->bias;

    if (!isContested && capturingTeam == TEAM_BLUE && boltVars->bias <= DOMINATION_CAPTURE_COMPLETE_LOW) {
        pvars->owner = TEAM_BLUE;
    } else if (!isContested && capturingTeam == TEAM_RED && boltVars->bias >= DOMINATION_CAPTURE_COMPLETE_HIGH) {
        pvars->owner = TEAM_RED;
    }

    printf("\n[Dom] base=%p capTeam=%d contested=%d count=%d target=%.3f bias=%.3f step=%.3f mem=0x%08X",
        this,
        capturingTeam,
        isContested,
        capturingCount,
        targetBias,
        boltVars->bias,
        step,
        *(u32*)(pvars->boltCrank->pVar));
}

void updateBase(Moby* this)
{
    DominationBase_t *pvars = (DominationBase_t*)this->pVar;
    if (!pvars) return;

    // turn collision off and move bolt crank to y: 0
	pvars->boltCrank->collData = 0;
	pvars->boltCrank->position[2] = 0;

    // draw base
    gfxRegistserDrawFunction(&drawBase, (Moby*)this);

    // handle players and base color
    basePlayerUpdate(this);

    // handle capture
    baseHandleCapture(this);
}

Moby *spawnBaseMobies(Moby *node, Moby *boltCrank)
{
    Moby *moby = mobySpawn(0x1c0d, sizeof(DominationBase_t));
    if (!moby) return NULL;

    moby->pUpdate = &updateBase;
    vector_copy(moby->position, boltCrank->position);
    moby->updateDist = -1;
    moby->drawn = 1;
    moby->opacity = 0x00;
    moby->drawDist = 0x00;

    // set pvars
    DominationBase_t *base = (DominationBase_t*)moby->pVar;
    memset(base, 0, sizeof(DominationBase_t));
    base->node = (Moby*)node;
    base->boltCrank = (Moby*)boltCrank;
    base->color = 0x00ffffff;
    base->owner = -1;


    return moby;
}

void domination(void)
{
	if (!isInGame())
		return;

	if (domInfo.gameState == 0) {
		getBases();
		domInfo.gameState = 1;
		domInfo.baseRaddius = BASE_RADIUS;
	}
}
