#!/usr/bin/env python3
# WOZT Capture2Cloud tooltip translation completeness check (patch 0829).
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ('Which mechanism sends the merged controller state to the real console.\n\nChanging backend is saved to scripts/.env and restarts the server cleanly. Backends shown grey are known to the architecture but are not implemented in this build yet.', 'What the browser encoder produces. This affects browsers only.', 'The Nintendo Switch homebrew and Android application use this native protocol server.', 'Feeds a real Wii U GamePad over its dedicated radio path.', 'Uses WUP Stream running on the GamePad; no Nintendo Wi-Fi adapter. Exclusive with the direct radio bridge. Build: scripts/build_wupstream.sh', 'Titan/ConsoleTuner only. Which controller protocol the adapter presents to the console.', 'Joy-Con 1 (L) + Joy-Con 2 (R): the right adapter must support Bluetooth LE with LE 2M and is taken over exclusively during the session. Pair / Sync validates the pair with L+R automatically. Click the stream window to capture the mouse for the Joy-Con 2 mouse mode; Right Ctrl releases it.', 'Pro Controller, or left Joy-Con. The physical Bluetooth address is saved because hci numbers can change after reboot.', 'Used by all pair profiles. In reverse hybrid mode this is the Classic right-hand adapter, not the BLE adapter. Both adapters must be different.', '0 preserves the incoming trigger value. A positive value turns it into a digital 0/100 press.', 'Cardinal-axis saturation point. For example, 90% means an incoming 90% push along X or Y is sent to the console as 100%.', '45-degree saturation point. This is separate from range because many physical stick gates reach a different radius in corners.', 'Brings the video window back -- after closing it by accident, or from headless, where there was none to begin with.', 'Shows the local capture monitor.', 'Runs the configured wake action, then recovers controller output.', 'Starts the Wii U GamePad bridge.', 'Stops the Wii U GamePad bridge.', 'Starts the Wii U GamePad access point.', 'Stops the Wii U GamePad access point.', 'Forces the GamePad to associate again.', 'Requests an H.264 recovery point now.', 'Runs the configured console wake action, then recovers controller output.', 'Performs the normal ordered shutdown and re-execs Capture2Cloud.', 'This backend does not expose a maintenance action.', 'Drops and reopens the Titan USB connection so it handshakes with the console again. This is the software equivalent of the unplug/replug recovery used by this backend.', 'Requests backend-owned paired-Switch recovery. The same operation is used by startup, Maintenance and remote C2C reset requests and does not require GTK.', 'Minimize')
EXPECTED = {'fr','de','it','es','pt','ru','ja','ko','zh-cn'}
seen = set()
errors = []
for path in sorted((ROOT/'data/languages').glob('*.json')):
    obj = json.loads(path.read_text(encoding='utf-8'))
    lang = obj.get('id')
    if lang not in EXPECTED:
        errors.append(f'{path.name}: unknown language {lang!r}')
        continue
    seen.add(lang)
    tr = obj.get('translations', {})
    for key in REQUIRED:
        if not isinstance(tr.get(key), str) or not tr[key].strip() or tr[key].strip() == key.strip():
            errors.append(f'{path.name}: missing translation for {key[:72]!r}')
for lang in sorted(EXPECTED - seen):
    errors.append(f'missing language: {lang}')
if errors:
    for err in errors:
        print('ERROR:', err, file=sys.stderr)
    sys.exit(1)
print(f'WOZT tooltip translations: {len(EXPECTED)} languages × {len(REQUIRED)} sources validated')
