# Capture2Cloud: builds clients et releases multi-plateformes

Ce pipeline est lancé sur chaque push `main`, sur les pull requests et manuellement. Il produit une AppImage Linux ainsi qu'un ZIP serveur et compile les clients à partir du commit courant : Android, Switch, Wii U Console, Wii U GamePad (programme PC qui pilote le GamePad) et Old 3DS/2DS.

## Prérequis et provenance

- **Android** : Android SDK 36, Java 21, Gradle du dépôt. Pour construire un APK **signé en release** et compatible avec les mises à jour de l'APK historique, définir les secrets GitHub `C2C_ANDROID_KEYSTORE_B64` (base64 du fichier `.jks`) et `C2C_ANDROID_PASSWORD`. Le keystore utilise l'alias `capture2cloud` conformément à `android/app/build.gradle.kts`. Sans ces deux secrets, un APK de développement **explicitement nommé** `capture2cloud-android-debug.apk` est publié ; ce n'est pas une mise à jour compatible avec l'APK signé des anciennes releases. Ne jamais committer le keystore.
- **Nintendo Switch** : image officielle `devkitpro/devkita64`, compilation de `switch_homebrew/Makefile` en NRO.
- **Wii U Console** : image `devkitpro/devkitppc`, compilation de `wiiu_console/Makefile` en RPX et WUHB.
- **Old 3DS/2DS** : image `devkitpro/devkitarm`, compilation de `3ds/Makefile` en CIA et 3DSX. La reconstruction de FFmpeg et de makerom peut être longue.
- **Wii U GamePad** : programme x86_64 compilé depuis `wiiu_gamepad/Makefile`, utilisant `libdrc` et x264 ABI 140 copiés depuis le commit épinglé `06922d55645a13b2558990dd3c7bde3212c9443c` de `wozt/bottom_screen` (`gamepad/vendor/`). Ce ZIP **n'est pas** un exécutable Wii U console : c'est le client PC de la radio GamePad.

Les SDK cross-compilation devkitPro et leurs portlibs sont installés dans les jobs dédiés. Ces images et paquets externes doivent être validés dans les premières GitHub Actions ; une dépendance introuvable fait échouer la CI, jamais de remplacement silencieux par une archive historique.

## Assets attendus

- `capture2cloud-x86_64.AppImage` et `capture2cloud-linux-x86_64.zip`
- `capture2cloud-<version>.apk` si signé, ou `capture2cloud-android-debug.apk` sinon
- `capture2cloud-switch.zip`
- `capture2cloud-wiiu-console.zip`
- `capture2cloud-wiiu-gamepad.zip`
- `capture2cloud-old3ds.zip`
- `SHA256SUMS` ; l'AppImage conserve également son `.sha256`

La validation `tools/check-release-assets.py` vérifie les noms et le contenu minimal de chaque ZIP. Le job `release` dépend de la réussite de **tous** les builds et ne publie jamais une release partielle. Les tags Git incrémentent automatiquement le quatrième composant suivant le standard, mais la version embarquée reste celle du fichier `VERSION` tant qu'un changement volontaire de version n'est pas effectué dans le code. Les noms d'assets client non versionnés restent compatibles avec les anciennes releases.

## Tests GTK locaux

La suite `tests/run_all.sh` fournit `C2C_RESOURCE_ROOT` **au seul test GTK**, pour que ce binaire construit dans `tests/.build` trouve les thèmes JSON, les traductions et l'icône dans la racine du projet. Cette variable n'est pas utilisée par la configuration persistante, les identifiants ni les backends.

Pour tester séparément :

```bash
cd ~/dev/capture2cloud
./tests/run_all.sh
```

Le `test_gtk_layout` vérifie que les 7 thèmes, les 9 langues et les 8 sections sont effectivement chargés. En cas de nouveau segfault, le lanceur tente de produire un backtrace avec GDB lorsque celui-ci est installé ; il ne masque pas la sortie d'erreur du test.
