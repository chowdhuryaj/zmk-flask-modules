# zmk-flask-modules

ZMK ports of the [Flask](https://github.com/chowdhuryaj) QMK module family
(`qmk-flask-modules`). Same rule as the QMK side: modules live here and are
consumed via west — never forked into a consumer config. Behaviors are
general-purpose; nothing here is tied to a particular app or workflow.

## Modules

| Module | Status | Flask origin |
|---|---|---|
| `flask_scroll` — rate-capped scroll converter | ✅ (unused on Imprint — AJ prefers the stock ZMK scroll chain, 2026-07-08) | `support_flask.c` drag-scroll dump |
| `flask_autoscroll` — hands-free continuous scroll (stepped + jog) | ✅ | `autoscroll` |
| `flask_proto` — raw-HID runtime tuning (meta / dragscroll / autoscroll) | ✅ | `mad_hid.c` frame |
| wheel chords | planned | `wheel_chords` |
| per-key / per-layer RGB maps | planned (split-sync research) | nlkb16 RGB channel |
| mouse gestures | adopted upstream: [kot149/zmk-mouse-gesture](https://github.com/kot149/zmk-mouse-gesture) | `pd_gestures` |
| os shortcuts (runtime mac/pc) | adopted upstream: [mctechnology17/zmk-switch-layout](https://github.com/mctechnology17/zmk-switch-layout) | `os_shortcuts` |

## Usage

`west.yml`:

```yaml
  - name: zmk-flask-modules
    remote: chowdhuryaj
    revision: main
```

### flask_scroll

Converts scroll-ball REL_X/REL_Y into rate-capped wheel notches: integer
divisor with remainder carry, fixed dump interval, per-interval notch cap,
surplus carried forward. Fast rolls can't flood slow scroll consumers,
reversals cancel pending surplus before output flips sign, and no counts are
ever lost to the cap.

```dts
#include <flask_scroll.dtsi>

&flask_scroll {
    divisor-x = <16>;    /* ball counts per horizontal notch */
    divisor-y = <12>;    /* ball counts per vertical notch */
    interval-ms = <16>;
    max-notches = <3>;
    invert-x;
};

/* scroll ball listener: swallow motion into the converter */
&trackball_central_listener {
    input-processors = <&flask_scroll>;
};

/* wheel notches re-enter the pipeline from the flask_scroll node */
/ {
    flask_scroll_out {
        compatible = "zmk,input-listener";
        device = <&flask_scroll>;
        /* optional: &zip_scroll_snap etc. */
    };
};
```

`divisor` sets both axes at once; `divisor-x`/`divisor-y` override per axis.

### flask_autoscroll

Hands-free continuous scrolling, port of the Flask (QMK) autoscroll module
(Ben White's stepped interval table + Contour Shuttle jog model). Stepped
mode: `&asc ASC_UP` / `&asc ASC_DOWN` move a signed speed level (-9..9,
through zero = stop) ticking the wheel every {1000,500,200,100,67,50,40,33,
25} ms. Jog mode: `&asc ASC_JOG` toggles the ball into a jog wheel —
vertical deflection sets direction and speed continuously (deadzone around
center, full deflection = level-9 speed), ball motion swallowed. Any other
key press stops either mode (runtime-tunable). Tunables live on the Flask
protocol's `0x1A` channel (same value ids as the QMK families) and persist
via `flask/autoscroll` settings on save.

```dts
#include <flask_autoscroll.dtsi>
#include <dt-bindings/flask/autoscroll.h>

&flask_autoscroll {
    speed-scale = <100>;  /* x100; 200 = twice as fast */
    jog-deadzone = <15>;  /* counts ignored around center */
    jog-range = <300>;    /* counts past deadzone to full speed */
    /* inverted; */
    /* stop-on-key = <0>; */
};

/* ahead of the scroll converter on the scroll ball: transparent unless
 * jogging (jog swallows raw motion) */
&trackball_central_listener {
    input-processors = <&flask_autoscroll &flask_scroll>;
};

/* wheel ticks re-enter the pipeline from the flask_autoscroll node */
/ {
    flask_autoscroll_out {
        compatible = "zmk,input-listener";
        device = <&flask_autoscroll>;
    };
};
```

Keymap: `&asc ASC_UP`, `&asc ASC_DOWN`, `&asc ASC_JOG`, `&asc ASC_STOP`.

The processors only compile into central/non-split builds (input processors
run on the central side).
