# main: device composition root

Initializes platform services and wires the VOX M-01 components, in order:
ESP-Iris boot probe, NVS, Recovery support (`iris_ota_support_start`, always
first so Recovery stays reachable), `app_secrets` (provisioning RPC),
`app_wifi`, the GSP display with `app_vox_eye`, and finally `app_vox_session`.
It contains no conversation, animation or drawing rules and exports no headers.

Callbacks adapt component interfaces only: eye taps → `app_vox_session_tap()`,
Wi-Fi/secrets changes → session events, session views → eye states. The private
composition context retains only owned opaque handles; their lifetime extends
until reboot. Bundle or display initialization failure leaves Recovery and the
session (logs only) running; other unrecoverable initialization errors use
ESP-IDF's error-check policy. Central dependency pins/overrides live in
`idf_component.yml`.
