# N64 : émulateur Nintendo 64 pour PlayStation 3 (version 6.0)

Projet écrit de zéro pour PS3 (PSL1GHT, CFW). Il se construit étape par étape. Jeux prioritaires : **Super Mario 64, Mario Kart 64, Ocarina of Time**.

## Version 6.0 : la grosse mise à jour (recompilateur + 2e fil)

Mesures sur PS3 (5.5), Mario 64 en jeu : 37 % de la vitesse, CPU ≈ 50 % du temps, 3D ≈ 34 %. L'interpréteur ne pouvait pas aller beaucoup plus vite. Cette version change donc la façon d'émuler.

### 1. Recompilateur MIPS → PowerPC (option RECOMPILATEUR, activée par défaut)
- Le code du jeu N64 est **traduit en code PS3** par blocs (jusqu'à 64 instructions), puis exécuté directement par le processeur. Avant, chaque instruction était décodée une par une.
- Les registres N64 les plus utilisés restent dans des registres PS3 (r14–r21).
- Les blocs s'enchaînent sans repasser par le C.
- Traduits directement :
  - calcul entier, décalages 32/64 bits, MULT/MULTU ;
  - ADD/SUB/ADDI avec le vrai piège de débordement ;
  - lectures et écritures en RDRAM ;
  - tous les branchements ;
  - déplacements flottants LWC1/SWC1/LDC1/SDC1/MTC1/MFC1, dans les deux modes FR.
- Le calcul flottant passe par l'interpréteur, mais sans vider les registres (appel léger).
- Le code modifié par le jeu ou par un DMA est détecté et retraduit.
- **Comment ça marche sur PS3** : un programme ne peut pas écrire dans sa propre zone de code. Le recompilateur utilise donc **PS3MAPI** (Cobra / webMAN MOD) pour écrire le code traduit dans une zone de 8 Mo réservée dans l'exécutable. Au démarrage, un petit test vérifie que le code écrit s'exécute vraiment.
- **Sans PS3MAPI**, l'émulateur repasse automatiquement en interpréteur. SELECT → INFOS affiche « RECOMPILATEUR ACTIF (PS3MAPI x.xx) » ou la raison de l'échec.
- **Tests sur PC** : 313 tests comparent le code traduit à l'interpréteur, instruction par instruction (programmes aléatoires, exceptions, code auto-modifié, mode FR 0/1). 72 encodages sont vérifiés contre un assembleur PowerPC de référence.
- **Mario 64 sur PC** : 99 % des instructions sont exécutées par le code traduit, environ 96 instructions N64 par appel. Les images du jeu sont identiques à celles de l'interpréteur.

### 2. La 3D sur le 2e fil du processeur (option DESSIN 3D EN PARALLÈLE)
- Le PPU de la PS3 exécute deux fils à la fois. Les tâches graphiques (microcode + RDP) sont maintenant dessinées par le 2e fil, pendant que le 1er émule le processeur N64. C'est comme sur la vraie N64, où le RSP/RDP travaille en parallèle du CPU.
- La fin d'une tâche n'est signalée au jeu qu'une fois le dessin fini, comme sur la console.
- Quand le jeu « suspend » la 3D pour calculer le son (yield), le dessin continue en fond et le son est calculé en même temps.
- Sur PC, en temps réel, le même nombre d'images est dessiné qu'avec le dessin sur place, et les images sont correctes.

### À savoir
- **Rien de tout ça n'a encore tourné sur une vraie PS3**. Les deux nouveautés ont été testées sur PC uniquement, avec un interpréteur PowerPC pour le recompilateur.
- En cas de plantage ou de bug d'image, couper **RECOMPILATEUR** puis **DESSIN 3D EN PARALLÈLE** dans les options. Ces choix sont gardés.
- Pas encore corrigé : le triangle rose dans Bob-omb et les traînées de couleur dans le hall du château.

## Version 5.6 : le RSX fonctionne, place au CPU

Mesures sur PS3 (5.5), écran titre : ÉCRAN tombe de 41-59 % à 9 %. L'agrandissement par le RSX marche. Le temps restant se partage entre CPU 58 % et 3D 29 %. Corrections :
- **Cache de textures à 32 entrées**, reconnues par leur contenu. La tête de Mario alterne plusieurs textures dans la même tuile : avant, chacune était redécodée à chaque triangle. Gain d'environ 25 % sur l'écran titre (PC).
- **Accès mémoire du CPU** : chemin direct pour KSEG0/KSEG1, sans passer par la traduction d'adresse.
- **Arrondi FPU** (CVT.W, ROUND) sans appel à la bibliothèque mathématique. L'écran titre en fait des millions.

## Version 5.5 : écran noir de la 5.4 corrigé

La 5.4 affichait un écran noir dès le démarrage : la copie de l'interface par le RSX (`rsxSetTransferData`) ne fonctionnait pas. Changements :
- **Menus** : retour à la copie par le PPU, la méthode éprouvée.
- **Écran de jeu** : seul l'agrandissement de l'image passe par le RSX.
  - L'interface (bords, texte) n'est recopiée par le PPU que lorsqu'elle change.
  - Le panneau d'infos est copié par petits morceaux.
- **Sécurité** : si le RSX ne répond pas dans les 300 ms, l'émulateur repasse tout seul en affichage logiciel.

## Version 5.4 : l'affichage passe au RSX

Mesures sur PS3 (5.3) : jeu jouable à 26-41 %. La ligne COPIE indiquait 11 ms et DESSIN 27,5 ms par image : le PPU passait 40 à 60 % de son temps à afficher.
- **Agrandissement de l'image par le RSX**. L'image N64 (320×240) est donnée telle quelle au RSX, qui l'agrandit lui-même avec un lissage (`rsxSetTransferScaleSurface`). Avant, le PPU l'agrandissait pixel par pixel.
- **Copie de l'interface par le RSX** (`rsxSetTransferData`). Le canevas en mémoire principale est rendu visible au RSX par `gcmMapMainMemory`, au lieu des 3,7 Mo copiés par le PPU à chaque image.
- **Interface redessinée au plus 2 fois par seconde** en jeu. Le panneau d'infos devient opaque et le RSX le pose par-dessus l'image.
- **Option AFFICHAGE DU JEU : RSX / LOGICIEL**. LOGICIEL reprend l'ancienne méthode, si l'image RSX est fausse sur une console.

## Version 5.3 : deuxième essai sur PS3

Mesures sur PS3 (5.2) : titre 18 %, menu 27 %, écran blanc 43 %. La ligne TEMPS montrait 43 à 57 % du temps pour l'écran.
- **Écran blanc bloqué après le choix du fichier** (bug) : le saut d'images comptait les trames vidéo. Or Mario 64 n'envoie une image qu'une trame sur deux, toujours sur une trame impaire. Avec « 1 image sur 4 », la règle ne retombait jamais sur une trame impaire : plus rien n'était dessiné et l'écran restait blanc. Le saut compte maintenant les images envoyées par le jeu.
- **Affichage** : l'écran PS3 n'est plus redessiné ni recopié quand rien n'a changé. Seuls les bords sont effacés (au lieu des 3,7 Mo de l'écran entier).
- **Nouvelle ligne dans les infos** : temps de copie vers le RSX, d'attente de l'image et de dessin de l'interface.

## Version 5.2 : après le premier essai sur PS3

Premier essai de Mario 64 sur PS3 : écran titre à 8 %, menus à 22 % (avec 1 image sur 4), son haché. Corrections :
- **Boucle principale** : l'émulateur tourne jusqu'à la prochaine image réellement dessinée, avec 120 ms au plus. Avant, chaque image affichée était limitée à 13 ms d'émulation, et l'écran PS3 était redessiné même pour les images sautées.
- **Son** : le port audio est alimenté pendant l'émulation, plus seulement entre deux images.
- **Moins de conversions flottant ↔ entier** dans le rasteriseur (très coûteuses sur le PPU) : ombrage en point fixe, coordonnées de texture en une seule conversion.
- **Nouvelle option QUALITÉ 3D** : NORMALE, RAPIDE (textures non lissées), TRÈS RAPIDE (sans perspective).
- **Profilage à l'écran** : ligne « TEMPS CPU / 3D / SON / ÉCRAN » dans SELECT → INFOS, pour savoir où va le temps sur PS3.

## Version 5.1 : premier test avec Super Mario 64 (sur PC)

La ROM de l'utilisateur a été lancée sur PC avec le même cœur que la version PS3 (`make romrun`). Résultats :
- Écran titre, tête de Mario, choix du fichier, lettre de Peach, arrivée de Lakitu, puis **Mario jouable** devant le château.
- Musique produite, avec 0 commande graphique ou audio inconnue.

**4 corrections trouvées grâce à ce test** :
1. **Manette** (bug bloquant) : libultra n'écrit les commandes du PIF qu'une fois puis se contente de relire. L'émulateur ne les rejouait pas, donc START et A n'étaient jamais vus. Les commandes sont maintenant rejouées à chaque lecture.
2. **Lignes de points bleus sur le sol** : les grands triangles coupés près de la caméra avaient des coordonnées énormes. Il y a maintenant un découpage à 3 écrans (bande de garde) et un test d'appartenance exact (entiers, 1/16 de pixel) : plus aucun trou entre deux triangles.
3. **Cache des textures** : chaque tuile est décodée une fois en RGBA, au lieu d'un décodage à chaque pixel.
4. **Paramètres de texture et attributs préparés une fois par triangle** : environ 1,4 × plus rapide en jeu.

Vitesse sur le PC de test : environ 120 images/s dans le jardin du château (2 × le temps réel), avec 77 % d'attente sautée. La PS3 sera nettement plus lente ; le saut d'images automatique compense.

## Étape 5 : la vitesse

Un vrai recompilateur MIPS → PowerPC demande, sur PS3, de pouvoir exécuter du code fabriqué à la volée. Ce n'est possible qu'avec des bidouilles CFW (PS3MAPI), impossibles à tester ici. Cette étape suit donc une **voie sûre**, entièrement testée sur PC :

- **Saut des boucles d'attente** (option, activé par défaut). Une boucle courte qui ne fait que relire la mémoire ou un registre en attendant un événement est détectée par analyse du code : aucune écriture, et chaque registre lu est invariant ou recalculé dans l'itération. Le temps est alors avancé directement jusqu'au prochain événement (ligne vidéo suivante), Count/Compare compris.
  - Démo : 1 562 483 → 1 269 instructions par image.
  - Les jeux passent souvent une grande part de leur temps à attendre l'image suivante.
- **2 cycles par instruction** (option 1/2/3, 2 par défaut comme dans les émulateurs courants). Il faut moitié moins d'instructions émulées par seconde de jeu.
- **Saut d'images automatique** (option AUTO / aucun / 1 sur 2 à 1 sur 4). Si le jeu ralentit, seule une image sur 2, 3 ou 4 est dessinée, mais la logique du jeu et le son gardent leur vitesse. L'image précédente reste affichée entre deux images dessinées.
- **RDP environ 2,5 × plus rapide** :
  - chaque ligne de triangle ne parcourt que sa partie intérieure ;
  - le combiner est préparé une fois par triangle au lieu d'à chaque pixel ;
  - la texture 1 n'est lue que si le mélange l'utilise.
- Le CPU lit ses instructions directement en RDRAM sans appel de fonction.
- Écran d'infos : pourcentage du temps d'attente sauté, et « 1 IMAGE SUR n » quand le saut d'images est actif.
- `make bench` : mesures de vitesse sur PC.

**Limites honnêtes** :
- Sans recompilateur, le processeur reste interprété. Les gains réels dépendent de la façon dont chaque jeu attend.
- Mario 64 attend beaucoup, Zelda (plus gourmand) moins.
- Le rendu reste logiciel : le SDK PSL1GHT ne permet pas encore d'écrire les shaders de fragments du RSX.
- Si un jeu se comporte bizarrement : remettre CYCLES PAR INSTRUCTION à 1, puis couper SAUT DES BOUCLES D'ATTENTE.

## Étape 4 : son, sauvegardes, états

- **Microcode audio émulé en haut niveau (HLE)**. La variante est reconnue automatiquement :
  - **ABI 1** : Super Mario 64 et beaucoup de jeux de 1996-97 ;
  - **NEAD** : Mario Kart 64 et Zelda Ocarina of Time (et Majora's Mask).

  Il gère le décodage ADPCM (4 et 2 bits, boucles), le rééchantillonnage, les enveloppes de volume (exponentielles en ABI 1, linéaires en NEAD), le mixage sec/effet, les filtres (POLEF, FILTER), l'entrelacement stéréo, ainsi que la copie, la duplication et le gain des tampons.
- **Interface AI** : file de 2 tampons comme la vraie console. Le son passe de la fréquence du jeu (22 à 44 kHz) à **48 kHz stéréo** sur la PS3.
- **Sauvegardes de cartouche** dans `USRDIR/saves/` :
  - `<rom>.eep` pour l'EEPROM 4K/16K (Mario 64, Mario Kart 64) ;
  - `<rom>.sra` pour la SRAM 256 kbit (Zelda Ocarina of Time).

  Elles sont écrites au plus une fois par seconde quand le jeu sauvegarde, ainsi qu'au reset et en quittant.
- **États de sauvegarde** dans `USRDIR/states/` : **10 emplacements par jeu**. Menu SELECT :
  - SAUVER L'ÉTAT, CHARGER L'ÉTAT ;
  - EMPLACEMENT (◀ ▶) ;
  - SON OUI/NON.
- **Démo maison** : en plus du cube, maintenir **B** joue une note (~360 Hz). Elle passe par une vraie liste audio ABI 1 puis par le DMA AI, ce qui vérifie toute la chaîne du son.
- Infos à l'écran : nom du microcode audio reconnu à côté de TÂCHES SON.

**Limites honnêtes** :
- La table du rééchantillonneur est une approximation de celle du microcode : le son peut être très légèrement différent.
- Pas encore de FlashRAM (Majora's Mask, Paper Mario…) ni de Controller Pak (fantômes de Mario Kart). Les ABI audio « 3 » (MusyX, jeux Rare) ne sont pas gérées : ces jeux restent muets.
- Le son suit la vitesse de l'émulation. Tant que le jeu tourne sous 100 % (avant l'étape 5), il sera haché.
- Un état n'est valable que pour la même version de l'émulateur et la même ROM. Il fait environ 5 Mo (9 Mo avec l'Expansion Pak).
- Testé sur PC uniquement : listes audio construites à la main, plus la démo complète.

## Étape 3 : les graphismes 3D

- **Microcode graphique émulé en haut niveau (HLE)**. Le type est reconnu automatiquement dans les données du microcode :
  - **F3D** (Super Mario 64) ;
  - **F3DEX** (Mario Kart 64) ;
  - **F3DEX2 / F3DZEX** (Ocarina of Time).

  L'émulateur gère :
  - matrices (pile, projection) et viewport ;
  - sommets, éclairage directionnel et ambiant ;
  - placage d'environnement, brouillard ;
  - triangles, TRI2 et quads ;
  - élimination des faces avant et arrière, découpage au plan proche ;
  - sous-listes, segments, modes de géométrie, « othermode » ;
  - rectangles texturés.
- **RDP logiciel** : il dessine directement dans l'image en RDRAM, comme le vrai, puis le VI l'affiche.
  - TMEM 4 Ko : LOADBLOCK, LOADTILE et LOADTLUT, avec l'entrelacement des lignes impaires.
  - Formats de texture RGBA16/32, CI4/8 (palettes RGBA16 ou IA16), IA4/8/16 et I4/8.
  - Répétition, miroir et bornage des textures ; filtrage bilinéaire ; correction de perspective.
  - **Combiner** 1 et 2 cycles : (A−B)×C+D sur la couleur et l'alpha.
  - Tampon Z, transparence, test alpha, brouillard.
  - Modes FILL et COPY, rectangles de remplissage, ciseaux.
  - Listes RDP envoyées directement par les registres DP.
- **Correction importante** : les registres du RSP (0x04040000) étaient mal décodés à l'étape 2. Les jeux ne pouvaient donc pas démarrer de tâche (TÂCHES GFX restait à 0). C'est corrigé.
- **Démo maison** : un cube 3D texturé tourne, dessiné par une vraie liste d'affichage F3DEX2 avec double tampon et tampon Z.
  - Stick ou croix : vitesse de rotation.
  - A : inverse le sens.
- **Infos à l'écran** (SELECT → INFOS) : microcode reconnu, triangles reçus et éliminés, commandes inconnues.

**Limites honnêtes** :
- Tout est calculé par le processeur de la PS3 (PPU), sans le RSX. Les jeux seront donc **très lents** jusqu'à l'étape 5 (recompilateur et rendu RSX).
- Le mélangeur de couleurs est simplifié : opaque, transparent ou brouillard.
- Pas d'anticrénelage (couverture des pixels) ni de niveaux de détail (mipmaps).
- Les triangles RDP « bruts » (rares) sont ignorés.
- Microcodes spéciaux non gérés : S2DEX, utilisé pour les fonds 2D de certains jeux.
- Rien n'a encore été vérifié sur une vraie PS3 ni avec les vrais jeux. Les tests passent sur PC avec des listes construites à la main et la démo.

## Étape 2 : le processeur et la machine

- **Processeur VR4300** (interpréteur) :
  - jeu d'instructions MIPS III entier en 64 bits ;
  - créneaux de retard, branchements « likely », accès non alignés (LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR) ;
  - multiplications et divisions 32/64 bits (comportement N64 en cas de division par zéro) ;
  - **COP0** : exceptions, interruptions, Count/Compare, ERET ;
  - **TLB** 32 entrées ;
  - **FPU COP1** : simple et double précision, conversions, modes d'arrondi, comparaisons, BC1.
- **Machine** :
  - RDRAM 4 ou 8 Mo (Expansion Pak), MI (interruptions) ;
  - **VI** : image 16 et 32 bits affichée à l'écran ;
  - **PI** : DMA cartouche → RDRAM ;
  - **SI + PIF** : manettes et EEPROM 4K/16K ;
  - **AI** : minutage des DMA du son ;
  - **RSP** : tâches graphiques et audio reconnues, rendu à l'étape 3.
- **Démarrage sans BIOS** (HLE) : copie IPL3 de 1 Mo, adresse de démarrage corrigée pour 6103/6106, variables système (osTvType, osMemSize…), état des registres à la sortie du PIF.
- **Écran d'exécution** : image VI, vitesse en %, et pour les jeux qui n'affichent pas encore d'image, des compteurs de diagnostic (tâches graphiques et son reçues, interruptions, DMA, exceptions, PC). SELECT ouvre le menu : reprendre, reset, infos, manette, bibliothèque.

## Étape 1 : interface et cartouches

- **Bibliothèque** : lit les ROMs `.z64`, `.v64` et `.n64` et les convertit automatiquement en big-endian. Les fichiers qui ne sont pas des ROMs N64 sont ignorés.
- **Analyse de l'en-tête** : titre interne, code du jeu (ex. `NSME`), région, version, adresse de démarrage, version de libultra.
- **Puce CIC** identifiée par le CRC32 de l'IPL3 (6101, 6102, 6103, 6105, 6106, 7102, 5101).
- **Somme de contrôle IPL3** recalculée sur 1 Mo et comparée à celle de l'en-tête, avec les variantes propres à 6103, 6105 et 6106. Elle signale les ROMs modifiées.
- **Base de jeux** : nom, type de sauvegarde (EEPROM 4K/16K, SRAM, FlashRAM), nombre de joueurs, Expansion Pak nécessaire.
- **Recherche** (titre, code, région, nom de fichier), filtres (tous, favoris, récents, USA, Europe, Japon), tris.
- **Démarrage diagnostique** : chargement complet de la ROM en mémoire avec barre de progression, conversion, empreinte CRC32, puis liste de contrôle (CIC, somme, adresse de démarrage, sauvegarde, région 50/60 Hz, mémoire 4/8 Mo).
- **Manette N64 virtuelle** :
  - test en direct ;
  - zone morte radiale, amplitude du stick (±80 comme la vraie manette) ;
  - Z sur L2 ou R2, boutons C sur le stick droit (et en option sur △/○).

## Feuille de route

1. ✅ Interface, ROMs, manette
2. ✅ Processeur VR4300 (interpréteur), mémoire RDRAM, PI/SI/VI/AI/MI, démarrage HLE (sans BIOS)
3. ✅ Graphismes : microcodes F3D / F3DEX / F3DEX2, RDP logiciel
4. ✅ Son (ABI 1 / NEAD), sauvegardes EEPROM/SRAM, états
5. ✅ Vitesse : saut des attentes, 2 cycles/instruction, saut d'images, RDP optimisé (recompilateur impossible sans PS3MAPI)

## Commandes (bibliothèque)

✕ démarrer · △ rechercher · □ favori · L1/R1 filtres · SELECT tri · START options

## ROMs

Placez-les dans l'un de ces dossiers :

- `/dev_usb000/n64/` (ou usb001 à usb006)
- `/dev_hdd0/n64/`, `/dev_hdd0/ROMS/n64/`
- `/dev_hdd0/game/NSIX00001/USRDIR/roms/`

## Compiler

**GitHub** : envoyez le dossier dans un dépôt, y compris `.github`. L'onglet **Actions** lance les tests, compile, puis publie **n64-HEN-CFW**. Installez le fichier `n64.gnpdrm.pkg` qu'il contient.

**Sur PC** :

```sh
make soak   # 17 tests ROMs + 35 processeur + 35 graphismes + 18 son + 27 démo + 313 recompilateur + encodage PowerPC
make bench  # mesures de vitesse
make romrun ROM="mario.z64" FRAMES=3000 SCRIPT="700:START,705:-,1000:SHOT"   # une vraie ROM, captures dans shots/
JIT=1 THREAD=1 PACE=16683 ./build_host/romrun mario.z64 3000 ""   # recompilateur (code PowerPC interprété), 2e fil, temps réel
python3 tools/make_demo.py   # régénère la démo (source/demo_rom.c)
make host   # session simulée + captures
```

La somme de contrôle a été vérifiée contre l'algorithme de référence (`n64crc.c`, snesrc) sur une ROM synthétique.
