// ============================================================
//  KIRO  —  "kirov15"   Cute Sleep & Living Quirks Build
//
//  WHAT'S NEW vs zenov14:
//  • Adorable Sleep Arc:
//    - Drowsy "nodding off" head-bob & drooping pupils
//    - Breathing-synced anime sleep bubble (inflates/deflates)
//      that pops when woken up!
//    - Curved sleeping lashes + occasional REM dream twitches
//  • New Autonomous Quirks:
//    - Hiccups (after eating/drinking too fast!)
//    - Humming / Singing with floating ♪ ♫ music notes
//    - Cute Sneeze build-up (> <) + burst
//    - Shy Peek-a-Boo when tilted away
//  • Rebranded cleanly for Kiro.
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
//  ██  KIRO PERSONALITY & BEHAVIOUR KNOBS  ██
// ═══════════════════════════════════════════════════════════
#define KIRO_MAX_ANGRY       0.80f   // irritation ceiling
#define KIRO_CRY_THRESHOLD   0.68f   // irritation that triggers crying
#define KIRO_ANGER_SPEED     1.15f   // irritation build multiplier
#define KIRO_MIN_HAPPY       0.10f   // valence floor
#define KIRO_MAX_HAPPY       1.00f   // valence ceiling
#define KIRO_HAPPY_SPEED     1.35f   // valence recovery/rise speed
#define KIRO_MIN_BORED       0.00f   // boredom floor
#define KIRO_MAX_BORED       0.92f   // boredom ceiling
#define KIRO_SLEEP_THRESH    0.66f   // sleep_pressure threshold for sleep
#define KIRO_CURIOUS_SPIKE   0.68f   // max autonomous curiosity burst
#define KIRO_PET_WARMTH      0.20f   // attachment gained per gentle rock/pet
#define KIRO_MOOD_INERTIA    0.84f   // emotional momentum (0..0.95)
#define KIRO_CHAOS_RATE      1.25f   // multiplier for random quirks/celebrations
#define KIRO_DEBUG_MS        0       // serial debug interval ms (0 = off)

#define KIRO_MIN_CRY_MS      5000
#define KIRO_MAX_CRY_MS      12000

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
#define EH   34
#define ER    9
#define ELX  12
#define ERX  74
#define EY   11
#define PW   17
#define PH   18
#define PR    5

// ═══════════════════════════════════════════════════════════
//  ENUMS & STRUCTS
// ═══════════════════════════════════════════════════════════
enum AutoAction {
  ACT_NONE = 0,
  ACT_EATING,
  ACT_DRINKING,
  ACT_CELEBRATE,
  ACT_ZOOMIES,
  ACT_BLEP,
  ACT_UWU,
  ACT_HICCUP,
  ACT_SING,
  ACT_SNEEZE,
  ACT_PEEK
};

enum FoodType {
  FOOD_APPLE = 0,
  FOOD_RAMEN,
  FOOD_ONIGIRI,
  FOOD_COOKIE
};

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
  float energy;         // 0=exhausted  1=full stamina
  float mischief;       // 0=serious    1=cheeky/playful
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
  float look_tilt_y;
  bool  heart_pupils;
  bool  star_pupils;
  bool  sparkle_pupils;
  bool  rosy_cheeks;
  bool  dizzy_mode;
  bool  sweat_drop;
  uint8_t mouth_mode;   // 0=none, 1=uwu(ω), 2=blep(:P), 3=chew, 4=sip, 5=happy_D, 6=tiny_o, 7=sing_wave
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
  float hydration;
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
static InternalState  g_state  = {0.65f, 0.28f, 0.52f, 0.0f, 0.38f, 0.18f, 0.15f, 0.85f, 0.35f, 0,0,0,0};
static FaceParams     g_face   = {};
static MotionFeatures g_motion = {};
static NeedsState     g_needs  = {0.25f, 0.25f, 0.18f, 0.15f};

// Autonomous Action State
static AutoAction s_action       = ACT_NONE;
static uint32_t   s_action_start = 0;
static uint32_t   s_action_dur   = 0;
static uint32_t   s_next_auto_t  = 0;
static FoodType   s_food_type    = FOOD_APPLE;
static uint32_t   s_last_hiccup_pulse = 0;

// Arcs & Micro-states
static bool     s_crying       = false;
static uint32_t s_cry_start    = 0;
static float    s_cry_tear1    = 0.0f;
static float    s_cry_tear2    = 7.0f;

static bool     s_sleeping     = false;
static bool     s_trying_sleep = false;
static uint32_t s_sleep_start  = 0;
static uint32_t s_sleep_dur    = 0;
static float    s_sleep_bubble_r = 0.0f;
static float    s_nod_offset   = 0.0f;
static uint32_t s_dream_twitch_t = 0;

static float    s_yawn_open    = 0.0f;
static bool     s_wink_active  = false;
static uint32_t s_wink_end     = 0;
static float    s_flinch_timer = 0.0f;
static float    s_surprise_boost = 0.0f;

// Particles (0=bubble, 1=heart, 2=confetti, 3=crumb, 4=sparkle, 5=music_note)
#define MAX_PARTICLES 10
typedef struct {
  float x, y, vx, vy, r, phase;
  uint8_t type;
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
static void spawnParticles(uint8_t type, int count, int origin_x = -1, int origin_y = -1) {
  int spawned = 0;
  for (int i = 0; i < MAX_PARTICLES && spawned < count; i++) {
    if (!s_particles[i].alive) {
      s_particles[i].x = (origin_x >= 0) ? (origin_x + random(-6, 7)) : random(16, 112);
      if (origin_y >= 0) {
        s_particles[i].y = origin_y;
      } else if (type == 0) {
        s_particles[i].y = 58 + random(0, 6);
      } else if (type == 2) {
        s_particles[i].y = random(2, 16);
      } else {
        s_particles[i].y = EY + random(0, 12);
      }

      if (type == 2) { // confetti falls down
        s_particles[i].vx = (random(200) - 100) / 90.0f;
        s_particles[i].vy = 0.45f + random(55) / 100.0f;
      } else if (type == 3) { // food crumbs pop outward & fall
        s_particles[i].vx = (random(200) - 100) / 70.0f;
        s_particles[i].vy = -(0.5f + random(40) / 100.0f);
      } else { // bubbles, hearts, sparkles, music notes float up
        s_particles[i].vx = (random(200) - 100) / 110.0f;
        s_particles[i].vy = -(0.35f + random(45) / 100.0f);
      }

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

  float gyro_mag = sqrtf(fgx * fgx + fgy * fgy + fgz * fgz);
  g_input.tapped = (g_input.jerk > 0.28f && gyro_mag < 90.0f);

  bool was_inv = g_input.inverted;
  g_input.inverted = (fabsf(s_pf) > 135.f || fabsf(s_rf) > 135.f);
  if (g_input.inverted && !was_inv) {
    s_inverted_since = millis();
    s_action = ACT_NONE;
    spawnParticles(0, 6);
  }
  if (!g_input.inverted) s_inverted_since = 0;

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
//  AUTONOMOUS BEHAVIOUR & RANDOM QUIRKS ENGINE
// ═══════════════════════════════════════════════════════════
static void startAutoAction(AutoAction act, uint32_t dur_ms, uint32_t now) {
  s_action = act;
  s_action_start = now;
  s_action_dur = dur_ms;
  s_thought.text = nullptr;

  if (act == ACT_EATING) {
    s_food_type = (FoodType)(esp_random() % 4);
  } else if (act == ACT_CELEBRATE) {
    spawnParticles(2, 8);
    s_squish_v = 2.4f;
  } else if (act == ACT_UWU) {
    spawnParticles(1, 3);
  } else if (act == ACT_ZOOMIES) {
    s_squish_v = -2.2f;
  } else if (act == ACT_HICCUP) {
    s_last_hiccup_pulse = now;
    s_squish_v = 2.8f;
  } else if (act == ACT_SING) {
    spawnParticles(5, 2, 95, 16);
  }
}

static void updateAutonomousActions(float dt, uint32_t now) {
  if (s_action != ACT_NONE) {
    if (s_crying || g_input.inverted || g_motion.motion_energy > 0.65f) {
      s_action = ACT_NONE;
      return;
    }

    uint32_t elapsed = now - s_action_start;

    if (s_action == ACT_EATING && (elapsed % 900) < 25) {
      spawnParticles(3, 2, 64, 52);
    } else if (s_action == ACT_CELEBRATE && (elapsed % 750) < 25) {
      spawnParticles(2, 3);
      s_squish_v = 1.6f;
    } else if (s_action == ACT_HICCUP && (now - s_last_hiccup_pulse) > 1350) {
      s_last_hiccup_pulse = now;
      s_squish_v = 3.2f;
      s_surprise_boost = 0.6f;
    } else if (s_action == ACT_SING && (elapsed % 1100) < 25) {
      spawnParticles(5, 1, (esp_random() % 2 == 0) ? 20 : 108, 18);
    } else if (s_action == ACT_SNEEZE && elapsed > (s_action_dur - 500) && elapsed < (s_action_dur - 450)) {
      s_squish_v = -4.2f; // big ACHOO squash!
      spawnParticles(4, 3);
    }

    if (elapsed >= s_action_dur) {
      AutoAction finished = s_action;
      s_action = ACT_NONE;
      s_next_auto_t = now + (uint32_t)((4200 + (esp_random() % 7000)) / KIRO_CHAOS_RATE);

      if (finished == ACT_EATING) {
        g_needs.hunger = max(0.0f, g_needs.hunger - 0.65f);
        g_state.energy = min(1.0f, g_state.energy + 0.25f);
        g_state.valence = min(KIRO_MAX_HAPPY, g_state.valence + 0.14f);
        g_state.irritation = max(0.0f, g_state.irritation - 0.15f);
        spawnParticles(4, 3);
        // 30% chance to get cute post-meal hiccups!
        if ((esp_random() % 100) < 30) {
          startAutoAction(ACT_HICCUP, 4200, now);
        }
      } else if (finished == ACT_DRINKING) {
        g_needs.hydration = max(0.0f, g_needs.hydration - 0.70f);
        g_state.energy = min(1.0f, g_state.energy + 0.18f);
        g_state.valence = min(KIRO_MAX_HAPPY, g_state.valence + 0.10f);
        if ((esp_random() % 100) < 25) {
          startAutoAction(ACT_HICCUP, 3800, now);
        }
      } else if (finished == ACT_CELEBRATE || finished == ACT_ZOOMIES) {
        g_state.energy = max(0.05f, g_state.energy - 0.15f);
        g_state.attention_need = max(0.0f, g_state.attention_need - 0.30f);
      }
    }
    return;
  }

  if (s_sleeping || s_crying || g_input.inverted || now < s_next_auto_t) return;

  // Priority 1: Physiological Needs
  if (g_needs.hunger > 0.55f && (esp_random() % 100) < 48) {
    startAutoAction(ACT_EATING, 4200 + (esp_random() % 1500), now);
    return;
  }
  if (g_needs.hydration > 0.55f && (esp_random() % 100) < 48) {
    startAutoAction(ACT_DRINKING, 3600 + (esp_random() % 1200), now);
    return;
  }

  // Priority 2: Conditional Mood & Personality Quirks
  int roll = esp_random() % 100;

  // Shy Peek-a-Boo if tilted significantly to one side while curious
  if (fabsf(s_rf) > 38.0f && fabsf(s_rf) < 85.0f && g_state.curiosity > 0.45f && roll < 30) {
    startAutoAction(ACT_PEEK, 2800, now);
    return;
  }

  // Singing / Humming when happy & content
  if (g_state.valence > 0.68f && g_state.irritation < 0.20f && g_motion.stillness > 0.35f && roll < 22) {
    startAutoAction(ACT_SING, 3600 + (esp_random() % 1400), now);
    return;
  }

  // Celebration when very happy & energetic
  if (g_state.valence > 0.76f && g_state.energy > 0.45f && roll < 22) {
    startAutoAction(ACT_CELEBRATE, 3400 + (esp_random() % 1400), now);
    return;
  }

  // Zoomies when mischievous + energetic + bored
  if (g_state.mischief > 0.55f && g_state.energy > 0.60f && g_motion.stillness > 0.4f && roll < 20) {
    startAutoAction(ACT_ZOOMIES, 2800 + (esp_random() % 1200), now);
    return;
  }

  // UwU bliss when affectionate
  if (g_state.attachment > 0.68f && g_state.valence > 0.62f && roll < 26) {
    startAutoAction(ACT_UWU, 2600 + (esp_random() % 1400), now);
    return;
  }

  // Cheeky Blep (:P)
  if (g_state.mischief > 0.45f && g_state.irritation < 0.25f && roll < 24) {
    startAutoAction(ACT_BLEP, 2200 + (esp_random() % 1400), now);
    return;
  }

  // Rare cute sneeze!
  if (roll < 6) {
    startAutoAction(ACT_SNEEZE, 2100, now);
    return;
  }

  // Spontaneous snack/sip
  if (g_needs.hunger > 0.32f && roll < 14) {
    startAutoAction(ACT_EATING, 3800, now);
    return;
  }
  if (g_needs.hydration > 0.32f && roll < 14) {
    startAutoAction(ACT_DRINKING, 3200, now);
    return;
  }

  s_next_auto_t = now + 2400 + (esp_random() % 3200);
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
    g_state.dvalence   += s_mevents[i].valence_delta   * env * dt * 3.0f * KIRO_HAPPY_SPEED;
    g_state.dcuriosity += s_mevents[i].curiosity_delta * env * dt * 4.0f;
  }
}

static void applyMotionEvents(uint32_t now) {
  float me = g_motion.motion_energy;
  float rs = g_motion.rotation_speed;
  float os = g_motion.oscillation_score;
  float st = g_motion.stillness;
  bool sleeping = (g_state.sleep_pressure > 0.75f);

  // 0. Sharp Tap / Poke
  if (!sleeping && g_input.tapped && (now - s_cd_tap) > 700) {
    s_cd_tap = now;
    if (g_state.mischief > 0.72f && g_state.valence > 0.70f && (esp_random() % 100) < 40) {
      startAutoAction(ACT_BLEP, 2000, now);
    } else {
      s_flinch_timer = 0.65f;
      s_squish_v = -3.8f;
      g_state.irritation = constrain(g_state.irritation + 0.14f * KIRO_ANGER_SPEED, 0.0f, KIRO_MAX_ANGRY);
      pushMotionEvent(0.30f, -0.15f, 0.10f, 0.6f);
    }
  }

  // 1. Shake
  if (!sleeping && me > 0.48f && (now - s_cd_shake) > 1100) {
    s_cd_shake = now;
    float inten = constrain((me - 0.48f) / 0.52f, 0.0f, 1.0f);
    if (g_state.valence > 0.68f && g_state.irritation < 0.25f) {
      g_state.attachment = constrain(g_state.attachment + KIRO_PET_WARMTH * inten, 0.0f, 1.0f);
      g_state.mischief   = constrain(g_state.mischief + 0.12f * inten, 0.0f, 1.0f);
      pushMotionEvent(0.35f * inten, 0.15f * inten, 0.20f * inten, 0.7f);
      if (inten > 0.6f && s_action == ACT_NONE && (esp_random() % 100) < 35) {
        startAutoAction(ACT_CELEBRATE, 2800, now);
      }
    } else {
      g_state.irritation = constrain(g_state.irritation + 0.18f * inten * KIRO_ANGER_SPEED, 0.0f, KIRO_MAX_ANGRY);
      pushMotionEvent(0.40f * inten, -0.18f * inten, 0.05f, 0.8f);
    }
  }

  // 2. Smooth Tilt
  if (!sleeping && rs > 0.08f && me < 0.35f && (now - s_cd_rotate) > 750) {
    s_cd_rotate = now;
    float inten = constrain((rs - 0.08f) / 0.30f, 0.0f, 1.0f);
    pushMotionEvent(0.10f * inten, 0.04f * inten, 0.35f * inten, 0.7f);
  }

  // 3. Gentle Rocking / Petting
  if (!sleeping && os > 0.32f && (now - s_cd_oscillate) > 1600) {
    s_cd_oscillate = now;
    float inten = constrain((os - 0.32f) / 0.68f, 0.0f, 1.0f);
    g_state.irritation = max(0.0f, g_state.irritation - 0.16f * inten);
    g_state.attachment = constrain(g_state.attachment + 0.12f * inten, 0.0f, 1.0f);
    pushMotionEvent(0.12f * inten, 0.42f * inten, 0.15f * inten, 1.2f);
    if (g_state.valence > 0.75f && g_state.attachment > 0.72f) {
      spawnParticles(1, 2);
      if (s_action == ACT_NONE && (esp_random() % 100) < 30) {
        startAutoAction(ACT_UWU, 2600, now);
      }
    }
  }

  // 4. Sudden freeze
  float edrop = s_prev_energy - me;
  if (!sleeping && edrop > 0.28f && me < 0.12f && (now - s_cd_stop) > 1400) {
    s_cd_stop = now;
    s_blink_vel = 18.0f;
    pushMotionEvent(0.05f, 0.0f, 0.28f, 0.45f);
  }
  s_prev_energy = me;

  // 5. Prolonged stillness
  if (st > 0.55f && (now - s_cd_still) > 4500) {
    s_cd_still = now;
    g_state.attention_need = constrain(g_state.attention_need + 0.12f, KIRO_MIN_BORED, KIRO_MAX_BORED);
    g_state.mischief = constrain(g_state.mischief + 0.08f * g_state.energy, 0.0f, 1.0f);
    pushMotionEvent(-0.10f, 0.0f, 0.12f, 1.8f);
  }
}

// ═══════════════════════════════════════════════════════════
//  UNIFIED SLEEP, CRY & INTERNAL STATE ENGINE
// ═══════════════════════════════════════════════════════════
static float getZNorm() {
  return constrain(cosf(s_pf * DEG_TO_RAD) * cosf(s_rf * DEG_TO_RAD), -1.f, 1.f);
}

static void updateCryArc(float dt, uint32_t now) {
  if (!s_crying && g_state.irritation >= KIRO_CRY_THRESHOLD) {
    s_crying = true;
    s_cry_start = now;
    s_cry_tear1 = 0;
    s_cry_tear2 = 7.0f;
    g_state.valence = max(KIRO_MIN_HAPPY, g_state.valence - 0.22f);
  }

  if (s_crying) {
    uint32_t elapsed = now - s_cry_start;
    g_state.valence = max(KIRO_MIN_HAPPY, g_state.valence - 0.02f * dt);
    float soothe = (g_motion.oscillation_score > 0.3f) ? 0.12f : 0.045f;
    g_state.irritation = max(0.0f, g_state.irritation - soothe * dt);

    if (elapsed > KIRO_MAX_CRY_MS || (elapsed > KIRO_MIN_CRY_MS && g_state.irritation < 0.18f)) {
      s_crying = false;
      g_state.valence = constrain(g_state.valence + 0.18f, KIRO_MIN_HAPPY, KIRO_MAX_HAPPY);
      g_state.attachment = constrain(g_state.attachment + 0.12f, 0.0f, 1.0f);
    }
    s_breath_rate = 0.045f + sinf(now * 0.004f) * 0.02f;
  }
}

static void updateThought(uint32_t now) {
  if (s_thought.text && now > s_thought.end_t) s_thought.text = nullptr;

  if (!s_thought.text && now > s_next_thought_t && !s_sleeping && s_action == ACT_NONE &&
      g_state.attention_need > 0.32f && g_motion.stillness > 0.42f) {
    const char* thoughts[] = {"...", "??", "!", "~", "hmm", "o.O", "zz?"};
    const char* t;
    if (g_needs.hunger > 0.62f)          t = "nom?";
    else if (g_needs.hydration > 0.62f)  t = "sip?";
    else if (g_state.sleep_pressure > 0.50f) t = "yawn..";
    else if (g_state.irritation > 0.45f) t = "hmph";
    else if (g_state.valence < 0.35f)    t = "...";
    else if (g_state.attachment > 0.72f) t = "<3";
    else if (g_state.curiosity > 0.62f)  t = "??";
    else                                 t = thoughts[random(7)];

    s_thought = {t, 0.0f, now + 1400 + (uint32_t)random(700)};
    s_next_thought_t = now + 4500 + (uint32_t)random(7000);
  }
  if (s_thought.text) s_thought.y = min(7.0f, s_thought.y + 0.25f);
}

static void updateInternalState(float dt, uint32_t now) {
  TimeProfile tp = get_time_profile(s_hour);
  tickMotionEvents(dt);

  float jerk_norm = g_motion.motion_energy;

  // ── Arousal ──────────────────────────────────────────────
  float action_arousal = (s_action == ACT_ZOOMIES || s_action == ACT_CELEBRATE) ? 0.45f : 0.0f;
  float at = constrain(0.32f + tp.arousal_bias + jerk_norm * 0.55f + action_arousal, 0.0f, 1.0f);
  if (s_sleeping) at = 0.05f;
  g_state.darousal += (at - g_state.arousal) * 2.5f * dt;
  g_state.darousal *= KIRO_MOOD_INERTIA;
  g_state.arousal = constrain(g_state.arousal + g_state.darousal * dt, 0.0f, 1.0f);

  // ── Irritation ───────────────────────────────────────────
  float irr_build = (jerk_norm > 0.32f) ? (jerk_norm - 0.32f) * 0.08f * tp.irritation_scale * KIRO_ANGER_SPEED * dt : 0.0f;
  float irr_decay = (jerk_norm < 0.10f) ? 0.045f * dt : 0.012f * dt;
  if (g_needs.hunger > 0.70f)    irr_build += 0.004f * dt;
  if (g_needs.hydration > 0.70f) irr_build += 0.004f * dt;
  if (g_needs.fatigue > 0.75f)   irr_build += 0.005f * dt;

  g_state.dirrit += (irr_build - irr_decay);
  g_state.dirrit *= KIRO_MOOD_INERTIA;
  g_state.irritation = constrain(g_state.irritation + g_state.dirrit, 0.0f, KIRO_MAX_ANGRY);

  // ── Valence ──────────────────────────────────────────────
  float valence_baseline = 0.58f + tp.valence_bias;
  float valence_push = -g_state.irritation * 0.45f
                       + g_state.attachment * 0.20f
                       + (g_state.arousal - 0.4f) * 0.10f;
  if (g_needs.hunger > 0.68f)    valence_push -= 0.07f;
  if (g_needs.hydration > 0.68f) valence_push -= 0.07f;
  if (g_needs.fatigue > 0.75f)   valence_push -= 0.06f;
  if (g_needs.social < 0.25f)    valence_push += 0.06f;

  float valence_target = constrain(valence_baseline + valence_push, KIRO_MIN_HAPPY, KIRO_MAX_HAPPY);
  g_state.dvalence += (valence_target - g_state.valence) * 1.6f * KIRO_HAPPY_SPEED * dt;
  g_state.dvalence *= KIRO_MOOD_INERTIA;
  g_state.dvalence += 0.004f * sinf(now * 0.0003f + s_phase_a) * dt;
  g_state.valence = constrain(g_state.valence + g_state.dvalence * dt, KIRO_MIN_HAPPY, KIRO_MAX_HAPPY);

  // ── Curiosity & Mischief ─────────────────────────────────
  if ((now - s_curiosity_spike_t) > (uint32_t)(6500 + (int)(esp_random() % 9500))) {
    s_curiosity_spike_t = now;
    s_curiosity_spike = 0.28f + (esp_random() % ((int)(KIRO_CURIOUS_SPIKE * 100))) / 100.f;
  }
  s_curiosity_spike *= (1.0f - 0.75f * dt);
  float ct = constrain(s_curiosity_spike + g_motion.rotation_speed * 0.45f + jerk_norm * 0.15f, 0.0f, 1.0f);
  g_state.dcuriosity += (ct - g_state.curiosity) * 2.2f * dt;
  g_state.dcuriosity *= 0.78f;
  g_state.curiosity = constrain(g_state.curiosity + g_state.dcuriosity * dt, 0.0f, 1.0f);

  float mischief_target = constrain((g_state.valence - 0.4f) * 0.8f + (g_state.energy - 0.4f) * 0.5f - g_state.irritation, 0.0f, 1.0f);
  g_state.mischief += (mischief_target - g_state.mischief) * 0.15f * dt;

  // ── Unified Sleep Engine ─────────────────────────────────
  float z = getZNorm();
  bool face_down = (z > 0.45f);
  s_trying_sleep = (s_action == ACT_NONE) &&
                   ((g_motion.stillness > 0.45f && z > 0.25f && g_state.energy < 0.55f) ||
                    (g_state.sleep_pressure > 0.48f && g_motion.stillness > 0.25f));

  if (!s_sleeping) {
    if (s_trying_sleep || (face_down && g_motion.stillness > 0.5f)) {
      g_state.sleep_pressure += (0.15f + max(0.0f, tp.sleep_bias) * 0.25f) * dt;
      if (g_state.sleep_pressure > 0.78f) {
        s_sleeping = true;
        s_sleep_start = now;
        s_sleep_dur = 480000 + (esp_random() % 240000);
        s_sleep_bubble_r = 2.0f;
      }
    } else {
      float wake_drive = g_motion.motion_energy * 0.85f + g_motion.rotation_speed * 0.55f + (s_action != ACT_NONE ? 0.3f : 0.0f);
      g_state.sleep_pressure -= (0.14f + wake_drive) * dt;
    }
  } else {
    g_state.energy = min(1.0f, g_state.energy + 0.016f * dt);
    if ((now - s_sleep_start) > s_sleep_dur) {
      s_sleeping = false;
      g_state.sleep_pressure = 0.22f;
      spawnParticles(4, 4, 72, 46); // snot bubble pop sparkle!
    }
    if (g_motion.motion_energy > 0.48f || g_input.tapped) {
      s_sleeping = false;
      g_state.sleep_pressure = 0.18f;
      spawnParticles(4, 4, 72, 46); // snot bubble pop sparkle!
      s_surprise_boost = 0.75f;
      if (esp_random() % 100 < 45) g_state.irritation = constrain(g_state.irritation + 0.20f, 0.0f, KIRO_MAX_ANGRY);
    }
  }
  g_state.sleep_pressure = constrain(g_state.sleep_pressure, 0.0f, 1.0f);

  // ── Attachment & Attention ───────────────────────────────
  float att = 0.42f + g_state.valence * 0.30f - g_state.irritation * 0.38f;
  g_state.attachment = constrain(g_state.attachment + (att - g_state.attachment) * 0.22f * dt, 0.0f, 1.0f);

  float an_chg = (g_motion.stillness > 0.45f) ? 0.016f * g_motion.stillness * dt : -0.07f * dt;
  g_state.attention_need = constrain(g_state.attention_need + an_chg, KIRO_MIN_BORED, KIRO_MAX_BORED);

  s_phase_a += 0.00017f; s_phase_b += 0.00011f; s_phase_c += 0.00023f;
}

// ═══════════════════════════════════════════════════════════
//  RESOLVE BEHAVIOUR (Physics, Sleep Nodding & Saccades)
// ═══════════════════════════════════════════════════════════
static void resolveBehaviour(float dt, uint32_t now) {
  // Slower, deeper breathing when asleep
  float br = s_sleeping ? 0.0022f : (0.015f + g_state.arousal * 0.055f);
  s_breath_rate += (br - s_breath_rate) * 0.5f * dt;
  s_breath = (sinf(now * s_breath_rate) + 1.0f) * 0.5f;

  // Anime sleep bubble radius synced to breathing!
  if (s_sleeping) {
    float target_r = 2.5f + s_breath * 6.5f;
    s_sleep_bubble_r += (target_r - s_sleep_bubble_r) * 0.15f;
  } else {
    s_sleep_bubble_r = 0.0f;
  }

  // Cute "nodding off" head bob when drowsy (fighting sleep before passing out)
  if (!s_sleeping && g_state.sleep_pressure > 0.42f) {
    float drowsy_amt = (g_state.sleep_pressure - 0.42f) / 0.36f;
    // Slow droop down, sudden micro-jerk back up!
    float cycle = fmodf(now * 0.00045f, 1.0f);
    float nod = (cycle < 0.82f) ? (cycle / 0.82f) * 5.5f : (1.0f - (cycle - 0.82f) / 0.18f) * 5.5f;
    s_nod_offset = nod * constrain(drowsy_amt, 0.0f, 1.0f);
  } else if (s_sleeping) {
    s_nod_offset += (3.0f + s_breath * 1.5f - s_nod_offset) * 0.08f;
  } else {
    s_nod_offset *= 0.85f;
  }

  // Occasional cute REM dream twitch while asleep
  if (s_sleeping && (now - s_dream_twitch_t) > (uint32_t)(3500 + (esp_random() % 5000))) {
    s_dream_twitch_t = now;
    s_squish_v = 1.2f;
    if ((esp_random() % 100) < 45) spawnParticles(1, 1, 95, 22); // dreaming of love <3
  }

  float ear_boost = (s_action == ACT_CELEBRATE || s_action == ACT_ZOOMIES || s_action == ACT_SING) ? 0.22f : 0.0f;
  float ear_speed = max(0.01f, 0.02f + (g_state.valence - 0.5f) * 0.16f + g_state.arousal * 0.08f + ear_boost);
  s_ear_phase += ear_speed;

  // Squish spring
  if (g_input.jerk > 0.12f) s_squish_v = -1.8f * g_input.jerk;
  s_squish_v += (1.0f - s_squish) * 24.0f * dt;
  s_squish_v *= (1.0f - 7.5f * dt);
  s_squish = constrain(s_squish + s_squish_v * dt, 0.62f, 1.38f);

  // Macro Gaze Target
  float li = (s_action == ACT_ZOOMIES) ? 180.f : max(450.f, 3800.f - g_state.arousal * 2400.f - g_state.curiosity * 900.f);
  if ((now - s_look_t) > (uint32_t)li) {
    s_look_t = now;
    if (s_action == ACT_EATING || s_action == ACT_DRINKING) {
      s_tpx = ((float)((int)(esp_random() % 40) - 20)) / 100.f;
      s_tpy = 0.65f;
    } else if (!s_sleeping && g_state.sleep_pressure > 0.45f) {
      // Heavy-lidded upward/inward drift when fighting sleep
      s_tpx = 0.0f;
      s_tpy = -0.35f;
    } else {
      float cx = (g_state.curiosity - 0.5f) * 0.5f;
      float cy = -g_state.curiosity * 0.25f;
      float rx = ((float)((int)(esp_random() % 200) - 100)) / 100.f;
      float ry = ((float)((int)(esp_random() % 160) - 80)) / 100.f;
      s_tpx = constrain(rx * 0.78f + cx, -1.f, 1.f);
      s_tpy = constrain(ry * 0.78f + cy, -1.f, 1.f);
      if (g_state.valence < 0.35f) s_tpy += 0.35f;
      if (g_state.irritation > 0.5f) { s_tpx *= 0.18f; s_tpy *= 0.10f; }
    }
  }

  // Micro-saccades
  if (!s_sleeping && (now - s_saccade_t) > (uint32_t)(260 + (esp_random() % 600))) {
    s_saccade_t = now;
    float amp = (g_state.arousal > 0.5f || g_state.curiosity > 0.6f) ? 0.14f : 0.06f;
    s_saccade_ox = ((float)((int)(esp_random() % 200) - 100) / 100.0f) * amp;
    s_saccade_oy = ((float)((int)(esp_random() % 200) - 100) / 100.0f) * amp;
  }

  // Pupil spring
  float sp = (s_action == ACT_ZOOMIES) ? 0.32f : (0.10f + g_state.arousal * 0.20f);
  float dp = 0.66f + g_state.sleep_pressure * 0.18f;
  float rb = (s_action == ACT_EATING || s_action == ACT_DRINKING) ? 0.0f : constrain(g_motion.rotation_speed * 2.8f + fabsf(s_tilt_gaze_x) * 0.6f, 0.0f, 0.75f);
  float gtx = s_tpx * (1.0f - rb) + s_tilt_gaze_x * rb + s_saccade_ox;
  float gty = s_tpy * (1.0f - rb) + s_tilt_gaze_y * rb + s_saccade_oy;
  s_pvx = (s_pvx + (gtx - s_px) * sp) * dp;
  s_pvy = (s_pvy + (gty - s_py) * sp) * dp;
  s_px += s_pvx;
  s_py += s_pvy;

  s_dizzy_angle += 0.11f + g_state.arousal * 0.05f;

  // Happy bounce & Sad droop
  float hbi = max(0.0f, g_state.valence - 0.65f);
  if (s_action == ACT_CELEBRATE || s_action == ACT_ZOOMIES) {
    s_happy_bounce = fabsf(sinf(now * 0.016f)) * 4.5f;
  } else if (s_action == ACT_SING) {
    s_happy_bounce = fabsf(sinf(now * 0.008f)) * 3.0f;
  } else if (hbi > 0 && !s_sleeping && g_state.sleep_pressure < 0.45f) {
    s_happy_bounce = sinf(now * 0.011f) * hbi * 3.5f;
  } else {
    s_happy_bounce *= 0.86f;
  }

  float sadness = max(0.0f, 0.50f - g_state.valence);
  s_sad_droop += (sadness * 4.2f - s_sad_droop) * 0.06f;

  // Wink when playful
  if (!s_wink_active && !s_sleeping && s_action == ACT_NONE && g_state.valence > 0.76f && g_state.arousal > 0.45f &&
      (now - s_wink_end) > 7000 && (esp_random() % 1400) < 3) {
    s_wink_active = true;
    s_wink_end = now + 380 + (uint32_t)random(180);
  }
  if (s_wink_active && now > s_wink_end) s_wink_active = false;

  // Cute Yawn when drowsy
  if (s_trying_sleep && g_state.sleep_pressure > 0.44f && !s_sleeping && !s_crying && s_action == ACT_NONE) {
    s_yawn_open = min(11.0f, s_yawn_open + 0.45f * dt * 60.0f);
  } else {
    s_yawn_open = max(0.0f, s_yawn_open - 0.55f * dt * 60.0f);
  }

  s_surprise_boost *= 0.86f;
  if (s_flinch_timer > 0.0f) s_flinch_timer = max(0.0f, s_flinch_timer - dt);

  // Particle physics
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!s_particles[i].alive) continue;
    s_particles[i].x += s_particles[i].vx + sinf(now * 0.004f + s_particles[i].phase) * 0.18f + g_input.tilt_x * 0.35f;
    s_particles[i].y += s_particles[i].vy;
    if (s_particles[i].type == 3) s_particles[i].vy += 0.08f;
    if (s_particles[i].type == 0 && !g_input.inverted) s_particles[i].vy += 0.045f;
    if (s_particles[i].y < -8 || s_particles[i].y > 72 || (now - s_particles[i].born) > 3500) {
      s_particles[i].alive = false;
    }
  }

  // Needs progression (1Hz)
  static uint32_t needs_t = 0;
  if (now - needs_t > 1000) {
    needs_t = now;
    if (s_action != ACT_EATING)   g_needs.hunger    = min(1.0f, g_needs.hunger + 0.008f);
    if (s_action != ACT_DRINKING) g_needs.hydration = min(1.0f, g_needs.hydration + 0.009f);

    if (!s_sleeping) g_needs.fatigue = min(1.0f, g_needs.fatigue + 0.009f);
    else             g_needs.fatigue = max(0.0f, g_needs.fatigue - 0.024f);

    if (g_motion.stillness > 0.5f) g_needs.social = min(1.0f, g_needs.social + 0.014f);
    else                           g_needs.social = max(0.0f, g_needs.social - 0.028f);
  }
}

// ═══════════════════════════════════════════════════════════
//  MAP STATE → ORGANIC FACE & MOUTH PARAMS
// ═══════════════════════════════════════════════════════════
static void mapStateToFace(uint32_t now) {
  float v  = g_state.valence;
  float a  = g_state.arousal;
  float ir = g_state.irritation;
  float sp = g_state.sleep_pressure;
  float cu = g_state.curiosity;
  float at = g_state.attachment;

  float target_open = constrain(1.0f - sp * 1.15f + a * 0.25f + s_surprise_boost * 0.45f, 0.0f, 1.0f);
  if (s_sleeping) target_open = 0.0f;
  if (s_crying) target_open *= 0.52f;
  if (s_flinch_timer > 0.0f) target_open = 0.12f;
  if (s_action == ACT_SNEEZE && (now - s_action_start) < (s_action_dur - 500)) target_open = 0.10f; // pre-sneeze squint
  if (s_action == ACT_ZOOMIES || s_action == ACT_CELEBRATE) target_open = 1.0f;

  // Asymmetry & Peek-a-boo
  float asym = 0.06f * sinf(now * 0.00008f + s_phase_c) + cu * 0.12f;
  float open_l = constrain(target_open + asym, 0.0f, 1.0f);
  float open_r = constrain(target_open - asym * 0.6f, 0.0f, 1.0f);

  if (s_wink_active || s_action == ACT_BLEP) open_r = 0.0f;
  if (s_action == ACT_PEEK) {
    if (s_rf > 0) { open_l = 0.0f; open_r = 1.0f; }
    else          { open_l = 1.0f; open_r = 0.0f; }
  }

  s_sopen_l += (open_l - s_sopen_l) * 0.28f;
  s_sopen_r += (open_r - s_sopen_r) * 0.28f;
  g_face.eye_open_l = s_sopen_l;
  g_face.eye_open_r = s_sopen_r;

  // Lower-lid cheek push (smiling crescent eyes)
  float target_smile = 0.0f;
  if (s_action == ACT_EATING) {
    target_smile = 0.45f + 0.35f * fabsf(sinf(now * 0.012f));
  } else if (s_action == ACT_DRINKING || s_action == ACT_UWU || s_action == ACT_CELEBRATE || s_action == ACT_SING) {
    target_smile = 0.72f;
  } else if (v > 0.65f && ir < 0.25f && !s_crying && sp < 0.55f) {
    target_smile = constrain((v - 0.65f) * 2.2f + g_motion.oscillation_score * 0.35f, 0.0f, 0.85f);
  }
  s_ssmile += (target_smile - s_ssmile) * 0.16f;
  g_face.smile_curve = s_ssmile;

  // Brow angle
  float target_brow = 0.0f;
  if (ir > 0.20f) target_brow = constrain((ir - 0.20f) * 1.6f, 0.0f, 1.0f);
  else if (v < 0.42f || s_crying || (sp > 0.45f && !s_sleeping)) {
    // Soft puppy-dog sleepy/sad brow slant
    float sleepy_brow = (!s_sleeping && sp > 0.45f) ? (sp - 0.45f) * 1.4f : 0.0f;
    target_brow = -constrain((0.42f - v) * 2.4f + (s_crying ? 0.4f : 0.0f) + sleepy_brow, 0.0f, 0.85f);
  }
  s_sbrow += (target_brow - s_sbrow) * 0.15f;
  g_face.brow_angle = s_sbrow;

  // Head tilt offset
  g_face.look_tilt_y = (cu > 0.55f || s_action == ACT_BLEP || s_action == ACT_PEEK) ? sinf(now * 0.0025f) * 3.0f : 0.0f;

  // Pupil size
  float ps = 0.92f + a * 0.50f + at * 0.28f - ir * 0.28f - sp * 0.45f + s_surprise_boost * 0.40f;
  if (s_crying) ps -= 0.22f;
  if (s_action == ACT_ZOOMIES || s_action == ACT_PEEK) ps = 1.45f;
  ps = constrain(ps, 0.38f, 1.75f);
  s_spsize += (ps - s_spsize) * 0.12f;
  g_face.pupil_size = s_spsize;

  g_face.pupil_x = constrain(s_px, -1.0f, 1.0f);
  g_face.pupil_y = constrain(s_py + (s_crying ? 0.32f : 0.0f), -1.0f, 1.0f);

  float jt = ir * 0.55f + a * 0.12f + (s_crying ? 0.18f : 0.0f) + (s_action == ACT_ZOOMIES ? 0.25f : 0.0f);
  s_sjitter += (jt - s_sjitter) * 0.12f;
  g_face.jitter = s_sjitter;

  g_face.squish_x = s_squish;
  g_face.squish_y = 1.0f / max(0.65f, s_squish);

  // Special pupils & effects
  g_face.heart_pupils   = (at > 0.80f && v > 0.78f && !s_crying && !s_sleeping && s_action == ACT_NONE);
  g_face.star_pupils    = (s_action == ACT_CELEBRATE) || (cu > 0.84f && a > 0.65f && !s_crying && !s_sleeping);
  g_face.sparkle_pupils = (s_action == ACT_ZOOMIES) || (s_surprise_boost > 0.45f);
  g_face.rosy_cheeks    = (at > 0.56f && v > 0.56f) || s_sleeping || s_action == ACT_UWU || s_action == ACT_EATING || (v > 0.72f && g_motion.oscillation_score > 0.28f);
  g_face.dizzy_mode     = (g_input.inverted && (millis() - s_inverted_since) > 1800);
  g_face.sweat_drop     = (ir > 0.45f && ir < KIRO_CRY_THRESHOLD && !s_crying);

  // Mouth Mode Resolution
  g_face.mouth_mode = 0;
  if (!s_crying && !g_face.dizzy_mode) {
    if (s_sleeping)                    g_face.mouth_mode = 6; // tiny peaceful 'o' mouth under sleep bubble
    else if (s_action == ACT_EATING)   g_face.mouth_mode = 3;
    else if (s_action == ACT_DRINKING) g_face.mouth_mode = 4;
    else if (s_action == ACT_BLEP)     g_face.mouth_mode = 2;
    else if (s_action == ACT_HICCUP)   g_face.mouth_mode = 6; // surprised 'o'
    else if (s_action == ACT_SING)     g_face.mouth_mode = 7; // humming wavy mouth
    else if (s_action == ACT_UWU || (at > 0.76f && v > 0.72f)) g_face.mouth_mode = 1;
    else if (s_action == ACT_CELEBRATE || (v > 0.82f && a > 0.55f)) g_face.mouth_mode = 5;
  }

  // Plain-English Emotion Label
  if (s_action == ACT_EATING)            s_emotion_label = "eating_snack";
  else if (s_action == ACT_DRINKING)     s_emotion_label = "sipping_drink";
  else if (s_action == ACT_CELEBRATE)    s_emotion_label = "celebrating";
  else if (s_action == ACT_ZOOMIES)      s_emotion_label = "zoomies";
  else if (s_action == ACT_BLEP)         s_emotion_label = "cheeky_blep";
  else if (s_action == ACT_UWU)          s_emotion_label = "uwu_bliss";
  else if (s_action == ACT_HICCUP)       s_emotion_label = "hiccups";
  else if (s_action == ACT_SING)         s_emotion_label = "humming";
  else if (s_action == ACT_SNEEZE)       s_emotion_label = "sneezing";
  else if (s_action == ACT_PEEK)         s_emotion_label = "peeking";
  else if (s_crying)                     s_emotion_label = "crying";
  else if (s_flinch_timer > 0.0f)        s_emotion_label = "flinching";
  else if (g_face.dizzy_mode)            s_emotion_label = "dizzy";
  else if (s_sleeping)                   s_emotion_label = "sleeping_bubble";
  else if (sp > 0.48f)                   s_emotion_label = "nodding_off";
  else if (g_face.heart_pupils)          s_emotion_label = "in_love";
  else if (g_face.star_pupils)           s_emotion_label = "starstruck";
  else if (ir > 0.55f)                   s_emotion_label = "furious";
  else if (ir > 0.32f)                   s_emotion_label = "grumpy";
  else if (v > 0.80f && a > 0.55f)       s_emotion_label = "ecstatic";
  else if (v > 0.70f)                    s_emotion_label = "happy";
  else if (v < 0.30f)                    s_emotion_label = "sad";
  else if (cu > 0.65f)                   s_emotion_label = "curious";
  else if (g_state.attention_need > 0.65f) s_emotion_label = "bored";
  else                                   s_emotion_label = "content";
}

// ═══════════════════════════════════════════════════════════
//  BLINK
// ═══════════════════════════════════════════════════════════
static void updateBlink(uint32_t now) {
  if (s_blink_lid < 0.05f && now > s_next_blink && !s_sleeping) {
    float bms = 4200.f - g_state.arousal * 2200.f + g_state.sleep_pressure * 1200.f;
    float nms = (float)((int)(esp_random() % 1800) - 900);
    uint32_t iv = (uint32_t)max(650.f, bms + nms);
    s_blink_seq = (esp_random() % 100 < 18) ? 2 : 1;
    s_next_blink = now + iv;
    // Slower, heavier blink when drowsy!
    s_blink_vel = (g_state.sleep_pressure > 0.45f) ? 10.0f : 19.0f;
  }
  if (s_blink_vel > 0.0f) {
    s_blink_lid += s_blink_vel * 0.016f;
    if (s_blink_lid >= 1.0f) {
      s_blink_lid = 1.0f;
      s_blink_vel = (g_state.sleep_pressure > 0.45f) ? -8.0f : -16.0f;
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
//  PROCEDURAL PROPS, MOUTHS & DRAWING HELPERS
// ═══════════════════════════════════════════════════════════
static void draw_star_pupil(int cx, int cy, int r) {
  oled->fillCircle(cx, cy, max(2, r - 2), SSD1306_BLACK);
  for (int d = 0; d < 4; d++) {
    float ang = d * 1.5708f + millis() * 0.005f;
    oled->drawLine(cx, cy, cx + (int)((r + 2) * cosf(ang)), cy + (int)((r + 2) * sinf(ang)), SSD1306_BLACK);
  }
}

static void draw_sparkle_pupil(int cx, int cy, int r) {
  oled->fillTriangle(cx, cy - r, cx - r / 3, cy, cx + r / 3, cy, SSD1306_BLACK);
  oled->fillTriangle(cx, cy + r, cx - r / 3, cy, cx + r / 3, cy, SSD1306_BLACK);
  oled->fillTriangle(cx - r, cy, cx, cy - r / 3, cx, cy + r / 3, SSD1306_BLACK);
  oled->fillTriangle(cx + r, cy, cx, cy - r / 3, cx, cy + r / 3, SSD1306_BLACK);
}

static void draw_heart_shape(int cx, int cy, int size, uint16_t color) {
  oled->fillCircle(cx - size / 2, cy - size / 4, size / 2, color);
  oled->fillCircle(cx + size / 2, cy - size / 4, size / 2, color);
  oled->fillTriangle(cx - size, cy, cx + size, cy, cx, cy + size, color);
}

static void draw_music_note(int cx, int cy) {
  oled->fillCircle(cx, cy, 2, SSD1306_WHITE);
  oled->drawFastVLine(cx + 2, cy - 6, 6, SSD1306_WHITE);
  oled->drawLine(cx + 2, cy - 6, cx + 5, cy - 4, SSD1306_WHITE);
}

static void draw_rosy(int ex, int ey, bool right_eye) {
  int cx = right_eye ? ex + EW - 4 : ex - 3;
  int cy = ey + EH + 1;
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

// Cute procedural mouth expressions centered at (64, 51)
static void draw_cute_mouth(uint32_t now, int emo_y) {
  int mx = 64;
  int my = 51 + emo_y / 2;

  if (g_face.mouth_mode == 1) {
    // ω UwU Cat-Mouth
    oled->drawCircle(mx - 3, my, 3, SSD1306_WHITE);
    oled->drawCircle(mx + 3, my, 3, SSD1306_WHITE);
    oled->fillRect(mx - 7, my - 4, 15, 4, SSD1306_BLACK);
  } else if (g_face.mouth_mode == 2) {
    // :P Cheeky Blep Tongue
    oled->drawFastHLine(mx - 5, my - 1, 11, SSD1306_WHITE);
    oled->fillRoundRect(mx - 3, my, 6, 7, 2, SSD1306_WHITE);
    oled->drawFastVLine(mx, my + 1, 4, SSD1306_BLACK);
  } else if (g_face.mouth_mode == 3) {
    // Chewing mouth
    int chew = (int)(3.0f + 3.0f * sinf(now * 0.018f));
    oled->fillRoundRect(mx - 4, my - 1, 8, chew, 2, SSD1306_WHITE);
  } else if (g_face.mouth_mode == 5) {
    // Happy open 'D' grin
    oled->fillCircle(mx, my, 5, SSD1306_WHITE);
    oled->fillRect(mx - 6, my - 6, 13, 6, SSD1306_BLACK);
    oled->drawFastHLine(mx - 5, my, 11, SSD1306_WHITE);
  } else if (g_face.mouth_mode == 6) {
    // Tiny cute 'o' mouth (sleepy or hiccup)
    oled->drawCircle(mx, my, 2, SSD1306_WHITE);
  } else if (g_face.mouth_mode == 7) {
    // Singing / Humming 'w' wave mouth
    int wave = (int)(sinf(now * 0.015f) * 2.0f);
    oled->fillCircle(mx, my, 3 + abs(wave), SSD1306_WHITE);
  }
}

// Procedural Food & Drink Props (bottom center)
static void draw_food_or_drink(uint32_t now) {
  int bob = (int)(sinf(now * 0.012f) * 2.0f);
  int cx = 64;
  int cy = 57 + bob;

  if (s_action == ACT_EATING) {
    uint32_t elapsed = now - s_action_start;
    float progress = constrain((float)elapsed / (float)s_action_dur, 0.0f, 1.0f);

    if (s_food_type == FOOD_APPLE) {
      oled->fillCircle(cx, cy, 5, SSD1306_WHITE);
      oled->drawLine(cx, cy - 5, cx + 2, cy - 8, SSD1306_WHITE);
      if (progress > 0.35f) oled->fillCircle(cx + 4, cy - 1, 3, SSD1306_BLACK);
      if (progress > 0.70f) oled->fillCircle(cx - 4, cy + 1, 3, SSD1306_BLACK);
    } else if (s_food_type == FOOD_RAMEN) {
      oled->fillCircle(cx, cy - 1, 6, SSD1306_WHITE);
      oled->fillRect(cx - 7, cy - 8, 15, 7, SSD1306_BLACK);
      oled->drawFastHLine(cx - 6, cy - 1, 13, SSD1306_WHITE);
      int st = (now / 200) % 4;
      oled->drawPixel(cx - 2, cy - 4 - st, SSD1306_WHITE);
      oled->drawPixel(cx + 2, cy - 6 - st, SSD1306_WHITE);
    } else if (s_food_type == FOOD_ONIGIRI) {
      oled->fillTriangle(cx, cy - 6, cx - 6, cy + 4, cx + 6, cy + 4, SSD1306_WHITE);
      oled->fillRect(cx - 2, cy + 1, 5, 4, SSD1306_BLACK);
      if (progress > 0.5f) oled->fillCircle(cx, cy - 5, 3, SSD1306_BLACK);
    } else {
      oled->fillCircle(cx, cy, 5, SSD1306_WHITE);
      oled->drawPixel(cx - 2, cy - 1, SSD1306_BLACK);
      oled->drawPixel(cx + 2, cy + 1, SSD1306_BLACK);
      oled->drawPixel(cx, cy + 2, SSD1306_BLACK);
      if (progress > 0.45f) oled->fillCircle(cx + 3, cy - 2, 3, SSD1306_BLACK);
    }
  } else if (s_action == ACT_DRINKING) {
    oled->fillRect(cx - 4, cy - 4, 9, 9, SSD1306_WHITE);
    oled->drawFastHLine(cx - 5, cy - 4, 11, SSD1306_WHITE);
    oled->drawLine(cx + 1, cy - 4, cx + 4, cy - 9, SSD1306_WHITE);
    oled->drawPixel(cx - 2, cy + 3, SSD1306_BLACK);
    oled->drawPixel(cx + 1, cy + 3, SSD1306_BLACK);
  }
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

  // 2. Closed Eye States
  float eo = right ? g_face.eye_open_r : g_face.eye_open_l;
  float lid_total = constrain(max(1.0f - eo, s_blink_lid), 0.0f, 1.0f);

  if (lid_total >= 0.92f) {
    int ly = ecy + 3;
    if (s_sleeping || g_state.sleep_pressure > KIRO_SLEEP_THRESH) {
      // Cute Soft U-Curve with tiny outer eyelashes!
      for (int i = 3; i < ew2 - 3; i++) {
        float t = (float)(i - ew2 / 2) / (ew2 / 2);
        int dy2 = (int)(4.5f * (1.0f - t * t));
        oled->drawPixel(rx + i, ly + dy2, SSD1306_WHITE);
        oled->drawPixel(rx + i, ly + dy2 + 1, SSD1306_WHITE);
      }
      // Cute outer eyelashes
      if (right) {
        oled->drawLine(rx + ew2 - 4, ly + 1, rx + ew2 - 1, ly + 4, SSD1306_WHITE);
        oled->drawLine(rx + ew2 - 8, ly + 3, rx + ew2 - 6, ly + 6, SSD1306_WHITE);
      } else {
        oled->drawLine(rx + 4, ly + 1, rx + 1, ly + 4, SSD1306_WHITE);
        oled->drawLine(rx + 8, ly + 3, rx + 6, ly + 6, SSD1306_WHITE);
      }
    } else if (s_flinch_timer > 0.0f || s_action == ACT_SNEEZE) {
      // >< Tight Flinch / Sneeze Chevron
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
      oled->drawFastHLine(rx + 3, ly, ew2 - 6, SSD1306_WHITE);
      oled->drawFastHLine(rx + 3, ly + 1, ew2 - 6, SSD1306_WHITE);
    }
    if (g_face.rosy_cheeks) draw_rosy(rx, ry, right);
    return;
  }

  // 3. Base White Sclera
  oled->fillRoundRect(rx, ry, ew2, eh2, ER, SSD1306_WHITE);

  // 4. Pupil
  int max_ox = (ew2 - PW) / 2 - 2;
  int max_oy = (eh2 - PH) / 2 - 2;
  float eye_convergence = right ? -0.08f : 0.08f;
  float gaze_x = constrain(g_face.pupil_x + eye_convergence, -1.0f, 1.0f);
  int pcx = ecx + (int)(gaze_x * max_ox);
  int pcy = ecy + (int)(g_face.pupil_y * max_oy);

  int pw2 = constrain((int)(PW * g_face.pupil_size), 5, ew2 - 6);
  int ph2 = constrain((int)(PH * g_face.pupil_size), 5, eh2 - 6);

  if (g_face.heart_pupils) {
    draw_heart_shape(pcx, pcy, 6, SSD1306_BLACK);
  } else if (g_face.star_pupils) {
    draw_star_pupil(pcx, pcy, pw2 / 2);
  } else if (g_face.sparkle_pupils) {
    draw_sparkle_pupil(pcx, pcy, pw2 / 2 + 1);
  } else {
    oled->fillRoundRect(pcx - pw2 / 2, pcy - ph2 / 2, pw2, ph2, PR, SSD1306_BLACK);
    oled->fillCircle(pcx - pw2 / 4, pcy - ph2 / 4, 2, SSD1306_WHITE);
    oled->drawPixel(pcx + pw2 / 5, pcy + ph2 / 5, SSD1306_WHITE);
  }

  // 5. Upper Eyelid & Brow Cutout
  int lid_px = (int)(lid_total * eh2);
  if (lid_px > 0) {
    oled->fillRect(rx, ry, ew2, lid_px, SSD1306_BLACK);
  }

  if (fabsf(g_face.brow_angle) > 0.08f) {
    int slant = (int)(g_face.brow_angle * 11.0f);
    int top_y = ry + lid_px;
    if (slant > 0) {
      if (right) oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx, top_y + slant, SSD1306_BLACK);
      else       oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx + ew2, top_y + slant, SSD1306_BLACK);
    } else {
      int s = -slant;
      if (right) oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx + ew2, top_y + s, SSD1306_BLACK);
      else       oled->fillTriangle(rx, top_y, rx + ew2, top_y, rx, top_y + s, SSD1306_BLACK);
    }
  }

  // 6. Lower Eyelid Cheek Push (smiling crescent)
  if (g_face.smile_curve > 0.05f) {
    int push = (int)(g_face.smile_curve * 11.5f);
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
  int emo_y  = (int)(-s_happy_bounce + s_sad_droop + s_nod_offset);
  int tilt_l = (int)(-g_face.look_tilt_y);
  int tilt_r = (int)(g_face.look_tilt_y);

  draw_one_eye(ELX + fury_x, EY + emo_y + tilt_l, false);
  draw_one_eye(ERX + fury_x, EY + emo_y + tilt_r, true);

  draw_ears(emo_y);

  // Cute Mouths
  if (s_action != ACT_EATING && s_action != ACT_DRINKING && s_yawn_open <= 1.2f) {
    draw_cute_mouth(now, emo_y);
  }

  // Anime Sleep Bubble (inflates & deflates with breath!)
  if (s_sleeping && s_sleep_bubble_r > 1.5f) {
    int r = (int)s_sleep_bubble_r;
    int bx = 66 + r / 2;
    int by = 51 + emo_y / 2 - r / 3;
    oled->drawCircle(bx, by, r, SSD1306_WHITE);
    // Cute specular shine inside the sleep bubble
    if (r >= 4) {
      oled->drawPixel(bx - r / 2, by - r / 2, SSD1306_WHITE);
      oled->drawPixel(bx - r / 2 + 1, by - r / 2, SSD1306_WHITE);
    }
  }

  // Food & Drink Props
  if (s_action == ACT_EATING || s_action == ACT_DRINKING) {
    draw_food_or_drink(now);
  }

  // Crying Tears
  if (s_crying) {
    float spd = 0.28f + g_state.irritation * 0.75f;
    draw_teardrop(ELX + EW / 2, EY + emo_y + EH - 2, s_cry_tear1, spd);
    draw_teardrop(ERX + EW / 2, EY + emo_y + EH - 2, s_cry_tear2, spd * 1.08f);
  }

  // Stress Sweat Drop
  if (g_face.sweat_drop) {
    int sy = EY + emo_y + 2 + ((now / 120) % 6);
    int sx = ERX + EW + 5;
    oled->fillCircle(sx, sy + 3, 2, SSD1306_WHITE);
    oled->fillTriangle(sx - 2, sy + 3, sx + 2, sy + 3, sx, sy - 2, SSD1306_WHITE);
  }

  // Yawn Oval
  if (s_yawn_open > 1.2f) {
    int yw = (int)(s_yawn_open * 1.5f);
    int yh = (int)(s_yawn_open * 0.85f);
    oled->fillRoundRect(64 - yw / 2, 48 + emo_y / 2, yw, yh, 4, SSD1306_WHITE);
  }

  // Floating Particles (0=bubble, 1=heart, 2=confetti, 3=crumb, 4=sparkle, 5=music_note)
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!s_particles[i].alive) continue;
    int px = (int)s_particles[i].x;
    int py = (int)s_particles[i].y;
    uint8_t t = s_particles[i].type;
    if (t == 0) {
      oled->drawCircle(px, py, (int)s_particles[i].r, SSD1306_WHITE);
      oled->drawPixel(px - 1, py - 1, SSD1306_WHITE);
    } else if (t == 1) {
      draw_heart_shape(px, py, 3, SSD1306_WHITE);
    } else if (t == 2) {
      if (((now / 100) + i) % 2 == 0) oled->fillRect(px, py, 3, 2, SSD1306_WHITE);
      else                            oled->drawLine(px, py, px + 2, py + 2, SSD1306_WHITE);
    } else if (t == 3) {
      oled->fillRect(px, py, 2, 2, SSD1306_WHITE);
    } else if (t == 4) {
      oled->drawFastHLine(px - 2, py, 5, SSD1306_WHITE);
      oled->drawFastVLine(px, py - 2, 5, SSD1306_WHITE);
    } else if (t == 5) {
      draw_music_note(px, py);
    }
  }

  // Floating Sine-Wave Sleep Z's (two staggered Z's drifting up)
  if (s_sleeping || (g_state.sleep_pressure > KIRO_SLEEP_THRESH && g_state.arousal < 0.22f)) {
    float zt1 = (now % 2600) / 2600.0f;
    int zx1 = ERX + EW - 6 + (int)(sinf(zt1 * 6.28f) * 3.0f);
    int zy1 = EY + 12 - (int)(zt1 * 13.0f);
    oled->setTextSize(1);
    oled->setTextColor(SSD1306_WHITE);
    oled->setCursor(zx1, zy1);
    oled->print("z");

    float zt2 = ((now + 1300) % 2600) / 2600.0f;
    int zx2 = ERX + EW - 1 + (int)(sinf(zt2 * 6.28f + 1.5f) * 3.0f);
    int zy2 = EY + 10 - (int)(zt2 * 13.0f);
    oled->setCursor(zx2, zy2);
    oled->print("Z");
  }

  // Thought Bubble
  if (s_thought.text) {
    int ty = max(1, EY - 10 - (int)s_thought.y);
    oled->setTextSize(1);
    oled->setTextColor(SSD1306_WHITE);
    oled->setCursor(52, ty);
    oled->print(s_thought.text);
  }

  // Subtle Hunger/Thirst Icons
  if (g_needs.hunger > 0.68f && !s_sleeping && s_action == ACT_NONE) {
    oled->drawFastVLine(120, 54, 9, SSD1306_WHITE);
    oled->drawFastVLine(122, 54, 5, SSD1306_WHITE);
    oled->drawFastVLine(124, 54, 9, SSD1306_WHITE);
  }
  if (g_needs.hydration > 0.68f && !s_sleeping && s_action == ACT_NONE) {
    oled->fillCircle(5, 60, 2, SSD1306_WHITE);
    oled->fillTriangle(3, 60, 7, 60, 5, 56, SSD1306_WHITE);
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
  if (KIRO_DEBUG_MS == 0 || (now - s_last_debug) < (uint32_t)KIRO_DEBUG_MS) return;
  s_last_debug = now;

  Serial.println(F("╔══════════ KIRO v15 ══════════╗"));
  Serial.printf("  EMOTION : %s\n", s_emotion_label);
  printBar(g_state.valence,        "  valence ");
  printBar(g_state.arousal,        "  arousal ");
  printBar(g_state.irritation,     "  irritate");
  printBar(g_state.sleep_pressure, "  sleep   ");
  printBar(g_state.energy,         "  energy  ");
  printBar(g_state.mischief,       "  mischief");
  Serial.println(F("╚══════════════════════════════╝\n"));
}

// ═══════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\nKIRO — kirov15 Cute Sleep & Living Quirks Build"));

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
  g_state.valence        = constrain(0.62f + tp.valence_bias, KIRO_MIN_HAPPY, KIRO_MAX_HAPPY);

  s_last_mpu = millis();
  s_curiosity_spike_t = millis();
  s_next_blink = millis() + 1800;
  s_osc_sample_t = millis();
  s_next_thought_t = millis() + 4000;
  s_next_auto_t = millis() + 4500;
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
  updateAutonomousActions(dt, now);
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
