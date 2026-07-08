# zmk-flask-modules

ZMK ports of the [Flask](https://github.com/chowdhuryaj) QMK module family
(`qmk-flask-modules`). Same rule as the QMK side: modules live here and are
consumed via west — never forked into a consumer config.

## Modules

| Module | Status | Flask origin |
|---|---|---|
| `flask_scroll` — PACS-safe scroll converter | ✅ | `support_flask.c` drag-scroll dump |
| autoscroll (Ben White jog) | planned | `autoscroll` |
| wheel chords | planned | `wheel_chords` |
| 8-way ratchet gestures | planned | `pd_gestures` |
| wiggle-ball | planned | `wiggle_ball` |
| select-word | planned | `select_word` |
| os-shortcuts (runtime mac/pc) | planned | `os_shortcuts` |

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
surplus carried forward. Fast rolls can't flood slow viewers (iSite PACS),
reversals cancel pending surplus before output flips sign, and no counts are
ever lost to the cap.

```dts
#include <flask_scroll.dtsi>

&flask_scroll {
    divisor = <12>;      /* ball counts per notch; 8 PC / 120 macOS in Flask */
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

The processor only compiles into central/non-split builds (input processors
run on the central side).
