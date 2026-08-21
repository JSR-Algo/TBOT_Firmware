# Lesson-production regression suite

CI runs the portable firmware host probes from this directory. `t54-firmware.sh` retains its
campaign `SKIP_REGATE` classification because it mutates a test source before execution.

Physical H1 playback and robot capture remain outside software CI. No flashing, deployment, or
worker activation is performed by this suite.
