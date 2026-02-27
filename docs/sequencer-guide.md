# MIDI Sequencer Guide

The microkernel includes a pattern-based multi-track MIDI sequencer built as an
actor (`/sys/sequencer`). It runs on both Linux and ESP32, driven by a 5ms
periodic timer with wall-clock tick calculation -- no floating point in the hot
path. This guide covers practical usage. For the technical specification and
future roadmap (AI generation, clip launcher, touch UI), see
[Sequencer.md](Sequencer.md).

## Quick start

```
> midi configure                    # init MIDI hardware
> seq demo                          # load C major scale on track 0
> seq start                         # play (loops by default)
> seq tempo 140                     # change tempo
> seq fx 0 transpose 7              # transpose up a fifth
> seq stop                          # stop playback
```

## Concepts

### Time base

The sequencer uses **480 PPQN** (pulses per quarter note). All positions and
durations are expressed in ticks:

| Musical value | Ticks |
|---|---|
| Whole note | 1920 |
| Half note | 960 |
| Quarter note | 480 |
| Eighth note | 240 |
| Sixteenth note | 120 |
| One bar (4/4) | 1920 |

Tempo is stored as BPM x 100 (integer) -- so 120.5 BPM is `12050`. The tick
engine uses integer-only math: `ticks = elapsed_us * bpm_x100 * PPQN / 6e9`.

### Events

Every musical event is a 16-byte packed struct (`seq_event_t`) with a type,
flags, absolute tick position, and a type-specific data union:

| Type | Data |
|---|---|
| `NOTE` | pitch (microtonal), velocity, channel, duration |
| `NOTE_OFF` | pitch, release velocity, channel |
| `CONTROL` | CC number, 16-bit value, channel |
| `PITCH_BEND` | 14-bit signed value, channel |
| `PROGRAM` | program number, channel |
| `AFTERTOUCH` | value, channel |
| `TEMPO` | BPM x 100 (inline tempo change) |

Pitch is a 16-bit value: upper byte = MIDI note (0-127), lower byte = cents
(0-255 mapped to 0-99). This gives microtonal resolution without floating point.

### Patterns and tracks

- Up to **8 independent tracks**, each with its own pattern
- Each track has **2 pattern slots** (double-buffer) for gapless switching
- Patterns have a **length in ticks** -- tracks with different lengths create
  polyrhythms via per-track modulo wrapping
- **Note-off events are auto-generated** at load time from note durations, then
  the entire event list is sorted. You only need to specify Note On events.

### Looping

Looping is **enabled by default**. Each track wraps independently at its own
pattern length, which is how polyrhythms work -- a 4-bar melody over a 2-bar
bass line naturally creates a 2:1 polyrhythmic feel.

## Transport controls

```
> seq start                         # start from beginning
> seq stop                          # stop, reset position, kill active notes
> seq pause                         # pause/resume toggle
> seq tempo 120                     # set tempo (BPM, 1-300)
> seq status                        # show playback state + per-track info
```

Starting while already playing restarts from the beginning. Stopping kills all
currently sounding notes (sends Note Off for every active note across all
channels).

## Loading patterns

Patterns are loaded via messages (`MSG_SEQ_LOAD_PATTERN`) with a flex-array
payload. The shell provides two built-in demos:

```
> seq demo                          # C major scale, 8th notes, 2 bars, track 0
> seq demo2                         # 2-track polyrhythm:
                                    #   Track 0: 4-bar piano melody (C5-A5)
                                    #   Track 1: 2-bar bass line (C2-A2)
```

From C code, use the convenience constructors and builder:

```c
seq_event_t events[] = {
    seq_note(0,            60, 100, SEQ_PPQN / 2, 0),  /* C4 at tick 0 */
    seq_note(SEQ_PPQN,     64, 90,  SEQ_PPQN / 2, 0),  /* E4 at beat 2 */
    seq_note(SEQ_PPQN * 2, 67, 95,  SEQ_PPQN / 2, 0),  /* G4 at beat 3 */
};

seq_load_payload_t *p = seq_build_load_payload(
    0,                          /* track 0 */
    0,                          /* slot 0 */
    SEQ_TICKS_PER_BAR,          /* 1 bar = 1920 ticks */
    "C major triad",            /* pattern name */
    events, 3);

actor_send(rt, seq_id, MSG_SEQ_LOAD_PATTERN, p, seq_load_payload_size(3));
free(p);
```

## Multi-track

### Mute and solo

```
> seq mute 1                        # mute track 1 (kills active notes)
> seq unmute 1                      # unmute track 1
> seq solo 0                        # solo track 0 (mutes all others)
> seq unsolo 0                      # unsolo track 0
```

Solo uses Ableton-style bitmask logic: multiple tracks can be soloed
simultaneously. When any track is soloed, only soloed tracks produce output.
Muting or unsoloing while playing immediately kills hanging notes on affected
tracks.

### Slot switching

Each track has two pattern slots for gapless A/B switching:

```
> seq switch 0 1                    # queue track 0 to switch to slot 1
```

The switch happens at the next pattern boundary -- the current pattern plays to
completion, active notes are killed, then the new slot becomes active. This
enables seamless arrangement changes without audible glitches.

## Per-track effects

Each track has an inline effect chain with 4 slots. Effects are applied at
playback time on a copy of each event -- the original pattern data is never
modified. This means the same pattern can sound completely different on two
tracks by applying different effects.

### Transpose

Shift pitch by semitones and/or cents. Applies to both Note On and Note Off
events (so the Note Off matches the transposed pitch -- no hanging notes).

```
> seq fx 0 transpose 7              # up a perfect fifth
> seq fx 0 transpose -12            # down an octave
> seq fx 0 transpose 0 50           # up 50 cents (quarter-tone)
```

### Velocity scale

Scale note velocity by a percentage (1-200%). Values are clamped to 1-127.

```
> seq fx 0 velocity 50              # half velocity (quieter)
> seq fx 0 velocity 150             # 150% velocity (louder)
```

### Humanize

Add random variation to note velocity using a fast xorshift32 PRNG (per-track
seed, deterministic). The range parameter sets the maximum +/- deviation.

```
> seq fx 0 humanize 15              # +/- 15 velocity variation
```

### CC scale

Remap a CC value from the full 0-127 range to a custom min-max range. Useful
for constraining a modulation source to a musically useful subset.

```
> seq fx 0 ccscale 1 20 80          # CC1 (mod wheel): remap to 20-80
```

### Managing effects

```
> seq fx 0 clear                    # clear all effects on track 0
> seq fx 0 clear 0                  # clear only slot 0
> seq fx 0 disable 0                # disable slot 0 (keep settings)
> seq fx 0 enable 0                 # re-enable slot 0
```

### Bypass flag

Individual events can set the `SEQ_FLAG_BYPASS_FX` (0x10) flag to skip the
entire effect chain. This is useful for anchor notes that must stay at their
original pitch regardless of transpose settings.

### Chain order

Effects are applied in slot order (0, 1, 2, 3). Order matters:

1. **Transpose +12 then Velocity 50%** -- note goes up an octave, velocity halved
2. **Velocity 50% then Transpose +12** -- same result (these two commute)

But for humanize + velocity scale, order can matter: humanize first adds
randomness to the original velocity, then scaling compresses or expands the
range. Scaling first, then humanizing, operates on the already-scaled value.

## Example session

A complete session showing multi-track playback with effects:

```
> midi configure                    # init MIDI hardware
MIDI configured: I2C0 addr=0x48 SDA=8 SCL=9 IRQ=43 freq=400000

> seq demo2                         # load 2-track polyrhythm
Montage split demo loaded (all ch 0):
  Track 0: 4-bar piano melody (C5-A5)
  Track 1: 2-bar bass line    (C2-A2)
Use 'seq start' to play, 'seq tempo 100' for tempo

> seq tempo 105                     # set tempo
Tempo set to 105.0 BPM

> seq fx 0 transpose 5              # piano up a fourth
Track 0: transpose +5 semi +0 cents → slot 0

> seq fx 0 velocity 80              # soften the piano
Track 0: velocity scale 80% → slot 1

> seq fx 0 humanize 10              # add feel
Track 0: humanize ±10 → slot 2

> seq start                         # play
Sequencer started

> seq status                        # check state
(status request sent)

> seq solo 1                        # hear just the bass
Track 1 soloed

> seq unsolo 1                      # bring piano back
Track 1 unsoloed

> seq fx 0 clear                    # remove all piano effects
Track 0: all effects cleared

> seq stop                          # done
Sequencer stopped
```

## C API reference

### Init

```c
actor_id_t sequencer_init(runtime_t *rt);
```

Spawns the sequencer actor, registers as `/sys/sequencer`. Requires MIDI actor
at `/node/hardware/midi`. Returns `ACTOR_ID_INVALID` if MIDI is not available.

### Message types

| Message | Type | Payload |
|---|---|---|
| `MSG_SEQ_LOAD_PATTERN` | 0xFF000080 | `seq_load_payload_t` (flex array) |
| `MSG_SEQ_START` | 0xFF000081 | (empty) |
| `MSG_SEQ_STOP` | 0xFF000082 | (empty) |
| `MSG_SEQ_PAUSE` | 0xFF000083 | (empty, toggles) |
| `MSG_SEQ_SET_TEMPO` | 0xFF000084 | `seq_tempo_payload_t` |
| `MSG_SEQ_SET_POSITION` | 0xFF000085 | `seq_position_payload_t` |
| `MSG_SEQ_SET_LOOP` | 0xFF000086 | `seq_loop_payload_t` |
| `MSG_SEQ_MUTE_TRACK` | 0xFF000087 | `seq_mute_payload_t` |
| `MSG_SEQ_SOLO_TRACK` | 0xFF000088 | `seq_solo_payload_t` |
| `MSG_SEQ_SWITCH_SLOT` | 0xFF000089 | `seq_switch_slot_payload_t` |
| `MSG_SEQ_SET_FX` | 0xFF00008B | `seq_set_fx_payload_t` |
| `MSG_SEQ_CLEAR_FX` | 0xFF00008C | `seq_clear_fx_payload_t` |
| `MSG_SEQ_ENABLE_FX` | 0xFF00008D | `seq_enable_fx_payload_t` |
| `MSG_SEQ_STATUS` | 0xFF000092 | (send empty, reply: `seq_status_payload_t`) |

### Convenience constructors

```c
seq_event_t seq_note(tick_t tick, uint8_t note, uint8_t vel,
                     tick_t duration, uint8_t channel);

seq_event_t seq_cc(tick_t tick, uint8_t cc, uint8_t val, uint8_t channel);

seq_event_t seq_program(tick_t tick, uint8_t program, uint8_t channel);
```

### Effect types

```c
typedef enum {
    SEQ_FX_NONE = 0,
    SEQ_FX_TRANSPOSE,        /* semitones + cents */
    SEQ_FX_VELOCITY_SCALE,   /* percentage (1-200) */
    SEQ_FX_HUMANIZE,         /* random velocity +/- range */
    SEQ_FX_CC_SCALE,         /* remap CC to min-max range */
} seq_fx_type_t;
```

## Design notes

**Why integer math?** The ESP32 has no FPU on most variants, and even on the
S3 (which has one), avoiding float keeps the tick calculation branch-free and
deterministic. BPM x 100 gives 0.01 BPM resolution without ever touching a
float.

**Why note-off expansion at load time?** Inserting note-off events and sorting
once at pattern load means the playback engine is a simple linear scan -- no
duration tracking, no pending-note-off queue, no priority heap. The 5ms timer
just walks forward through the sorted event list.

**Why copy-then-apply for effects?** The FX chain operates on a stack copy of
each event before emission. This means: (1) the original pattern data is never
modified, so clearing effects restores the original sound instantly; (2) active
note tracking sees the *transformed* pitch, so Note Off always matches the
actual MIDI note that was sent -- no hanging notes even with transpose.

**Why inline effect chains?** Four effects per track, stored inline in the
track struct (no heap allocation). On an ESP32 with limited RAM, this is the
difference between "works" and "works reliably." The entire sequencer state
including all 8 tracks, 16 pattern slots, and 32 effect slots fits in a single
`calloc`.
