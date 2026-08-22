/* Tape Echo 2 for Ableton Move (Schwung audio_fx module).
 *
 * The Move-facing parameter surface. Single source of truth: the DSP shell,
 * the chain_params JSON served via get_param, the ui_hierarchy, and the state
 * blob all iterate this table. movy_config.json (the curated knob layout)
 * mirrors it by hand — tools/check_config.py fails the build if they drift.
 *
 * Order matters: the first TE2_VISIBLE_PARAM_COUNT entries are the published
 * surface. Everything after that is still settable via set_param and still
 * round-trips in state, but is deliberately NOT offered as a knob — either it
 * is a compatibility control, or it duplicates something the Move already does
 * at the mixer/chain level (output trim, panning, slot bypass).
 *
 * The DSP core and its parameter semantics are Tape Echo 2 by Dusk Audio
 * (GPL-3.0, github.com/dusk-audio/dusk-audio-plugins); see src/ported/.
 */

#ifndef TE2_PARAMS_H
#define TE2_PARAMS_H

enum Te2ParamIndex {
    /* ---- published: page 1, the echo itself ---- */
    TE2_P_MODE = 0,     /* enum 0..11 -> DSP mode 1..12 */
    TE2_P_RATE,         /* repeat rate 0..1 (0 = slow/177ms, 1 = fast/69ms) */
    TE2_P_INTENSITY,    /* feedback; self-oscillates above ~0.75 */
    TE2_P_ECHO_VOL,
    TE2_P_REVERB_VOL,   /* audible in modes 5-12 only */
    TE2_P_MIX,          /* dry/wet crossfade, 0.5 = both at unity */
    TE2_P_SYNC,         /* enum Off/On: leading head locks to host tempo */
    TE2_P_NOTE,         /* physical 1..11 Echo Rate detent (Galaxy-style) */
    /* ---- published: page 2, tape character ---- */
    TE2_P_INPUT,        /* preamp drive / saturation amount */
    TE2_P_BASS,         /* -1..1, echo path only */
    TE2_P_TREBLE,       /* -1..1, echo path only */
    TE2_P_WOW,          /* wow & flutter amount */
    TE2_P_AGE,          /* enum New/Used/Old -> 0.0/0.5/1.0 */
    TE2_P_SEND,         /* enum Off/On: program feed to tape ("dub" switch) */
    TE2_P_PINGPONG,     /* enum Off/On: alternate successive repeats L/R */
    TE2_P_WIDTH,        /* 0..100: how far the ping-pong swings (0 = centred) */
    /* ---- not published (see header comment) ---- */
    TE2_P_OUTPUT_VOL,   /* -20..+20 dB; left at 0.5 = unity, use the mixer */
    TE2_P_ECHO_PAN,     /* left at 0.5 = center */
    TE2_P_REVERB_PAN,   /* left at 0.5 = center */
    TE2_P_POWER,        /* left On; the chain slot does bypass */
    TE2_P_PRESET,       /* factory program launcher, still usable via set_param */
    TE2_P_DRY,          /* legacy dry level, default 1.0 */
    TE2_PARAM_COUNT,
    TE2_VISIBLE_PARAM_COUNT = TE2_P_OUTPUT_VOL
};

typedef enum { TE2_FLOAT, TE2_INT, TE2_ENUM } te2_type_t;

typedef struct {
    const char  *key;
    const char  *name;
    te2_type_t   type;
    float        min, max, def;
    const char *const *options;  /* TE2_ENUM only */
    int          n_options;
} te2_param_t;

/* Movy truncates an enum option to 5 chars on the knob readout
 * (store.ts formatValue), so every name must stay DISTINCT within its first
 * five characters — "Head 1"/"Head 2"/"Head 3" all rendered as "Head ".
 * R = spring reverb; digits are the active playback heads. */
static const char *const te2_opts_mode[12] = {
    "H1", "H2", "H3", "H2+3",
    "H1+R", "H2+R", "H3+R", "H12+R",
    "H23+R", "H13+R", "H123R", "Rev",
};
static const char *const te2_opts_offon[2] = { "Off", "On" };
static const char *const te2_opts_age[3]   = { "New", "Used", "Old" };
/* Order matches kFactoryPresets in the vendored TapeEchoParams.hpp. */
static const char *const te2_opts_preset[13] = {
    "Default", "Slapback Vocal", "Rockabilly Gtr", "Classic Tape",
    "Dub Throw", "Synced 1/8 Dub", "Multi-Head", "Orbital Echo",
    "Full Wash", "Ambient Trails", "Worn Tape", "Runaway Drone",
    "Spring Only",
};

/* Defaults for mode / repeat_rate / intensity / echo_volume are NOT this
 * engine's own — they are what an untouched TapeDelay v0.4.3 becomes when its
 * state is run through the legacy import in tape_echo_plugin.cpp. This module
 * inherited that module's id, so a fresh insert has to land where the thing it
 * replaced landed: a 400 ms echo with a few repeats, not a 177 ms slapback
 * with none.
 *
 * TapeDelay's own create_instance came up at time=400, feedback=0.4, mix=0.5,
 * tone=0.5, stereo_width=0, division=free. Note those are the values in its C,
 * which is what you actually heard — its chain_params declared no defaults at
 * all for the floats, and the "default" fields in its ui_hierarchy (mix 0.35,
 * tone 0.6) were never applied to anything.
 *
 * 400 ms is out of head 1's reach (69.33..177.354 ms) and head 2's, so it
 * lands on head 3 at 145.641 ms x 2.76118 = 400.000 ms.
 *
 * The two that are not obvious were measured against the old engine rather
 * than reasoned about, by rendering an impulse through both and comparing the
 * repeat trains: intensity 0.44 gives the same -12.1 dB per repeat as
 * feedback 0.4 did, and echo_volume 1.0 restores the wet-to-dry ratio that the
 * old crossfade Mix produced at 0.5. Both derivations live with the import
 * functions. Together they are the difference between one repeat 22 dB down
 * and the usable three-repeat echo TapeDelay actually had.
 *
 * tools/loadtest.cpp pins all four against the import path itself, so these
 * cannot drift from the mapping they were derived from. */
static const te2_param_t te2_params[TE2_PARAM_COUNT] = {
    { "mode",           "Mode",          TE2_ENUM,  0,  11, 2,    te2_opts_mode,   12 },
    { "repeat_rate",    "Repeat Rate",   TE2_FLOAT, 0,   1, 0.414888f, 0, 0 },
    { "intensity",      "Intensity",     TE2_FLOAT, 0,   1, 0.44f, 0, 0 },
    { "echo_volume",    "Echo Volume",   TE2_FLOAT, 0,   1, 1.0f, 0, 0 },
    { "reverb_volume",  "Reverb Volume", TE2_FLOAT, 0,   1, 0.0f, 0, 0 },
    { "mix",            "Mix",           TE2_FLOAT, 0,   1, 0.5f, 0, 0 },
    { "tempo_sync",     "Tempo Sync",    TE2_ENUM,  0,   1, 0,    te2_opts_offon,  2 },
    { "echo_rate_note", "Echo Rate Note",TE2_INT,   1,  11, 5,    0, 0 },
    { "input_volume",   "Input Drive",   TE2_FLOAT, 0,   1, 0.5f, 0, 0 },
    { "bass",           "Bass",          TE2_FLOAT, -1,  1, 0.0f, 0, 0 },
    { "treble",         "Treble",        TE2_FLOAT, -1,  1, 0.0f, 0, 0 },
    { "wow_flutter",    "Wow & Flutter", TE2_FLOAT, 0,   1, 0.0f, 0, 0 },
    { "tape_age",       "Tape Age",      TE2_ENUM,  0,   2, 1,    te2_opts_age,    3 },
    { "input_send",     "Input Send",    TE2_ENUM,  0,   1, 1,    te2_opts_offon,  2 },
    { "ping_pong",      "Ping Pong",     TE2_ENUM,  0,   1, 0,    te2_opts_offon,  2 },
    { "stereo_width",   "Stereo Width",  TE2_INT,   0, 100, 0,    0, 0 },
    /* unpublished */
    { "output_volume",  "Output Volume", TE2_FLOAT, 0,   1, 0.5f, 0, 0 },
    { "echo_pan",       "Echo Pan",      TE2_FLOAT, 0,   1, 0.5f, 0, 0 },
    { "reverb_pan",     "Reverb Pan",    TE2_FLOAT, 0,   1, 0.5f, 0, 0 },
    { "power",          "Power",         TE2_ENUM,  0,   1, 1,    te2_opts_offon,  2 },
    { "preset",         "Preset",        TE2_ENUM,  0,  12, 0,    te2_opts_preset, 13 },
    { "dry_level",      "Dry Level",     TE2_FLOAT, 0,   1, 1.0f, 0, 0 },
};

#endif /* TE2_PARAMS_H */
