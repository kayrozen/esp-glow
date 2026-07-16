;; samples/demo-boot.fnl -- pad-addressing acceptance demo for the APC40 mkII
;; (samples/apc40.mdef). Every pad is addressed by (col, row); glow.bind.pad-xy
;; and glow.led.auto-xy resolve the same coordinate through the .mdef to the
;; same (note, channel), so no note number is ever hand-copied between a
;; binding and its LED feedback -- see README_LIVE_CONTROL.md.
;;
;; Grid: col = track (0..7), row = scene (0..4), the APC40's 8x5 clip-launch
;; matrix. Loads clean on the real mkII .mdef; on a smaller or grid-less
;; controller every pad-xy/led-xy call below degrades to a silent no-op.

(glow.cue.define :warm   {:effects []})
(glow.cue.define :cool   {:effects []})
(glow.cue.define :chorus {:effects []})
(glow.cue.define :verse  {:effects []})

;; Scene row 0: momentary looks on tracks 1 and 2.
(glow.bind.pad-xy 0 0 :flash :warm)
(glow.led.auto-xy 0 0 :warm :red :off)

(glow.bind.pad-xy 1 0 :flash :cool)
(glow.led.auto-xy 1 0 :cool :blue :off)

;; Scene row 1: latched section cues on tracks 1 and 2.
(glow.bind.pad-xy 0 1 :toggle :chorus)
(glow.led.auto-xy 0 1 :chorus :green :off)

(glow.bind.pad-xy 1 1 :toggle :verse)
(glow.led.auto-xy 1 1 :verse :yellow :off)
