// ============================================================
//  ZENO Life 5  —  "zenov13"   Living Soul & Organic Animation
//
//  WHAT'S NEW vs zenov12:
//  • Organic Eye Geometry: Happy lower-lid cheek push (crescent
//    smiling eyes), angled brow masks, and squash-and-stretch.
//  • Micro-Saccades & Gaze Tracking: Eyes dart naturally when
//    scanning, lock onto tilt gravity, and peek when shy.
//  • Grudge & Flinch System: Sharp taps cause an immediate
//    flinch + glare; gentle rocking builds purr-like bliss.
//  • Procedural Particles: Sine-wave Zzz's, floating hearts,
//    stress sweat-drops, and gravity-aware dizzy bubbles.
//  • Bug Fixes: Fixed sleep_pressure conflict between
//    sleepEngine() and updateInternalState(), cleaned unused
//    clock buffer, and smoothed blink physics.
//
//  SDA->GPIO9  SCL->GPIO10  MPU AD0->GND
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ═══════════════════════════════════════════════════════════
//  ██  PERSONALITY & BEHAVIOUR KNOBS  ██
// ═══════════════════════════════════════════════════════════
#define ZENO_MAX_ANGRY       0.80f   // irritation ceiling
#define ZENO_CRY_THRESHOLD   0.68f   // irritation that triggers crying
#define ZENO_ANGER_SPEED     1.15f   // irritation build multiplier
#define ZENO_MIN_HAPPY       0.10f   // valence floor
#define ZENO_MAX_HAPPY       1.00f   // valence ceiling
#define ZENO_HAPPY_SPEED     1.35f   // valence recovery/rise speed
#define ZENO_MIN_BORED       0.00f   // boredom floor
#define ZENO_MAX_BORED       0.90f   // boredom ceiling
#define ZENO_SLEEP_THRESH    0.68f   // sleep_pressure threshold for sleep
#define ZENO_CURIOUS_SPIKE   0.65f   // max autonomous curiosity burst
#define ZENO_PET_WARMTH      0.18f   // attachment gained per gentle rock/pet
#define ZENO_MOOD_INERTIA    0.84f   // emotional momentum (0..0.95)
#define ZENO_DEBUG_MS        0       // serial debug interval ms (0 = off)

#define ZENO_MIN_CRY_MS      5000
#define ZENO_MAX_CRY_MS      12000

// ═══════════════════════════════════════════════════════════
//  Hardware & Geometry
// ═══════════════════════════════════════════════════════════
#define WIFI_SSID  "Ping"
#define WIFI_PASS  "shanmukha2007"
#define OLED_SDA    9
#define OLED_SCL   10
#define OLED_ADDR  0x3C
#define MPU_ADDR   0x68

Adafruit_SSD1306* oled = nullptr;

#define EW   42
#define EH   36
#define ER    9
#define ELX  12
#define ERX  74
#define EY   13
#define PW   17
#define PH   19
#define PR    5

// ═══════════════════════════════════════════════════════════
//  STRUCTS
// ═══════════════════════════════════════════════════════════
typedef struct {
  float ax, ay, az;
  float jerk;
  float tilt_x, tilt_y;
  bool  inverted;
  bool  tapped;
} InputState;

typedef struct {
  float valence;        // 0=miserable  0.5=neutral  1=blissful
  float arousal;        // 0=sedated    1=hyper
  float attachment;     // 0=aloof      1=bonded
  float irritation;     // 0=calm       1=furious
  float curiosity;      // 0=dull       1=fascinated
  float sleep_pressure; // 0=alert      1=deep sleep
  float attention_need; // 0=content    1=needy
  float dvalence, darousal, dirrit, dcuriosity;
} InternalState;

typedef struct {
  float eye_open_l, eye_open_r;
  float smile_curve;    // 0..1 lower-lid upward cheek push (happy crescent)
  float brow_angle;     // -1 (sad/worried) .. 0 (neutral) .. +1 (angry)
  float pupil_x, pupil_y;
  float pupil_size;
  float jitter;
  float squish_x, squish_y;
  float look_tilt_y;    // vertical head-lean offset between left/right eyes
  bool  heart_pupils;
  bool  star_pupils;
  bool  rosy_cheeks;
  bool  dizzy_mode;
  bool  sweat_drop;
} FaceParams;

typedef struct {
  float motion_energy;
  float rotation_speed;
  float oscillation_score;
  float stillness;
} MotionFeatures;

typedef struct {
  float arousal_delta;
  float valence_delta;
  float curiosity_delta;
  float duration_left;
  float total_duration;
} MotionEvent;

typedef struct {
  float hunger;
  float fatigue;
  float social;
} NeedsState;

typedef struct {
  float valence_bias;
  float arousal_bias;
  float sleep_bias;
  float irritation_scale;
} TimeProfile;

// ═══════════════════════════════════════════════════════════
//  GLOBAL STATE
// ═══════════════════════════════════════════════════════════
static InputState     g_input  = {};
static InternalState  g_state  = {0.62f, 0.25f, 0.50f, 0.0f, 0.35f, 0.20f, 0.15f, 0,0,0,0};
static FaceParams     g_face   = {};
static MotionFeatures g_motion = {};
static NeedsState     g_needs  = {0.15f, 0.20f, 0.15f};

// Arcs & Micro-states
static bool     s_crying       = false;
static uint32_t s_cry_start    = 0;
static float    s_cry_tear1    = 0.0f;
static float    s_cry_tear2    = 7.0f;

static bool     s_sleeping     = false;
static bool     s_trying_sleep = false;
static uint32_t s_sleep_start  = 0;
static uint32_t s_sleep_dur    = 0;

static float    s_yawn_open    = 0.0f;
static bool     s_wink_active  = false;
static uint32_t s_wink_end     = 0;
static float    s_flinch_timer = 0.0f;
static float    s_surprise_boost = 0.0f;

// Particles (Bubbles / Hearts)
#define MAX_PARTICLES 6
typedef struct {
  float x, y, vx, vy, r, phase;
  uint8_t type; // 0=bubble, 1=heart
  bool alive;
  uint32_t born;
} Particle;
static Particle s_particles[MAX_PARTICLES] = {};
static uint32_t s_inverted_since = 0;

// Motion events
#define MAX_MOTION_EVENTS 4
static MotionEvent s_mevents[MAX_MOTION_EVENTS] = {};
static uint32_t s_cd_shake=0, s_cd_rotate=0, s_cd_oscillate=0, s_cd_stop=0, s_cd_still=0, s_cd_tap=0;

// Smoothing & Oscillation
static float s_me_energy=0, s_me_rot=0, s_me_still_acc=0, s_me_still=0, s_prev_energy=0;
#define OSC_WINDOW 16
static float    s_osc_buf[OSC_WINDOW] = {};
static uint8_t  s_osc_idx = 0;
static float    s_osc_score = 0;
static uint32_t s_osc_sample_t = 0;

// Gaze & Saccades
static float    s_tilt_gaze_x=0, s_tilt_gaze_y=0;
static float    s_px=0, s_py=0, s_pvx=0, s_pvy=0;
static float    s_tpx=0, s_tpy=0;
static uint32_t s_look_t=0;
static uint32_t s_saccade_t=0;
static float    s_saccade_ox=0, s_saccade_oy=0;

// Blink
static float    s_blink_lid=0, s_blink_vel=0;
static uint32_t s_next_blink=0;
static uint8_t  s_blink_seq=0;

// Smooth animation channels
static float s_sopen_l=1.0f, s_sopen_r=1.0f;
static float s_ssmile=0.0f, s_sbrow=0.0f;
static float s_spsize=1.0f, s_sjitter=0.0f;
static float s_breath=0.0f, s_breath_rate=0.025f;
static float s_dizzy_angle=0.0f;
static float s_happy_bounce=0.0f, s_sad_droop=0.0f;
static float s_ear_phase=0.0f;
static float s_squish=1.0f, s_squish_v=0.0f;

// Autonomy & Time
static uint32_t s_curiosity_spike_t=0;
static float    s_curiosity_spike=0;
static float    s_phase_a=0, s_phase_b=0.17f, s_phase_c=0.61f;
static int      s_hour=12, s_minute=0, s_second=0;

// MPU
static float    s_rf=0, s_pf=0, s_mag_prev=1.0f;
static uint32_t s_last_mpu=0;

// Thought / Emote bubble
typedef struct { const char* text; float y; uint32_t end_t; } ThoughtBubble;
static ThoughtBubble s_thought = {nullptr, 0, 0};
static uint32_t s_next_thought_t = 0;

// Debug
static uint32_t s_last_debug=0;
static float s_prev_val_dbg=0.5f, s_prev_irr_dbg=0.0f;
static const char* s_emotion_label = "neutral";

// ═══════════════════════════════════════════════════════════
//  TIME PROFILE
// ═══════════════════════════════════════════════════════════
static TimeProfile get_time_profile(int h) {
  if (h>=6  && h<12) return { 0.15f,  0.20f, -0.25f, 0.85f};
  if (h>=12 && h<18) return { 0.05f,  0.05f, -0.10f, 1.00f};
  if (h>=18 && h<21) return { 0.10f, -0.10f,  0.10f, 1.05f};
  if (h>=21 && h<23) return { 0.00f, -0.22f,  0.30f, 1.25f};
  return                    {-0.05f, -0.32f,  0.45f, 1.60f};
}

// ═══════════════════════════════════════════════════════════
//  PARTICLE SPAWNER
// ═══════════════════════════════════════════════════════════
static void spawnParticles(uint8_t type, int count) {
  int spawned = 0;
  for (int i = 0; i < MAX_PARTICLES && spawned < count; i++) {
    if (!s_particles[i].alive) {
      s_particles[i].x = random(18, 110);
      s_particles[i].y = (type == 0) ? (58 + random(0, 6)) : (EY + random(0, 12));
      s_particles[i].vx = (random(200) - 100) / 110.0f;
      s_particles[i].vy = -(0.35f + random(45) / 100.0f);
      s_particles[i].r = 2.0f + random(2);
      s_particles[i].phase = random(0, 628) / 100.0f;
      s_particles[i].type = type;
      s_particles[i].alive = true;
      s_particles[i].born = millis();
      spawned++;
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  MPU6050
// ═══════════════════════════════════════════════════════════
static void mpu_write(uint8_t r, uint8_t v) {
  Wire.beginTransmission(MPU_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission(true);
}

static bool mpu_begin() {
  mpu_write(0x6B, 0x00); delay(10);
  mpu_write(0x1A, 0x02);
  mpu_write(0x1B, 0x00);
  mpu_write(0x1C, 0x00);
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x75); Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 1, 1);
  uint8_t w = Wire.read();
  return (w == 0x68 || w == 0x70);
}

static void readInputs(float dt) {
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x3B); Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 14, 1);
  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();
  int16_t gx = (Wire.read() << 8) | Wire.read();
  int16_t gy = (Wire.read() << 8) | Wire.read();
  int16_t gz = (Wire.read() << 8) | Wire.read();

  float fax = ax / 16384.f, fay = ay / 16384.f, faz = az / 16384.f;
  float fgx = gx / 131.f,   fgy = gy / 131.f,   fgz = gz / 131.f;

  float ar = atan2f(fay, faz) * 57.3f;
  float ap = atan2f(-fax, sqrtf(fay * fay + faz * faz)) * 57.3f;
  s_rf = 0.94f * (s_rf + fgx * dt) + 0.06f * ar;
  s_pf = 0.94f * (s_pf + fgy * dt) + 0.06f * ap;

  g_input.tilt_x = g_input.tilt_x * 0.86f + fax * 0.14f;
  g_input.tilt_y = g_input.tilt_y * 0.86f + fay * 0.14f;

  float mag = sqrtf(fax * fax + fay * fay + faz * faz);
  g_input.jerk = fabsf(mag - s_mag_prev);
  s_mag_prev = mag;
  g_input.ax = fax; g_input.ay = fay; g_input.az = faz;

  // Tap detection (sharp spike with low sustained rotation)
  float gyro_mag = sqrtf(fgx * fgx + fgy * fgy + fgz * fgz);
  g_input.tapped = (g_input.jerk > 0.28f && gyro_mag < 90.0f);

  // Upside-down detection
  bool was_inv = g_input.inverted;
  g_input.inverted = (fabsf(s_pf) > 135.f || fabsf(s_rf) > 135.f);
  if (g_input.inverted && !was_inv) {
    s_inverted_since = millis();
    spawnParticles(0, 6);
  }
  if (!g_input.inverted) s_inverted_since = 0;

  // Motion features
  float accel_dev = fabsf(mag - 1.0f);
  s_me_energy = s_me_energy * 0.84f + accel_dev * 0.16f;
  g_motion.motion_energy = constrain(s_me_energy / 0.45f, 0.0f, 1.0f);

  s_me_rot = s_me_rot * 0.80f + gyro_mag * 0.20f;
  g_motion.rotation_speed = constrain(s_me_rot / 220.0f, 0.0f, 1.0f);

  if (s_me_energy < 0.10f && g_motion.rotation_speed < 0.12f) s_me_still_acc += dt;
  else s_me_still_acc *= 0.72f;
  float still_raw = constrain(s_me_still_acc / 8.0f, 0.0f, 1.0f);
  s_me_still = s_me_still * 0.90f + still_raw * 0.10f;
  g_motion.stillness = s_me_still;

  // Oscillation (gentle rocking)
  if ((millis() - s_osc_sample_t) > 75) {
    s_osc_sample_t = millis();
    s_osc_buf[s_osc_idx % OSC_WINDOW] = fax;
    s_osc_idx++;
    if (s_osc_idx >= OSC_WINDOW) {
      int sc = 0; float sa = 0;
      for (int i = 1; i < OSC_WINDOW; i++) {
        if (s_osc_buf[i] * s_osc_buf[i - 1] < -0.002f) sc++;
        sa += fabsf(s_osc_buf[i]);
      }
      float ma = sa / (OSC_WINDOW - 1);
      float ro = (sc / 6.5f) * constrain(ma / 0.11f, 0.0f, 1.0f);
      s_osc_score = s_osc_score * 0.68f + ro * 0.32f;
    }
  }
  g_motion.oscillation_score = constrain(s_osc_score, 0.0f, 1.0f);

  s_tilt_gaze_x = s_tilt_gaze_x * 0.88f + fax * 0.12f;
  s_tilt_gaze_y = s_tilt_gaze_y * 0.88f + fay * 0.12f;

  if (g_input.jerk > 0.18f) s_surprise_boost = 0.85f;
}

// ═══════════════════════════════════════════════════════════
//  MOTION EVENTS & INTERACTIONS
// ═══════════════════════════════════════════════════════════
static void pushMotionEvent(float da, float dv, float dc, float dur) {
  int slot = 0; float ml = 999.0f;
  for (int i = 0; i < MAX_MOTION_EVENTS; i++) {
    if (s_mevents[i].duration_left <= 0) { slot = i; break; }
    if (s_mevents[i].duration_left < ml) { ml = s_mevents[i].duration_left; slot = i; }
  }
  s_mevents[slot] = {da, dv, dc, dur, dur};
}

static void tickMotionEvents(float dt) {
  for (int i = 0; i < MAX_MOTION_EVENTS; i++) {
    if (s_mevents[i].duration_left <= 0) continue;
    s_mevents[i].duration_left -= dt;
    if (s_mevents[i].duration_left < 0) s_mevents[i].duration_left = 0;
    float t = s_mevents[i].duration_left / s_mevents[i].total_duration;
    float env = (t > 0.85f) ? (1.0f - t) / 0.15f : t / 0.85f;
    env = constrain(env, 0.0f, 1.0f);
    g_state.darousal   += s_mevents[i].arousal_delta   * env * dt * 4.0f;
    g_state.dvalence   += s_mevents[i].valence_delta   * env * dt * 3.0f * ZENO_HAPPY_SPEED;
    g_state.dcuriosity += s_mevents[i].curiosity_delta * env * dt * 4.0f;
  }
}

static void applyMotionEvents(uint32_t now) {
  float me = g_motion.motion_energy;
  float rs = g_motion.rotation_speed;
  float os = g_motion.oscillation_score;
  float st = g_motion.stillness;
  bool sleeping = (g_state.sleep_pressure > 0.75f);

  // 0. Sharp Tap / Poke -> Flinch + Annoyance
  if (!sleeping && g_input.tapped && (now - s_cd_tap) > 700) {
    s_cd_tap = now;
    s_flinch_timer = 0.65f;
    s_squish_v = -3.8f;
    g_state.irritation = constrain(g_state.irritation + 0.14f * ZENO_ANGER_SPEED, 0.0f, ZENO_MAX_ANGRY);
    pushMotionEvent(0.30f, -0.15f, 0.10f, 0.6f);
  }

  // 1. Shake / Rough handling
  if (!sleeping && me > 0.48f && (now - s_cd_shake) > 1100) {
    s_cd_shake = now;
    float inten = constrain((me - 0.48f) / 0.52f, 0.0f, 1.0f);
    if (g_state.valence > 0.65f && g_state.irritation < 0.25f) {
      // Playful bounce when already happy
      g_state.attachment = constrain(g_state.attachment + ZENO_PET_WARMTH * inten, 0.0f, 1.0f);
      pushMotionEvent(0.35f * inten, 0.15f * inten, 0.20f * inten, 0.7f);
    } else {
      // Gets dizzy/annoyed if shaken when neutral or grumpy
      g_state.irritation = constrain(g_state.irritation + 0.18f * inten * ZENO_ANGER_SPEED, 0.0f, ZENO_MAX_ANGRY);
      pushMotionEvent(0.40f * inten, -0.18f * inten, 0.05f, 0.8f);
    }
  }

  // 2. Smooth Tilt / Look around
  if (!sleeping && rs > 0.08f && me < 0.35f && (now - s_cd_rotate) > 750) {
    s_cd_rotate = now;
    float inten = constrain((rs - 0.08f) / 0.30f, 0.0f, 1.0f);
    pushMotionEvent(0.10f * inten, 0.04f * inten, 0.35f * inten, 0.7f);
  }

  // 3. Gentle Rocking / Petting (Oscillation) -> Bliss & Hearts
  if (!sleeping && os > 0.32f && (now - s_cd_oscillate) > 1600) {
    s_cd_oscillate = now;
    float inten = constrain((os - 0.32f) / 0.68f, 0.0f, 1.0f);
    g_state.irritation = max(0.0f, g_state.irritation - 0.16f * inten);
    g_state.attachment = constrain(g_state.attachment + 0.10f * inten, 0.0f, 1.0f);
    pushMotionEvent(0.12f * inten, 0.42f * inten, 0.15f * inten, 1.2f);
    if (g_state.valence > 0.75f && g_state.attachment > 0.75f) {
      spawnParticles(1, 2); // floating hearts
    }
  }

  // 4. Sudden freeze
  float edrop = s_prev_energy - me;
  if (!sleeping && edrop > 0.28f && me < 0.12f && (now - s_cd_stop) > 1400) {
    s_cd_stop = now;
    s_blink_vel = -15.0f;
    pushMotionEvent(0.05f, 0.0f, 0.28f, 0.45f);
  }
  s_prev_energy = me;

  // 5. Prolonged stillness -> Boredom / Daydreaming
  if (st > 0.55f && (now - s_cd_still) > 4500) {
    s_cd_still = now;
    g_state.attention_need = constrain(g_state.attention_need + 0.12f, ZENO_MIN_BORED, ZENO_MAX_BORED);
    pushMotionEvent(-0.12f, 0.0f, 0.12f, 1.8f);
  }
}

// ═══════════════════════════════════════════════════════════
//  UNIFIED SLEEP & STATE ENGINE
// ═══════════════════════════════════════════════════════════
static float getZNorm() {
  return constrain(cosf(s_pf * DEG_TO_RAD) * cosf(s_rf * DEG_TO_RAD), -1.f, 1.f);
}

static void updateCryArc(float dt, uint32_t now) {
  if (!s_crying && g_state.irritation >= ZENO_CRY_THRESHOLD) {
    s_crying = true;
    s_cry_start = now;
    s_cry_tear1 = 0;
    s_cry_tear2 = 7.0f;
    g_state.valence = max(ZENO_MIN_HAPPY, g_state.valence - 0.22f);
  }

  if (s_crying) {
    uint32_t elapsed = now - s_cry_start;
    g_state.valence = max(ZENO_MIN_HAPPY, g_state.valence - 0.02f * dt);
    // Rocking soothes crying faster
    float soothe = (g_motion.oscillation_score > 0.3f) ? 0.12f : 0.045f;
    g_state.irritation = max(0.0f, g_state.irritation - soothe * dt);

    if (elapsed > ZENO_MAX_CRY_MS || (elapsed > ZENO_MIN_CRY_MS && g_state.irritation < 0.18f)) {
      s_crying = false;
      g_state.valence = constrain(g_state.valence + 0.18f, ZENO_MIN_HAPPY, ZENO_MAX_HAPPY);
      g_state.attachment = constrain(g_state.attachment + 0.12f, 0.0f, 1.0f);
    }
    s_breath_rate = 0.045f + sinf(now * 0.004f) * 0.02f;
  }
}

static void updateThought(uint32_t now) {
  if (s_thought.text && now > s_thought.end_t) s_thought.text = nullptr;

  if (!s_thought.text && now > s_next_thought_t && !s_sleeping &&
      g_state.attention_need > 0.35f && g_motion.stillness > 0.45f) {
    const char* thoughts[] = {"...", "??", "!", "~", "hmm", "o.O", "..!"};
    const char* t;
    if (g_state.irritation > 0.45f)      t = "hmph";
    else if (g_state.valence < 0.35f)    t = "...";
    else if (g_state.attachment > 0.72f) t = "<3";
    else if (g_state.curiosity > 0.62f)  t = "??";
    else                                 t = thoughts[random(7)];

    s_thought = {t, 0.0f, now + 1400 + (uint32_t)random(700)};
    s_next_thought_t = now + 4500 + (uint32_t)random(7000);
  }
  if (s_thought.text) s_thought.y = min(8.0f, s_thought.y + 0.25f);
}

static void updateInternalState(float dt, uint32_t now) {
  TimeProfile tp = get_time_profile(s_hour);
  tickMotionEvents(dt);

  float jerk_norm = g_motion.motion_energy;

  // ── Arousal ──────────────────────────────────────────────
  float at = constrain(0.32f + tp.arousal_bias + jerk_norm * 0.55f, 0.0f, 1.0f);
  g_state.darousal += (at - g_state.arousal) * 2.5f * dt;
  g_state.darousal *= ZENO_MOOD_INERTIA;
  g_state.arousal = constrain(g_state.arousal + g_state.darousal * dt, 0.0f, 1.0f);

  // ── Irritation ───────────────────────────────────────────
  float irr_build = (jerk_norm > 0.32f) ? (jerk_norm - 0.32f) * 0.08f * tp.irritation_scale * ZENO_ANGER_SPEED * dt : 0.0f;
  float irr_decay = (jerk_norm < 0.10f) ? 0.045f * dt : 0.012f * dt;
  if (g_needs.hunger > 0.65f)  irr_build += 0.004f * dt;
  if (g_needs.fatigue > 0.75f) irr_build += 0.005f * dt;

  g_state.dirrit += (irr_build - irr_decay);
  g_state.dirrit *= ZENO_MOOD_INERTIA;
  g_state.irritation = constrain(g_state.irritation + g_state.dirrit, 0.0f, ZENO_MAX_ANGRY);

  // ── Valence ──────────────────────────────────────────────
  float valence_baseline = 0.58f + tp.valence_bias;
  float valence_push = -g_state.irritation * 0.45f
                       + g_state.attachment * 0.20f
                       + (g_state.arousal - 0.4f) * 0.10f;
  if (g_needs.hunger > 0.65f)  valence_push -= 0.08f;
  if (g_needs.fatigue > 0.75f) valence_push -= 0.06f;
  if (g_needs.social < 0.25f)  valence_push += 0.06f;

  float valence_target = constrain(valence_baseline + valence_push, ZENO_MIN_HAPPY, ZENO_MAX_HAPPY);
  g_state.dvalence += (valence_target - g_state.valence) * 1.6f * ZENO_HAPPY_SPEED * dt;
  g_state.dvalence *= ZENO_MOOD_INERTIA;
  g_state.dvalence += 0.004f * sinf(now * 0.0003f + s_phase_a) * dt;
  g_state.valence = constrain(g_state.valence + g_state.dvalence * dt, ZENO_MIN_HAPPY, ZENO_MAX_HAPPY);

  // ── Curiosity ────────────────────────────────────────────
  if ((now - s_curiosity_spike_t) > (uint32_t)(7000 + (int)(esp_random() % 10000))) {
    s_curiosity_spike_t = now;
    s_curiosity_spike = 0.25f + (esp_random() % ((int)(ZENO_CURIOUS_SPIKE * 100))) / 100.f;
  }
  s_curiosity_spike *= (1.0f - 0.75f * dt);
  float ct = constrain(s_curiosity_spike + g_motion.rotation_speed * 0.45f + jerk_norm * 0.15f, 0.0f, 1.0f);
  g_state.dcuriosity += (ct - g_state.curiosity) * 2.2f * dt;
  g_state.dcuriosity *= 0.78f;
  g_state.curiosity = constrain(g_state.curiosity + g_state.dcuriosity * dt, 0.0f, 1.0f);

  // ── Unified Sleep Engine ─────────────────────────────────
  float z = getZNorm();
  bool face_down = (z > 0.45f);
  s_trying_sleep = (g_motion.stillness > 0.45f && z > 0.25f) ||
                   (g_state.sleep_pressure > 0.50f && g_motion.stillness > 0.25f);

  if (!s_sleeping) {
    if (s_trying_sleep || (face_down && g_motion.stillness > 0.5f)) {
      g_state.sleep_pressure += (0.18f + max(0.0f, tp.sleep_bias) * 0.25f) * dt;
      if (g_state.sleep_pressure > 0.78f) {
        s_sleeping = true;
        s_sleep_start = now;
        s_sleep_dur = 600000 + (esp_random() % 300000);
      }
    } else {
      float wake_drive = g_motion.motion_energy * 0.85f + g_motion.rotation_speed * 0.55f;
      g_state.sleep_pressure -= (0.14f + wake_drive) * dt;
    }
  } else {
    if ((now - s_sleep_start) > s_sleep_dur) {
      s_sleeping = false;
      g_state.sleep_pressure = 0.25f;
    }
    if (g_motion.motion_energy > 0.48f || g_input.tapped) {
      s_sleeping = false;
      g_state.sleep_pressure = 0.18f;
      if (esp_random() % 100 < 55) g_state.irritation = constrain(g_state.irritation + 0.22f, 0.0f, ZENO_MAX_ANGRY);
    }
  }
  g_state.sleep_pressure = constrain(g_state.sleep_pressure, 0.0f, 1.0f);

  // ── Attachment & Attention ───────────────────────────────
  float att = 0.42f + g_state.valence * 0.30f - g_state.irritation * 0.38f;
  g_state.attachment = constrain(g_state.attachment + (att - g_state.attachment) * 0.22f * dt, 0.0f, 1.0f);

  float an_chg = (g_motion.stillness > 0.45f) ? 0.016f * g_motion.stillness * dt : -0.07f * dt;
  g_state.attention_need = constrain(g_state.attention_need + an_chg, ZENO_MIN_BORED, ZENO_MAX_BORED);

  s_phase_a += 0.00017f; s_phase_b += 0.00011f; s_phase_c += 0.00023f;
}

// ═══════════════════════════════════════════════════════════
//  RESOLVE BEHAVIOUR (Physics, Saccades & Micro-expressions)
// ═══════════════════════════════════════════════════════════
static void resolveBehaviour(float dt, uint32_t now) {
  float br = 0.015f + g_state.arousal * 0.055f;
  s_breath_rate += (br - s_breath_rate) * 0.5f * dt;
  s_breath = (sinf(now * s_breath_rate * 0.001f) + 1.0f) * 0.5f;

  float ear_speed = max(0.01f, 0.02f + (g_state.valence - 0.5f) * 0.16f + g_state.arousal * 0.08f);
  s_ear_phase += ear_speed;

  // Squish spring
  if (g_input.jerk > 0.12f) s_squish_v = -1.8f * g_input.jerk;
  s_squish_v += (1.0f - s_squish) * 24.0f * dt;
  s_squish_v *= (1.0f - 7.5f * dt);
  s_squish = constrain(s_squish + s_squish_v * dt, 0.62f, 1.38f);

  // Macro Gaze Target
  float li = max(450.f, 3800.f - g_state.arousal * 2400.f - g_state.curiosity * 900.f);
  if ((now - s_look_t) > (uint32_t)li) {
    s_look_t = now;
    float cx = (g_state.curiosity - 0.5f) * 0.5f;
    float cy = -g_state.curiosity * 0.25f;
    float rx = ((float)((int)(esp_random() % 200) - 100)) / 100.f;
    float ry = ((float)((int)(esp_random() % 160) - 80)) / 100.f;
    s_tpx = constrain(rx * 0.75f + cx, -1.f, 1.f);
    s_tpy = constrain(ry * 0.75f + cy, -1.f, 1.f);
    if (g_state.valence < 0.35f) s_tpy += 0.35f;
    if (g_state.irritation > 0.5f) { s_tpx *= 0.18f; s_tpy *= 0.10f; }
  }

  // Micro-saccades (tiny rapid eye darts while awake)
  if (!s_sleeping && (now - s_saccade_t) > (uint32_t)(280 + (esp_random() % 650))) {
    s_saccade_t = now;
    float amp = (g_state.arousal > 0.5f || g_state.curiosity > 0.6f) ? 0.14f : 0.06f;
    s_saccade_ox = ((float)((int)(esp_random() % 200) - 100) / 100.0f) * amp;
    s_saccade_oy = ((float)((int)(esp_random() % 200) - 100) / 100.0f) * amp;
  }

  // Pupil spring
  float sp = 0.10f + g_state.arousal * 0.20f;
  float dp = 0.66f + g_state.sleep_pressure * 0.18f;
  float rb = constrain(g_motion.rotation_speed * 2.8f + fabsf(s_tilt_gaze_x) * 0.6f, 0.0f, 0.75f);
  float gtx = s_tpx * (1.0f - rb) + s_tilt_gaze_x * rb + s_saccade_ox;
  float gty = s_tpy * (1.0f - rb) + s_tilt_gaze_y * rb + s_saccade_oy;
  s_pvx = (s_pvx + (gtx - s_px) * sp) * dp;
  s_pvy = (s_pvy + (gty - s_py) * sp) * dp;
  s_px += s_pvx;
  s_py += s_pvy;

  s_dizzy_angle += 0.11f + g_state.arousal * 0.05f;

  // Happy bounce & Sad droop
  float hbi = max(0.0f, g_state.valence - 0.65f);
  if (hbi > 0 && !s_sleeping) s_happy_bounce = sinf(now * 0.011f) * hbi * 3.8f;
  else s_happy_bounce *= 0.86f;

  float sadness = max(0.0f, 0.50f - g_state.valence);
  s_sad_droop += (sadness * 4.5f - s_sad_droop) * 0.06f;

  // Wink when playful
  if (!s_wink_active && !s_sleeping && g_state.valence > 0.78f && g_state.arousal > 0.50f &&
      (now - s_wink_end) > 7500 && (esp_random() % 1500) < 3) {
    s_wink_active = true;
    s_wink_end = now + 380 + (uint32_t)random(180);
  }
  if (s_wink_active && now > s_wink_end) s_wink_active = false;

  // Yawn when drowsy
  if (s_trying_sleep && g_state.sleep_pressure > 0.45f && !s_sleeping && !s_crying) {
    s_yawn_open = min(13.0f, s_yawn_open + 0.45f * dt * 60.0f);
  } else {
    s_yawn_open = max(0.0f, s_yawn_open - 0.55f * dt * 60.0f);
  }

  s_surprise_boost *= 0.86f;
  if (s_flinch_timer > 0.0f) s_flinch_timer = max(0.0f, s_flinch_timer - dt);

  // Particle physics (gravity-aware)
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!s_particles[i].alive) continue;
    s_particles[i].x += s_particles[i].vx + sinf(now * 0.004f + s_particles[i].phase) * 0.18f + g_input.tilt_x * 0.4f;
    s_particles[i].y += s_particles[i].vy;
    if (s_particles[i].type == 0 && !g_input.inverted) s_particles[i].vy += 0.045f;
    if (s_particles[i].y < -8 || s_particles[i].y > 76 || (now - s_particles[i].born) > 3800) {
      s_particles[i].alive = false;
    }
  }

  // Needs (1Hz)
  static uint32_t needs_t = 0;
  if (now - needs_t > 1000) {
    needs_t = now;
    g_needs.hunger = min(1.0f, g_needs.hunger + 0.006f);
    if (!s_sleeping) g_needs.fatigue = min(1.0f, g_needs.fatigue + 0.010f);
    else             g_needs.fatigue = max(0.0f, g_needs.fatigue - 0.022f);

    if (g_motion.stillness > 0.5f) g_needs.social = min(1.0f, g_needs.social + 0.015f);
    else                           g_needs.social = max(0.0f, g_needs.social - 0.028f);
  }
}

// ═══════════════════════════════════════════════════════════
//  MAP STATE → ORGANIC FACE PARAMS
// ═══════════════════════════════════════════════════════════
static void mapStateToFace(uint32_t now) {
  float v  = g_state.valence;
  float a  = g_state.arousal;
  float ir = g_state.irritation;
  float sp = g_state.sleep_pressure;
  float cu = g_state.curiosity;
  float at = g_state.attachment;

  // Base openness
  float target_open = constrain(1.0f - sp * 1.15f + a * 0.25f + s_surprise_boost * 0.45f, 0.0f, 1.0f);
  if (s_crying) target_open *= 0.52f;
  if (s_flinch_timer > 0.0f) target_open = 0.12f; // tight flinch shut

  // Asymmetry & Curiosity head-tilt
  float asym = 0.06f * sinf(now * 0.00008f + s_phase_c) + cu * 0.12f;
  float open_l = constrain(target_open + asym, 0.0f, 1.0f);
  float open_r = constrain(target_open - asym * 0.6f, 0.0f, 1.0f);
  if (s_wink_active) open_r = 0.0f;

  s_sopen_l += (open_l - s_sopen_l) * 0.28f;
  s_sopen_r += (open_r - s_sopen_r) * 0.28f;
  g_face.eye_open_l = s_sopen_l;
  g_face.eye_open_r = s_sopen_r;

  // Organic lower-lid cheek push (smiling eyes)
  float target_smile = 0.0f;
  if (v > 0.65f && ir < 0.25f && !s_crying) {
    target_smile = constrain((v - 0.65f) * 2.2f + g_motion.oscillation_score * 0.35f, 0.0f, 0.85f);
  }
  s_ssmile += (target_smile - s_ssmile) * 0.14f;
  g_face.smile_curve = s_ssmile;

  // Brow angle: -1 (sad/worried) .. 0 (neutral) .. +1 (angry)
  float target_brow = 0.0f;
  if (ir > 0.20f) target_brow = constrain((ir - 0.20f) * 1.6f, 0.0f, 1.0f);
  else if (v < 0.42f || s_crying) target_brow = -constrain((0.42f - v) * 2.4f + (s_crying ? 0.4f : 0.0f), 0.0f, 1.0f);
  s_sbrow += (target_brow - s_sbrow) * 0.15f;
  g_face.brow_angle = s_sbrow;

  // Curiosity vertical eye offset (gives a "head tilt" feel on fixed screen)
  g_face.look_tilt_y = (cu > 0.55f) ? sinf(now * 0.002f) * (cu - 0.55f) * 6.0f : 0.0f;

  // Pupil size
  float ps = 0.92f + a * 0.50f + at * 0.28f - ir * 0.28f - sp * 0.45f + s_surprise_boost * 0.40f;
  if (s_crying) ps -= 0.22f;
  ps = constrain(ps, 0.38f, 1.75f);
  s_spsize += (ps - s_spsize) * 0.10f;
  g_face.pupil_size = s_spsize;

  // Pupil gaze
  g_face.pupil_x = constrain(s_px, -1.0f, 1.0f);
  g_face.pupil_y = constrain(s_py + sp * 0.30f - cu * 0.15f + (s_crying ? 0.32f : 0.0f), -1.0f, 1.0f);

  // Jitter
  float jt = ir * 0.55f + a * 0.12f + (s_crying ? 0.18f : 0.0f);
  s_sjitter += (jt - s_sjitter) * 0.12f;
  g_face.jitter = s_sjitter;

  // Squish
  g_face.squish_x = s_squish;
  g_face.squish_y = 1.0f / max(0.65f, s_squish);

  // Special states
  g_face.heart_pupils = (at > 0.80f && v > 0.78f && !s_crying);
  g_face.star_pupils  = (cu > 0.82f && a > 0.62f && !s_crying);
  g_face.rosy_cheeks  = (at > 0.58f && v > 0.58f) || (v > 0.72f && g_motion.oscillation_score > 0.28f);
  g_face.dizzy_mode   = (g_input.inverted && (millis() - s_inverted_since) > 1800);
  g_face.sweat_drop   = (ir > 0.45f && ir < ZENO_CRY_THRESHOLD && !s_crying);

  // Plain-English Emotion Label
  if (s_crying)                         s_emotion_label = "crying";
  else if (s_flinch_timer > 0.0f)       s_emotion_label = "flinching";
  else if (g_face.dizzy_mode)           s_emotion_label = "dizzy";
  else if (sp > 0.80f)                  s_emotion_label = "deep_sleep";
  else if (sp > 0.58f)                  s_emotion_label = "sleepy";
  else if (g_face.heart_pupils)         s_emotion_label = "in_love";
  else if (g_face.star_pupils)          s_emotion_label = "starstruck";
  else if (ir > 0.55f)                  s_emotion_label = "furious";
  else if (ir > 0.32f)                  s_emotion_label = "grumpy";
  else if (v > 0.80f && a > 0.55f)      s_emotion_label = "ecstatic";
  else if (v > 0.70f)                   s_emotion_label = "happy";
  else if (v < 0.30f)                   s_emotion_label = "sad";
  else if (cu > 0.65f)                  s_emotion_label = "curious";
  else if (g_state.attention_need > 0.65f) s_emotion_label = "bored";
  else                                  s_emotion_label = "content";
}

// ═══════════════════════════════════════════════════════════
//  BLINK
// ═══════════════════════════════════════════════════════════
static void updateBlink(uint32_t now) {
  if (s_blink_lid < 0.05f && now > s_next_blink && !s_sleeping) {
    float bms = 4200.f - g_state.arousal * 2200.f + g_state.sleep_pressure * 1200.f;
    float nms = (float)((int)(esp_random() % 1800) - 900);
    uint32_t iv = (uint32_t)max(650.f, bms + nms);
    s_blink_seq = (esp_random() % 100 < 18) ? 2 : 1; // 18% double-blink
    s_next_blink = now + iv;
    s_blink_vel = 19.0f; // closing velocity
  }
  if (s_blink_vel > 0.0f) {
    s_blink_lid += s_blink_vel * 0.016f;
    if (s_blink_lid >= 1.0f) {
      s_blink_lid = 1.0f;
      s_blink_vel = -16.0f; // snap open
    }
  } else if (s_blink_lid > 0.0f) {
    s_blink_lid += s_blink_vel * 0.016f;
    if (s_blink_lid <= 0.0f) {
      s_blink_lid = 0.0f;
      s_blink_vel = 0.0f;
      if (s_blink_seq == 2) {
        s_next_blink = now + 90 + (uint32_t)(esp_random() % 60);
        s_blink_seq = 1;
      } else {
        s_blink_seq = 0;
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  ORGANIC DRAWING HELPERS
// ═══════════════════════════════════════════════════════════
static void draw_star_pupil(int cx, int cy, int r) {
  oled->fillCircle(cx, cy, max(2, r - 2), SSD1306_BLACK);
  for (int d = 0; d < 4; d++) {
    float ang = d * 1.5708f + millis() * 0.004f;
    oled->drawLine(cx, cy, cx + (int)((r + 2) * cosf(ang)), cy + (int)((r + 2) * sinf(ang)), SSD1306_BLACK);
  }
}

static void draw_heart_shape(int cx, int cy, int size, uint16_t color) {
  oled->fillCircle(cx - size / 2, cy - size / 4, size / 2, color);
  oled->fillCircle(cx + size / 2, cy - size / 4, size / 2, color);
  oled->fillTriangle(cx - size, cy, cx + size, cy, cx, cy + size, color);
}

static void draw_rosy(int ex, int ey, bool right_eye) {
  int cx = right_eye ? ex + EW - 4 : ex - 3;
  int cy = ey + EH + 1;
  // Neat diagonal blush hash marks
  oled->drawLine(cx,     cy, cx + 2, cy - 2, SSD1306_WHITE);
  oled->drawLine(cx + 3, cy, cx + 5, cy - 2, SSD1306_WHITE);
}

static void draw_ears(int emo_y) {
  float v = g_state.valence;
  if (v < 0.58f || s_crying || s_sleeping) return;
  int amp = (int)(3.5f * fabsf(sinf(s_ear_phase)) * ((v - 0.58f) * 3.5f));
  if (amp < 1) return;
  int y1 = EY + emo_y + 6;
  int y2 = EY + emo_y + EH - 6;
  oled->drawLine(ELX - 3, y1, ELX - 3 - amp, y1 - 4, SSD1306_WHITE);
  oled->drawLine(ELX - 3, y2, ELX - 3 - amp, y2 + 4, SSD1306_WHITE);
  oled->drawLine(ERX + EW + 3, y1, ERX + EW + 3 + amp, y1 - 4, SSD1306_WHITE);
  oled->drawLine(ERX + EW + 3, y2, ERX + EW + 3 + amp, y2 + 4, SSD1306_WHITE);
}

static void draw_teardrop(int cx, int base_y, float& ty, float spd) {
  ty += spd;
  if (ty > 18.0f) ty = 0.0f;
  int y = base_y + (int)ty;
  oled->fillCircle(cx, y + 2, 2, SSD1306_WHITE);
  oled->fillTriangle(cx - 2, y + 2, cx + 2, y + 2, cx, y - 2, SSD1306_WHITE);
}

// ═══════════════════════════════════════════════════════════
//  DRAW ONE ORGANIC EYE
// ═══════════════════════════════════════════════════════════
static void draw_one_eye(int ex, int ey_base, bool right) {
  int ew2 = (int)(EW * g_face.squish_x);
  int eh2 = (int)(EH * g_face.squish_y);
  int ecx = ex + EW / 2;
  int ecy = ey_base + EH / 2;
  int rx  = ecx - ew2 / 2;
  int ry  = ecy - eh2 / 2;

  int jx = (int)(g_face.jitter * sinf(millis() * 0.031f + (right ? 1.3f : 0.0f)) * 3.0f);
  int jy = (int)(g_face.jitter * cosf(millis() * 0.027f + (right ? 0.7f : 0.0f)) * 2.0f);
  rx += jx; ry += jy; ecx += jx; ecy += jy;

  // 1. Dizzy Spiral Dots
  if (g_face.dizzy_mode) {
    oled->drawRoundRect(rx, ry, ew2, eh2, ER, SSD1306_WHITE);
    for (int r2 = 3; r2 <= 11; r2 += 4) {
      float ang = s_dizzy_angle + (right ? 1.6f : 0.0f) + r2 * 0.45f;
      oled->fillCircle(ecx + (int)(r2 * cosf(ang)), ecy + (int)(r2 * sinf(ang)), 2, SSD1306_WHITE);
    }
    return;
  }

  // 2. Closed Eye States (Sleep / Blink / Flinch)
  float eo = right ? g_face.eye_open_r : g_face.eye_open_l;
  float lid_total = constrain(max(1.0f - eo, s_blink_lid), 0.0f, 1.0f);

  if (lid_total >= 0.92f) {
    int ly = ecy + 2;
    if (g_state.sleep_pressure > ZENO_SLEEP_THRESH) {
      // Peaceful downward U-arc
      for (int i = 2; i < ew2 - 2; i++) {
        float t = (float)(i - ew2 / 2) / (ew2 / 2);
        int dy2 = (int)(3.5f * (1.0f - t * t));
        oled->drawPixel(rx + i, ly + dy2, SSD1306_WHITE);
        oled->drawPixel(rx + i, ly + dy2 + 1, SSD1306_WHITE);
      }
    } else if (s_flinch_timer > 0.0f) {
      // >< Tight Flinch Chevron
      int x_in  = right ? rx + 4 : rx + ew2 - 4;
      int x_out = right ? rx + ew2 - 4 : rx + 4;
      oled->drawLine(x_out, ly - 5, x_in, ly, SSD1306_WHITE);
      oled->drawLine(x_in, ly, x_out, ly + 5, SSD1306_WHITE);
      oled->drawLine(x_out, ly - 4, x_in, ly + 1, SSD1306_WHITE);
      oled->drawLine(x_in, ly + 1, x_out, ly + 6, SSD1306_WHITE);
    } else if (g_face.smile_curve > 0.35f) {
      // ^ ^ Happy Arch Blink
      for (int i = 2; i < ew2 - 2; i++) {
        float t = (float)(i - ew2 / 2) / (ew2 / 2);
        int dy2 = -(int)(4.0f * (1.0f - t * t));
        oled->drawPixel(rx + i, ly + dy2, SSD1306_WHITE);
        oled->drawPixel(rx + i, ly + dy2 + 1, SSD1306_WHITE);
      }
    } else {
      // Clean horizontal closed lid
      oled->drawFastHLine(rx + 3, ly, ew2 - 6, SSD1306_WHITE);
      oled->drawFastHLine(rx + 3, ly + 1, ew2 - 6, SSD1306_WHITE);
    }
    return;
  }

  // 3. Base White Sclera
  oled->fillRoundRect(rx, ry, ew2, eh2, ER, SSD1306_WHITE);

  // 4. Pupil (drawn before lid masks so lids naturally clip it)
  int max_ox = (ew2 - PW) / 2 - 2;
  int max_oy = (eh2 - PH) / 2 - 2;
  float eye_convergence = right ? -0.08f : 0.08f; // slight cute cross-eye focus
  float gaze_x = constrain(g_face.pupil_x + eye_convergence, -1.0f, 1.0f);
  int pcx = ecx + (int)(gaze_x * max_ox);
  int pcy = ecy + (int)(g_face.pupil_y * max_oy);

  int pw2 = constrain((int)(PW * g_face.pupil_size), 5, ew2 - 6);
  int ph2 = constrain((int)(PH * g_face.pupil_size), 5, eh2 - 6);

  if (g_face.heart_pupils) {
    draw_heart_shape(pcx, pcy, 6, SSD1306_BLACK);
  } else if (g_face.star_pupils) {
    draw_star_pupil(pcx, pcy, pw2 / 2);
  } else {
    oled->fillRoundRect(pcx - pw2 / 2, pcy - ph2 / 2, pw2, ph2, PR, SSD1306_BLACK);
    // Double specular catchlights for lifelike depth
    oled->fillCircle(pcx - pw2 / 4, pcy - ph2 / 4, 2, SSD1306_WHITE);
    oled->drawPixel(pcx + pw2 / 5, pcy + ph2 / 5, SSD1306_WHITE);
  }

  // 5. Upper Eyelid Mask (flat drop + angled brow slant)
  int lid_px = (int)(lid_total * eh2);
  if (lid_px > 0) {
    oled->fillRect(rx, ry, ew2, lid_px, SSD1306_BLACK);
  }

  // Angled Brow Cutout (carves directly into top of eye for crisp expressions)
  if (fabsf(g_face.brow_angle) > 0.08f) {
    int slant = (int)(g_face.brow_angle * 11.0f); // +angry, -sad
    int top_y = ry + lid_px;
    if (slant > 0) {
      // Angry inner slant
      if (right) oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx, top_y + slant, SSD1306_BLACK);
      else       oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx + ew2, top_y + slant, SSD1306_BLACK);
    } else {
      // Sad/worried outer slant
      int s = -slant;
      if (right) oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx + ew2, top_y + s, SSD1306_BLACK);
      else       oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx, top_y + s, SSD1306_BLACK);
    }
  }

  // 6. Lower Eyelid Cheek Push (Curves bottom of eye upward when happy!)
  if (g_face.smile_curve > 0.05f) {
    int push = (int)(g_face.smile_curve * 12.0f);
    int bot_y = ry + eh2;
    oled->fillCircle(ecx, bot_y + (ew2 / 2) - push, ew2 / 2 + 2, SSD1306_BLACK);
  }

  if (g_face.rosy_cheeks) draw_rosy(rx, ry, right);
}

// ═══════════════════════════════════════════════════════════
//  RENDER FRAME
// ═══════════════════════════════════════════════════════════
static void renderFrame(uint32_t now) {
  oled->clearDisplay();

  int fury_x = (g_state.irritation > 0.55f) ? (int)(sinf(now * 0.028f) * g_state.irritation * 3.5f) : 0;
  int emo_y  = (int)(-s_happy_bounce + s_sad_droop);
  int tilt_l = (int)(-g_face.look_tilt_y);
  int tilt_r = (int)(g_face.look_tilt_y);

  draw_one_eye(ELX + fury_x, EY + emo_y + tilt_l, false);
  draw_one_eye(ERX + fury_x, EY + emo_y + tilt_r, true);

  draw_ears(emo_y);

  // Crying Tears
  if (s_crying) {
    float spd = 0.28f + g_state.irritation * 0.75f;
    draw_teardrop(ELX + EW / 2, EY + emo_y + EH - 2, s_cry_tear1, spd);
    draw_teardrop(ERX + EW / 2, EY + emo_y + EH - 2, s_cry_tear2, spd * 1.08f);
  }

  // Stress Sweat Drop (top-right of right eye when irritated but not yet crying)
  if (g_face.sweat_drop) {
    int sy = EY + emo_y + 2 + ((now / 120) % 6);
    int sx = ERX + EW + 5;
    oled->fillCircle(sx, sy + 3, 2, SSD1306_WHITE);
    oled->fillTriangle(sx - 2, sy + 3, sx + 2, sy + 3, sx, sy - 2, SSD1306_WHITE);
  }

  // Yawn Oval
  if (s_yawn_open > 1.2f) {
    int yw = (int)(s_yawn_open * 1.8f);
    int yh = (int)(s_yawn_open * 0.9f);
    oled->fillRoundRect(64 - yw / 2, EY + EH + 4, yw, yh, 4, SSD1306_WHITE);
  }

  // Floating Particles (Bubbles & Hearts)
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!s_particles[i].alive) continue;
    int px = (int)s_particles[i].x;
    int py = (int)s_particles[i].y;
    if (s_particles[i].type == 0) {
      oled->drawCircle(px, py, (int)s_particles[i].r, SSD1306_WHITE);
      oled->drawPixel(px - 1, py - 1, SSD1306_WHITE);
    } else {
      draw_heart_shape(px, py, 3, SSD1306_WHITE);
    }
  }

  // Floating Sine-Wave Sleep Z's
  if (g_state.sleep_pressure > ZENO_SLEEP_THRESH && g_state.arousal < 0.22f) {
    float zt = (now % 2400) / 2400.0f;
    int zx = ERX + EW - 4 + (int)(sinf(zt * 6.28f) * 3.0f);
    int zy = EY + 10 - (int)(zt * 12.0f);
    oled->setTextSize(1);
    oled->setTextColor(SSD1306_WHITE);
    oled->setCursor(zx, zy);
    oled->print(zt > 0.5f ? "Z" : "z");
  }

  // Thought Bubble
  if (s_thought.text) {
    int ty = max(1, EY - 12 - (int)s_thought.y);
    oled->setTextSize(1);
    oled->setTextColor(SSD1306_WHITE);
    oled->setCursor(54, ty);
    oled->print(s_thought.text);
  }

  // Subtle Hunger Fork Icon (bottom right)
  if (g_needs.hunger > 0.72f && !s_sleeping) {
    oled->drawFastVLine(120, 54, 9, SSD1306_WHITE);
    oled->drawFastVLine(122, 54, 5, SSD1306_WHITE);
    oled->drawFastVLine(124, 54, 9, SSD1306_WHITE);
  }

  oled->display();
}

// ═══════════════════════════════════════════════════════════
//  TELEMETRY & DEBUG
// ═══════════════════════════════════════════════════════════
static void printBar(float v, const char* label) {
  int n = constrain((int)(v * 10.0f + 0.5f), 0, 10);
  Serial.print(label);
  Serial.print("[");
  for (int i = 0; i < 10; i++) Serial.print(i < n ? "*" : " ");
  Serial.printf("] %.2f\n", v);
}

void send_state_packet() {
  Serial.print("DATA,");
  Serial.print(g_motion.motion_energy, 3);     Serial.print(",");
  Serial.print(g_motion.rotation_speed, 3);    Serial.print(",");
  Serial.print(g_motion.oscillation_score, 3); Serial.print(",");
  Serial.print(g_motion.stillness, 3);         Serial.print(",");
  Serial.print(g_input.jerk, 3);               Serial.print(",");
  Serial.print(g_input.tilt_x, 3);             Serial.print(",");
  Serial.print(g_input.tilt_y, 3);             Serial.print(",");
  Serial.print(g_state.valence, 3);            Serial.print(",");
  Serial.print(g_state.arousal, 3);            Serial.print(",");
  Serial.print(g_state.irritation, 3);         Serial.print(",");
  Serial.print(g_state.sleep_pressure, 3);     Serial.print(",");
  Serial.print(g_state.curiosity, 3);          Serial.print(",");
  Serial.print(g_state.attachment, 3);
  Serial.println();
}

static void debugPrint(uint32_t now) {
  if (ZENO_DEBUG_MS == 0 || (now - s_last_debug) < (uint32_t)ZENO_DEBUG_MS) return;
  s_last_debug = now;

  Serial.println(F("╔══════════ ZENO v13 ══════════╗"));
  Serial.printf("  EMOTION : %s\n", s_emotion_label);
  printBar(g_state.valence,        "  valence ");
  printBar(g_state.arousal,        "  arousal ");
  printBar(g_state.irritation,     "  irritate");
  printBar(g_state.sleep_pressure, "  sleep   ");
  printBar(g_state.curiosity,      "  curiosty");
  printBar(g_state.attachment,     "  attach  ");
  Serial.printf("  pitch=%.1f  roll=%.1f  jerk=%.3f\n", s_pf, s_rf, g_input.jerk);
  Serial.println(F("╚══════════════════════════════╝\n"));

  s_prev_val_dbg = g_state.valence;
  s_prev_irr_dbg = g_state.irritation;
}

// ═══════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\nZENO Life5 — zenov13 Living Soul Build"));

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  oled = new Adafruit_SSD1306(128, 64, &Wire, -1);
  if (!oled->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED FAIL"));
    while (1) delay(1000);
  }

  bool mpu_ok = mpu_begin();
  Serial.printf("MPU: %s\n", mpu_ok ? "OK" : "FAIL");

  configTime(5 * 3600 + 1800, 0, "pool.ntp.org", "time.google.com");
  struct tm ti = {};
  bool rtc_valid = false;
  if (getLocalTime(&ti) && ti.tm_year > 120) {
    rtc_valid = true;
    s_hour = ti.tm_hour; s_minute = ti.tm_min; s_second = ti.tm_sec;
  }

  WiFi.mode(WIFI_STA);
  bool connected = false;
  for (uint8_t a = 0; a < 2 && !connected; a++) {
    if (a > 0) { WiFi.disconnect(true); delay(1500); }
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (uint8_t t = 0; t < 15 && WiFi.status() != WL_CONNECTED; t++) delay(400);
    if (WiFi.status() == WL_CONNECTED) connected = true;
  }
  if (connected) {
    for (uint8_t i = 0; i < 15 && !getLocalTime(&ti); i++) delay(400);
    if (getLocalTime(&ti) && ti.tm_year > 120) {
      s_hour = ti.tm_hour; s_minute = ti.tm_min; s_second = ti.tm_sec;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else if (!rtc_valid) {
    s_hour = 12; s_minute = 0;
  }

  TimeProfile tp = get_time_profile(s_hour);
  g_state.sleep_pressure = constrain(tp.sleep_bias + 0.15f, 0.0f, 1.0f);
  g_state.arousal        = constrain(0.32f + tp.arousal_bias, 0.0f, 1.0f);
  g_state.valence        = constrain(0.60f + tp.valence_bias, ZENO_MIN_HAPPY, ZENO_MAX_HAPPY);

  s_last_mpu = millis();
  s_curiosity_spike_t = millis();
  s_next_blink = millis() + 1800;
  s_osc_sample_t = millis();
  s_next_thought_t = millis() + 4500;
  memset(s_osc_buf, 0, sizeof(s_osc_buf));
  memset(s_mevents, 0, sizeof(s_mevents));
  memset(s_particles, 0, sizeof(s_particles));
}

void loop() {
  uint32_t now = millis();

  static uint32_t sec_t = 0;
  if (now - sec_t > 1000) {
    sec_t = now;
    s_second = (s_second + 1) % 60;
    if (s_second == 0) {
      s_minute = (s_minute + 1) % 60;
      if (s_minute == 0) s_hour = (s_hour + 1) % 24;
    }
  }

  static uint32_t last_frame = 0;
  float dt = (now - last_frame) * 0.001f;
  if (dt <= 0 || dt > 0.2f) dt = 0.016f;
  last_frame = now;

  static uint32_t mt = 0;
  if (now - mt > 16) {
    float mpu_dt = (now - s_last_mpu) * 0.001f;
    s_last_mpu = now;
    if (mpu_dt > 0 && mpu_dt < 0.5f) readInputs(mpu_dt);
    mt = now;
  }

  applyMotionEvents(now);
  updateInternalState(dt, now);
  updateCryArc(dt, now);
  updateThought(now);
  resolveBehaviour(dt, now);
  mapStateToFace(now);
  updateBlink(now);
  debugPrint(now);
  renderFrame(now);
  send_state_packet();
}
