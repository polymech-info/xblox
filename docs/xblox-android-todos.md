# Tanit XBlox — Android block roadmap

**Status:** Proposed handover · **Platform:** Native Android (Kotlin/Jetpack Compose) + shared C++/JNI XBlox runtime · **Priority:** End-user benefit, not number of APIs exposed · **Date:** 2026-11-15

## 1. Product direction

The useful abstraction is **“When X happens on my phone, do Y and optionally send the result to a chat.”** Do not turn the editor into a catalog of raw Android methods. Expose high-level triggers/actions, and put sensor access, operating-system wakeups and permission handling behind a Kotlin adapter.

The attached XBlox catalog already contains `onStart`, `wait`, `if`, `llmAgent`, `referenceInChat`, HTTP `fetch`, audio recording/playback/TTS/STT, `voiceListen`, Bluetooth, files, MQTT and camera/video blocks. **Their presence in the catalog does not establish that each implementation currently works on Android.** Extend/port existing actions instead of duplicating them as `android*` blocks. The new work below concerns phone-specific triggers, side effects and lifecycle behavior.

Priorities: **P0** = everyday, obvious value / foundation for other blocks; **P1** = strong automation features with narrower use cases or more permissions; **P2** = niche, battery-intensive, restricted or hardware-dependent. These are suggested public block labels and stable internal `kind` names, not claims about existing registration.

## 2. P0 — Essential / highest user value

| Block(s) | Role | What users can do | Android implementation / caveat |
|---|---|---|---|
| **Show / Update / Cancel Notification** `androidNotify` | Action | “Send me a morning briefing”; alert when a task finishes | `NotificationManager`, channels, `POST_NOTIFICATIONS` on Android 13+; stable notification IDs, tap/deep-link and optional action buttons |
| **On Notification Action** `androidOnNotificationAction` | Trigger | Tap **Summarize**, **Retry**, **Open chat** or **Dismiss** | Explicit `PendingIntent` receiver; correlate `taskId`, `runId`; support optional inline reply through `RemoteInput` |
| **Run at Time / After Delay / Repeat** `androidSchedule` | Trigger registration | “Tomorrow 14:30 call API”; “send stock developments every morning” | **Bridge into the existing scheduler** rather than duplicate schedule logic; use `WorkManager` for deferrable work, `AlarmManager` only for eligible precise alarm use cases |
| **Write Result to Chat** `androidChatResult` | Action | Scheduled agent writes a new conversation or appends to its own thread; user can iterate | Persistent run/result record; `newThread` / `appendThread` / `notificationOnly`; link notification to output; never silently overwrite a conversation |
| **Receive Share / Open in Tanit** `androidOnShare` | Trigger | Share document, image, URL or selected text → summarize, read aloud, chat, save | `ACTION_SEND`, `ACTION_SEND_MULTIPLE`, `ACTION_VIEW`, persisted/granted `content://` URI handling; reuse existing VFS pipeline |
| **Share / Open Link or App** `androidShare`, `androidOpenIntent` | Action | Send generated file to another app; open navigation, settings, browser | Android Sharesheet and explicit safe intents; background activity-launch restrictions; allowlist dangerous actions |
| **Get Current Location** `androidLocationGet` | Query | Weather for here, attach location to a note, find nearby place | Fused Location Provider or framework `LocationManager`; coarse/fine permission, accuracy + age, timeout, null/unavailable |
| **On Enter / Exit / Dwell at Place** `androidGeofence` | Trigger registration | Remind me near the supermarket; turn off work alerts after leaving office | GeofencingClient + `PendingIntent`, background location when necessary; approximate, not immediate; expose radius, dwell, expiration and availability |
| **Battery / Charging State** `androidPowerState`, `androidOnPower` | Query + trigger | Start upload on charging; warn at 15%; wait for battery to recover | `BatteryManager`, power broadcasts, power-save state; thresholds/debounce; avoid polling |
| **Network State / Changed** `androidNetworkState`, `androidOnNetwork` | Query + trigger | Sync only on Wi-Fi / unmetered connection; retry on internet return | `ConnectivityManager.NetworkCallback`; distinguish `connected`, `validated`, `metered`, transport; callbacks not guaranteed persistent after process death |
| **User Quick Action** `androidQuickAction` | Trigger | Run a flow from a home widget, app shortcut or Quick Settings tile | Static entrypoints plus user-configured flow IDs; `AppWidgetProvider`, shortcuts, `TileService`; route through one dispatcher |
| **Permission / Capability Check** `androidCapability` | Query/action | Show why a flow cannot run; request just the permission needed | Capability registry with `available` / `permissionNeeded` / `settingsNeeded` / `restricted` / `unsupported`; request permissions only from visible UI |

**P0 release criterion:** A user can create **one scheduled briefing, one location reminder, one share-to-agent flow and one battery/network-constrained flow**; each can launch from a cold process, produce an actionable notification, write an inspectable result to chat, and be cancelled from the app.

### Important scheduling nuance

`WorkManager` periodic work has a **15-minute minimum interval** and does not promise an exact clock minute. Precise alarm access is restricted and should not be claimed for general automations; use an explicit exactness capability and truthful fallback. A **5-minute recurring job** is not a normal persistent `PeriodicWorkRequest`; offer an active user-visible mode only where a legitimate foreground service use case applies, or make timing best-effort. Distinguish `dueAt`, `startedAt`, `finishedAt` and `delayedReason` in run history.

## 3. P1 — High-value optional capabilities

| Block(s) | Role | User scenarios | Notes |
|---|---|---|---|
| **On Shake / Flip / Orientation** `androidOnGesture` | Trigger | Shake to start recorder, flip face-down for silent mode, double-shake to bookmark | Use `SensorManager` accelerometer + gravity/rotation; debounce, cooldown, sensitivity presets; **not guaranteed to wake a killed/background app** |
| **Activity Changed** `androidOnActivity` | Trigger | “When driving, read urgent messages aloud”; commute logging | Activity Recognition Transition API where Play services are present; `ACTIVITY_RECOGNITION`; confidence and false positives |
| **Step Count** `androidSteps` | Query / threshold | Walking goals, activity summaries | `TYPE_STEP_COUNTER` if present; since-boot counter may reset; activity permission |
| **Read Incoming Notifications** `androidNotificationWatch` | Trigger | Summarize selected app notifications, extract delivery updates | Opt-in `NotificationListenerService` + special user-granted Notification Access, app allowlist, redact sensitive data; Android 15+ may hide OTP content |
| **Bluetooth / Headset Connected** `androidOnBluetooth` | Trigger | Start walkie-talkie when earbuds connect; connect to a device | Extend catalog Bluetooth abstractions, Android `BLUETOOTH_CONNECT` on modern targets; audio routing is platform-controlled |
| **BLE Device Nearby / Left** `androidBlePresence` | Trigger | At the workshop when a beacon is visible; home-device proximity | BLE filtered scan with `PendingIntent` or `CompanionDeviceManager`; missing signal is not proof of departure; rate limits/power |
| **NFC Tag Scanned** `androidOnNfc` | Trigger | Tap a tag on a machine to launch an inspection checklist | Intent dispatch / `NfcAdapter` reader mode; device support and unlocked-state constraints |
| **Scan QR / Barcode** `androidScanCode` | Action/trigger | Scan a product label, pair desktop, open equipment profile | Explicit camera UI/consent; integrate CameraX/ML Kit or on-device decoder; don't invoke unattended background camera |
| **Vibrate / Haptic Pattern** `androidVibrate` | Action | Discreet feedback, pattern alerts | `VibratorManager`, vibration capability and device silent/DND policy; short bounded patterns |
| **Phone / Device State** `androidDeviceState`, `androidOnDeviceState` | Query + trigger | React to screen on/off, device unlocked, headset unplugged, locale/time-zone change | Use documented system broadcasts/callbacks only; some events require dynamic registration or are not wakeup-capable |
| **File / Media Selected or Changed** `androidOnMedia` | Trigger | Summarize an imported PDF, transcribe newly selected recording | SAF/MediaStore content URIs and app-owned watchers; no universal silent watch of every folder/other app |
| **Read Calendar / Create Event** `androidCalendar` | Query/action | Prep before a meeting, create event after voice command | `CalendarContract` with permission or explicit calendar intent; prefer account/provider integration where available |
| **Contacts Pick / Share Contact** `androidContact` | Query/action | “Send this summary to this contact” | System Contacts Picker preferred; avoid blanket `READ_CONTACTS` where possible |
| **Audio Route / Media Action** `androidAudioRoute`, `androidMediaAction` | Query/action | TTS through connected earbuds, pause own playback, resume audiobook | `AudioManager`, `MediaSession`; do not imply arbitrary control of other apps without authorized APIs |

**P1 release criterion:** A gesture or notification trigger can run a bounded flow with a permission explanation, user-visible monitoring state, false-trigger protection, and a reliable way to disable the subscription.

## 4. P2 — Advanced / specialized / defer

| Block(s) | Why lower priority | Implementation guardrail |
|---|---|---|
| **Raw Sensor Sample / Stream** `androidSensorRead`, `androidSensorStream` | Accelerometer, gyroscope, compass, rotation vector: powerful for advanced builders, not everyday tasks | Enumerate actual hardware; specify sampling/batching/duration; bounded buffer and battery budget; cannot promise general always-on background streaming |
| **Environmental Sensor** `androidEnvironment` | Light, proximity, pressure, ambient temperature, humidity (often absent) | Hardware checks and `unsupported` outcomes; no fabricated values |
| **Continuous GPS Track** `androidLocationTrack` | Maps/trips useful but significant privacy and battery impact | Explicit start/stop, foreground notification, background permission if applicable, retention cap, no stealth tracking |
| **Device Diagnostics** `androidDeviceInfo` | Useful for power users/support flows | Battery health where available, thermals, storage, free memory; don't request privileged identifiers |
| **Wi-Fi/SSID or Network Proximity** `androidWifiInfo` | Potential home/work triggers, unreliable/permission-heavy across Android versions | Network-state events first; SSID/BSSID access requires additional context and permissions, no programmatic enable/disable Wi-Fi promise |
| **USB Peripheral / Serial** `androidUsbDevice` | Strong for industrial/ESP32 audience, niche general appeal | `UsbManager` user grant, explicit device identity, detach events, IO limits |
| **Accessibility UI Automation** `androidAccessibilityAction` | Powerful but sensitive, brittle and Play-policy-constrained | Separate opt-in module; never hide automation or bypass consent; do not assume universal screen control |
| **SMS / Call Log / Call-State Actions** `androidTelephony` | Android and Play policies limit access; privacy-sensitive | Prefer user-initiated intents, default-role flows, or special distribution; not baseline product feature |
| **Background Camera / Screen Capture** — **do not promise as generic scheduled action** | User expects unattended camera shots, but platform restrictions make it unreliable or disallowed | Require visible/user-authorized capture session, appropriate foreground service and MediaProjection approval where relevant |
| **DND / System Toggles / Device Admin** `androidSystemSettings` | Hardware and policy controlled | Open relevant Settings panels; do not present root/system-only toggles as ordinary actions |

## 5. Execution architecture (essential)

```text
Android OS event / PendingIntent / notification action / widget / share intent
       ↓
Kotlin trigger adapters  [register, unregister, capability check]
       ↓
Durable event inbox + deduplication + task/subscription database
       ↓
Background dispatcher  [WorkManager / alarms / permitted service]
       ↓
JNI → shared C++ XBlox executor  [bounded run, timeout, cancellation]
       ↓
Result store → chat thread + Android notification + run history
```

**Do not require a living XBlox session for scheduled/device events.** Registration and execution are separate stages. An event can wake the app's receiver, persist its payload, and launch a short run in a new process. Only keep an active session for a *user-visible* live-monitoring mode that legitimately needs it.

Recommended internal structure:

- `AndroidCapabilityRegistry`: check installed hardware, runtime permission, special app access, Play services availability, app state and restrictions.
- `AndroidEventBridge`: Kotlin adapters publish versioned events; native core consumes a common envelope. Prefer one shared event format, not dozens of JNI callback shapes.
- `AndroidSubscriptionStore`: `subscriptionId`, `flowId`, trigger config, enabled state, permission state, registered-at, next due; re-register appropriate listeners after reboot/app update and configuration changes.
- `AndroidDispatchWorker`: native library load on demand, constrained execution time, cancellation, rate limit and result commit. Long work must be scheduled with an eligible mechanism, not a receiver kept alive.
- `AndroidResultRouter`: `notification`, `newChat`, `appendChat`, `silentHistory`; notification opens correct chat/run; offline results queue locally.

**Versioned event envelope:**

```json
{
  "schema": "tanit.android.event/v1",
  "eventId": "unique-event-id",
  "subscriptionId": "subscription-id",
  "flowId": "flow-id",
  "kind": "location.geofence.enter",
  "occurredAt": "2026-11-15T12:00:00Z",
  "receivedAt": "2026-11-15T12:00:40Z",
  "payload": {"placeId": "workshop", "transition": "enter"},
  "delivery": {"attempt": 1, "source": "pendingIntent"}
}
```

`PREVIOUS` should receive this typed envelope for trigger flows. Keep old `onStart` behavior intact. Event delivery is **at least once**: deduplicate using `eventId` and idempotent external side effects. Record drops, delays, retry attempts and permission failures; no silent success.

## 6. Block UI contract and safety

- **Trigger vs action:** `When...` triggers have nested child blocks; `Get...` queries produce values; `Do...` actions return a structured result. Make this visually obvious in XBlox.
- **Scope and consent:** Ask only when a block/flow is activated. Location, notification listening, mic/camera, contacts and accessibility require contextual explanations; settings-only grants need guided Settings navigation.
- **Capabilities:** Every block defines `supported`, `needsPermission`, `needsUserAction`, `backgroundMode`, `batteryCost`, `accuracy`, and `PlayStoreConstraints`. Hide/disable unsupported devices and show why.
- **Scheduling:** User picks `exactNeeded` vs `bestEffort`, timezone, DST handling, miss policy (`skip`/`runLate`), network and charging constraints, retry policy, quiet hours.
- **Subscriptions:** A single management screen lists *all* active flows (time, geofence, notifications, sensors, BLE), next expected event and last run, with pause/delete and permission status.
- **Data hygiene:** Do not send full notification bodies, precise location or raw sensor streams to LLMs by default. Store minimum trigger metadata; explicit opt-in for cloud transmission; retention limits for location and notification histories.
- **Resource policy:** Sensor streams require a foreground context where applicable and automatic unsubscribe; BLE scanning needs filters; location uses low-power modes and dwell instead of GPS polling; flows have max concurrent runs, duration and output size.
- **Failure states:** `permission_denied`, `capability_missing`, `background_restricted`, `delayed`, `expired`, `cancelled`, `retrying`; show them in logs and editor validation.
- **No automatic permission bypass:** Background camera/microphone, screen capture, DND toggles, SMS and other-app actions are not generic OS powers.

## 7. Example automations to validate the API

1. **Daily briefing** — `androidSchedule(08:00, daily)` → existing `fetch` → `llmAgent` → `androidChatResult(newChat|appendChat)` → `androidNotify(openChat)`.
2. **Shop reminder** — `androidGeofence(enter, shop, 150 m)` → `androidNotify("Buy screws")` → optional **Done** action.
3. **Desk → phone share** — `androidOnShare(document)` → existing document reader / `llmAgent` → `androidChatResult` → open result.
4. **Smart upload** — `androidOnPower(plugged)` + check `androidNetworkState(unmetered)` → existing file/network block → completion notification.
5. **Shake to record** — `androidOnGesture(shake, cooldown=5 s)` → existing `audioRecord` / `voiceListen` → attach to chat; mark as foreground-only unless supported active monitoring is running.
6. **Bluetooth arrival** — `androidBlePresence(device)` → check debounce → optional MQTT command → notification. Never infer departure from one missed BLE advertisement.
7. **Read an alert** — `androidNotificationWatch(allowedApp)` → filter / redact → `llmAgent` summary → new chat or notification. Special opt-in permission and app-specific filters.

## 8. Build sequence and acceptance criteria

**Milestone A — Event infrastructure:** Capability registry, durable event envelope, registration APIs, cold-start dispatcher, permission UX, cancel/pause, result history and chat routing. Unit tests for replay/dedup and reboot recovery.

**Milestone B — Everyday P0:** Notifications/actions, schedule bridge, battery/network, Android share/intents, single location request. E2E tests on Android 14+ device with process killed, screen off, airplane mode, battery saver and revoked permissions.

**Milestone C — Context triggers:** Geofences, quick actions, notification tap/inline actions, flow editor UX. Test geofence delays and stale location; show uncertainty rather than promise instant triggers.

**Milestone D — P1 sensors:** Shake/rotation while active, Activity Recognition, steps, Bluetooth/BLE, optional Notification Access and NFC. Measure battery draw and false positives before shipping defaults.

**Hard acceptance conditions:**

- No loss of saved schedules/subscriptions after reboot, app update or ordinary process death; re-register providers where required. Explicitly explain force-stop and OEM battery restrictions when relevant.
- Permission revoked mid-flow produces a visible failure, not an infinite retry.
- Duplicate OS events do not repeat a paid API call or send duplicate alerts.
- Every flow can be paused, cancelled and inspected; no orphan foreground services or sensor listeners.
- Background runs have bounded native memory/CPU/time; execution can be resumed or marked failed without corrupting chat results.
- Android / shared core block metadata remains discoverable by `xblox info --json`, with platform availability rather than duplicate registries.

## 9. Official Android references

- Background work and WorkManager: https://developer.android.com/develop/background-work/background-tasks/persistent
- Periodic work minimum 15 minutes: https://developer.android.com/develop/background-work/background-tasks/persistent/getting-started/define-work
- Exact alarms and special access: https://developer.android.com/develop/background-work/services/alarms
- Foreground service restrictions: https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start
- Geofences, permissions and limits: https://developer.android.com/develop/sensors-and-location/location/geofencing
- Background location: https://developer.android.com/develop/sensors-and-location/location/background
- Motion sensors and Android 9+ restrictions: https://developer.android.com/develop/sensors-and-location/sensors/sensors_overview
- Activity recognition: https://developer.android.com/develop/sensors-and-location/location/transitions
- Bluetooth background presence: https://developer.android.com/develop/connectivity/bluetooth/ble/background
- Android 15 notification OTP restrictions: https://developer.android.com/about/versions/15/behavior-changes-all
