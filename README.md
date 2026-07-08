# zmk-flask-modules

ZMK ports of the [Flask](https://github.com/chowdhuryaj) QMK module family
(`qmk-flask-modules`). Same rule as the QMK side: modules live here and are
consumed via west — never forked into a consumer config. Behaviors are
general-purpose; nothing here is tied to a particular app or workflow.

## Modules

| Module | Status | Flask origin |
|---|---|---|
| `flask_scroll` — rate-capped scroll converter | ✅ | `support_flask.c` drag-scroll dump |
| autoscroll (interval-table jog) | planned | `autoscroll` |
| wheel chords | planned | `wheel_chords` |
| wiggle-ball | planned | `wiggle_ball` |
| per-key / per-layer RGB maps | planned (split-sync research) | nlkb16 RGB channel |
| select-word | planned | `select_word` |
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

The processor only compiles into central/non-split builds (input processors
run on the central side).
