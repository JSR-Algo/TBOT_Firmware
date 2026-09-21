# Attended M1 staging profile

`CONFIG_TBOT_M1_STAGING` defaults off. The profile targets LCDWiki ES3C35P /
ESP32-S3 and pins the M0 public API, OTA, WebSocket and provisioning endpoints.
It is separate from the private-LAN profile and retains BluFi, physical claim
confirmation, TLS and application authentication.

Append `config/m1-staging.defaults` to the normal LCDWiki defaults chain. Select
`IDF_TARGET=esp32s3` explicitly in a fresh build directory and use the selected
ESP-IDF Python/toolchain environment. Preserve the selected `dependencies.lock`;
unexpected dependency resolution invalidates the build. The configuration audit
is `scripts/assert_lcdwiki_m1_staging_config.py`. The artifact auditor accepts
`--profile m1-staging` and requires the embedded `m1-staging-v1` identity plus
the ordinary non-HIL safety/partition/symbol checks. Production audits reject
the staging flag and embedded identity.

Staging credentials, claim/reset state and backend-owned lesson records use
separate NVS namespaces. Saved ordinary credentials are not copied, migrated or
used as a fallback. Physical board UUID, Wi-Fi credentials and radio settings
remain shared. The two Wi-Fi endpoint keys are pinned on reads and cannot be
overwritten or individually erased through Settings while staging runs.

Bootstrap, claim and runtime replies must retain the selected API/WS endpoints.
OTA replies cannot activate MQTT, factory-claim shortcuts or firmware updates.
Direct firmware/partition-asset upgrade paths are also refused. Boot-time NVS
exhaustion/corruption fails closed instead of erasing saved storage; auth recovery
and re-pairing clear isolated claim state without forgetting ordinary Wi-Fi.

Restoring the ordinary application selects its original namespaces again.
These controls do not prove actual NVS free capacity, successful claiming or
physical restore. Do not flash until both clean reproducible builds, all required
software gates, exact signed physical admission and the reviewed app-only restore
procedure are ready. Never run the generic erase/reset commands printed by an
ESP-IDF build as the M1 flash procedure.

M1 does not authorize lesson assignment or shared SD pack activation/eviction.
Those operations need their own preservation proof before later milestones.
Connection/preflight is not a lesson, speech-smoothness or displayed-quality
receipt. Original media remains untouched by this profile.
