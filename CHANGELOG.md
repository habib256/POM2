# POM2 — Changelog

Historique des changements notables, organisé du plus récent au plus
ancien. Le `git log` reste la source canonique pour la mécanique
exacte ; ce fichier capture les **« pourquoi »** et les pièges qu'on
ne veut pas re-découvrir. Backlog actif → `TODO.md`. Implémentation
courante → `DEV.md`.

## 2026-05-25

- **//c / //c+ : banner corrompu au reboot avec plusieurs périphériques —
  correctif**. Régression du SmartPort embarqué : exposer mon stub bloc à
  `$C500` (à la place du **vrai** firmware SmartPort interne du //c) cassait
  l'autostart du ROM //c dès qu'un disque Disk II (slot 6) ET un média
  SmartPort (slot 5) coexistaient — un ProDOS booté depuis le Disk II (ex.
  le navigateur BITSY) énumérait le slot 5, appelait des points d'entrée
  `$C5xx` que le vrai firmware fournit mais pas mon stub → exécution erronée
  → la page texte se remplissait de fragments de code du stub (« fE1DMS@HP »
  au lieu de « Apple //c », `finalPC=$FD23` moniteur). Trace headless :
  `tests/iic_dual_boot_trace.cpp`.
  * **Fix** : flag « armé » (`Memory::setIicSmartPortArmed`). Le stub `$C500`
    n'est exposé à travers le masque INTCXROM **que** lorsque
    `EmulationController::bootFromSlot` l'arme (boot explicite GUI/CLI) ;
    **tout reset/cold-boot le désarme**. Donc l'autostart du //c voit
    toujours son **vrai** firmware → reboot propre (boot Disk II / banner),
    et le SmartPort se boote via le bouton « Boot » de la Disk Library / Slot
    Configuration. Conséquence : un média SmartPort persisté ne se réamorce
    pas tout seul au reboot (utiliser « Boot »).
  * Corrections de fond du stub au passage : `$Cn07` `$3C`→`$01` ($3C = la
    signature **Disk II**, qui faisait passer le slot 5 pour un 2ᵉ Disk II)
    et **STATUS ProDOS** complet (renvoie le nombre de blocs en X/Y via
    `$C0n5/$C0n6`) — un STATUS incomplet corrompait un scanner de volumes.
  Vérifié à l'écran (//c) : reboot Disk II+SmartPort propre (BITSY), boot
  explicite 3.5" OK (JEUX). 89 tests passent. Voir `project_iic_smartport_boot`.

- **[UI] Fusion Slot Manager → Slot Configuration**. Le panneau autonome
  *Slot Manager* est **supprimé** (`SlotManager_ImGui.{h,cpp}` retirés) ;
  sa fonctionnalité de baies montables est repliée dans une **2ᵉ colonne**
  de *Slot Configuration* (Machine → Slot Configuration). À gauche :
  assignation carte par slot, avec **tout l'intégré grisé/verrouillé**
  (AUX 80-col + built-ins du profil — sur //c/+/c+ : SSC, Mouse, SmartPort,
  Disk II). À droite : **disques internes + ports montables** construits par
  un parcours **live** du SlotBus — baies `MountableMediaCard` (SmartPort
  2 unités avec type-select, CFFA + HDV 1 baie) **et** lecteurs `DiskIICard`
  (5.25" internes), chacun avec montage/éjection/write-back/**Boot slot**.
  `persistMediaBay` (ex-lambda du Slot Manager) promu membre. Visibilité du
  panneau désormais persistée (`show_slot_config`). Vérifié à l'écran en //c.
  89 tests passent.

- **//c / //c+ : boot 3.5" ET HDV via SmartPort embarqué**. Symptôme : en
  profil //c, les images 3.5" et HDV ne bootaient pas depuis la Disk Library
  (le 5.25" oui). Diagnostic en cascade :
  * Les ROM de slot sont **masquées sur tout //c-class** (`IIcClassProfile::
    forcesIntCxRom()` toujours `true`) → une carte slot (`ProDOSHardDiskCard`,
    `SmartPortCard`, `CffaCard`) n'est **jamais bootable** (`$Cn00` lit la ROM
    interne, pas la carte). Auto-brancher une carte slot (l'astuce kiosk
    `ensureHdvCardForBoot`) est donc inopérant en //c-class.
  * **MAME ne modélise pas** le 3.5"/SmartPort sur le //c simple : sa carte
    `A2BUS_IWM` (`bus/a2bus/a2iwm.cpp`, machines apple2c0/c3/c4) n'a que **deux
    lecteurs 5.25"** ("WANTED: there are no ROM dumps"). Seul le //c+ a un 3.5"
    (IWM+MIG+Sony porté en `SmartPortHub`), mais son **boot 3.5" n'a jamais
    marché** (état documenté : "banner + 5.25" only" ; vérifié → "UNABLE TO
    FIND A BOOTABLE DISK ONLINE"). Donc aucun //c-class ne bootait 3.5"/HDV.
  * **Solution** (pas un portage MAME — il n'y a rien de fidèle à porter pour
    le //c simple) : un **SmartPort servi par l'hôte** au slot 5 intégré. Le
    vrai //c y avait son firmware SmartPort ; POM2 substitue un **stub bloc**
    (la `SmartPortCard` déjà prouvée sur //e). `Memory::memRead` perce un trou
    unique à `$C500-$C5FF` (bank 0) à travers le masque INTCXROM **ssi** la
    carte slot-5 a un média (`SlotPeripheral::exposesIicOnboardRom` — garde
    contre un boot autostart d'une carte vide → JMP $0801 sur du vide). Le
    device-select (`$C0D0-$C0DF`) n'étant jamais masqué, le protocole bloc
    `$C0D0-$C0D4` atteignait déjà le bus. Câblage : `routeMount35` utilise la
    carte SmartPort sur **tous** les profils (plus seulement non-//c+) ;
    `routeMountHdv`/`ensureHdvCardForBoot` routent le HDV //c-class vers le
    SmartPort slot 5 (jamais une cffa/hdv slot — masquée). Profil //c : ajout
    de `smartport35` en slot 5 intégré (le //c+ l'avait déjà). `bootFromSlot(5)`
    boote. Vérifié à l'écran : //c & //c+ × 3.5"+HDV (JEUX, Total Replay, ARCHON).
    Pin : `tests/iic_onboard_smartport_test.cpp` (+ `system_profile` mis à jour).
    88 tests passent. Détails : `project_iic_smartport_boot`.

- **Bug hunt round 10 — correctifs (CPU 65C02 / snapshot / Settings / Le Chat
  Mauve)**. Audit des sous-systèmes restants : couche HTTP AiControlServer,
  cœur CPU M6502, décode soft-switch / Language-Card / RamWorks de Memory,
  sérialisation de snapshot, Le Chat Mauve + Settings. **AiControlServer**
  (testé ASan — pas de crash/OOB distant) et **paging Memory** (LC + RamWorks)
  ressortent **propres**. 90 tests passent (nouveau pin : `settings_roundtrip`).
  * **[CPU] flag V de `SBC` décimal sur 65C02 faux** (`M6502.cpp` `SBC`) —
    l'overflow était calculé depuis l'accumulateur **ajusté BCD** au lieu de la
    différence binaire ; diverge du matériel (MAME `w65c02`) sur **2400**
    entrées. Recalculé depuis `(uint8_t)tmp` (binaire) → identique au matériel
    sur les 131072 entrées. NMOS inchangé. Pin : `cmos_6502_smoke`.
  * **[CPU] `(zp,X)` sous-compté d'1 cycle** (`M6502::IndZeroX`) — les 8 ops
    `(zp,X)` (LDA/STA/ADC/AND/CMP/EOR/ORA/SBC) prenaient 5 cycles au lieu de
    **6** (lecture-bidon du pointeur zero-page non indexé manquante). +1.
    Pin : `cpu_cycle_count`.
  * **[CPU] `ASL/LSR/ROL/ROR abs,X` sur 65C02 sur-comptés** (`M6502.cpp`) —
    facturés 7 cycles fixes ; le 65C02 prend **6** (7 seulement si page
    franchie). Nouvelle adresse `RmwAbsX` ; `INC/DEC abs,X` restent 7 sur les
    deux CPU. Pin : `cpu_cycle_count`.
  * **[Snapshot] section CPU : garde de longueur trop laxiste** (`AiControlServer.cpp`
    chargement) — la garde était `len >= 9` mais le lecteur consomme **16**
    octets → un snapshot forgé/tronqué (9–15 o) lisait jusqu'à 7 octets au-delà
    de la section (compteur de cycles / mode CPU corrompus, silencieux ;
    atteignable via l'API localhost). Garde `len >= 16` + docstring corrigée
    (les lignes IRQ/NMI ne sont pas persistées — signaux transitoires). Pin :
    `ai_control_server_smoke` (section CPU courte → ignorée).
  * **[Settings] valeur contenant `#` tronquée au rechargement** (`Settings.cpp`)
    — `load` coupait après le premier `#` n'importe où, mais `save` écrivait
    brut : un chemin `/home/u/My#Disks/jeu.dsk` rechargeait en `/home/u/My`
    (montage cassé silencieux). `#` n'est désormais un commentaire qu'en début
    de ligne. Pin : `settings_roundtrip`.
  * **[Settings] valeur contenant un saut de ligne corrompait le fichier** —
    échappement `\\`/`\n`/`\r` à l'écriture, dés-échappement à la lecture →
    round-trip sans perte pour toute chaîne. Pin : `settings_roundtrip`.
  * **[Settings] écriture atomique défaite à l'échec de flush** (`save`) — le
    stream était testé AVANT `close()` ; une erreur différée (disque plein)
    renommait alors le `.tmp` tronqué par-dessus la bonne config. `flush()`+
    `close()`+vérification avant le `rename`.
  * **[Le Chat Mauve] décalage FIFO parasite au reset** (`LeChatMauveCard`) —
    `an3Prev` initialisé à `false` ; un `$C05F` isolé après reset était vu
    comme front montant et clockait la FIFO. AN3 démarre HAUT (DHIRES off,
    comme MAME `device_reset` `m_dhires=false`) → `an3Prev = true`. Pin :
    `le_chat_mauve_smoke`.
  * **Vérifié propre** : AiControlServer (parsing HTTP/JSON, bornes mem
    peek/poke, cycle de vie sockets, threading — testé ASan), décode
    soft-switch Memory (machine LC write-enable, combinaisons paging IIe,
    banques RamWorks, lectures de statut).

- **Bug hunt round 9 — correctifs (Disk II / profils / threading)**. Audit des
  sous-systèmes restants : contrôleur Disk II + LSS, parsers d'images disque,
  bus de slots + agrégation IRQ, `EmulationController` (horloge/threads),
  machinerie de profils (`applyProfile`). **Parsers `DiskImage`** (WOZ/DSK/NIB/
  2MG, robustesse sur fichiers forgés/tronqués) et **`SlotBus`/`SlotPeripheral`**
  (bornes de slot, wire-OR IRQ, cycle de vie) ressortent **propres**. 89 tests
  passent (nouveau pin : `disk_wp` ; `diskii_lss` étendu).
  * **[Disk II] écriture d'une disquette protégée en écriture** (`DiskIICard.cpp`
    sites d'écriture + `DiskImage` `writeFlux`/`writeNibbleAt`/`saveDirty`) — les
    sites ne testaient que `writeBackEnabled`, pas la **WP physique du médium**
    (WOZ INFO+2 / flag 2IMG). `isWriteProtected() = fileWriteProtected ||
    !writeBackEnabled` reportait bien la WP au logiciel, mais un logiciel qui
    l'ignore et écrit quand même corrompait le buffer puis **écrasait le fichier
    source** à l'éjection. Sur vrai matériel la WP **inhibe le courant
    d'écriture** : `writeFlux`/`writeNibbleAt`/`saveDirty` honorent désormais
    `fileWriteProtected` (nouveau getter `isFileWriteProtected()`). Pin :
    `disk_wp` (le médium WP n'est muté ni en RAM ni sur disque).
  * **[Disk II] spin-down bloqué quand le lecteur sélectionné est vide**
    (`DiskIICard::advanceCycles`) — le `return` anticipé sur `!isLoaded` était
    AVANT le décompte du délai d'arrêt moteur, donc un motor-off sur un lecteur
    vide laissait `motorOn`/`active` coincés indéfiniment (LED + son). Le chemin
    données reste gardé par la présence du média, mais le spin-down tourne
    désormais quoi qu'il arrive. Pin : `diskii_lss` (cas spin-down sans disque).
  * **[Disk II] getters par-lecteur non bornés** (`DiskIICard.h`) — index `drive`
    non validé → accès hors bornes des tableaux `images[]`/`headQuarterTrack[]`/
    `trackPos[]`. Bornés (défaut sûr hors plage), cohérent avec insert/eject.
    Pin : `diskii_lss` (assertions OOB).
  * **[Profils] `applyProfile` appliquait les slots intégrés du MAUVAIS profil**
    (`MainWindow_Slots.cpp`) — `plugSlotsFromSettings()` (étape 7) lit
    `activeProfile` pour forcer les cartes built-in, mais `activeProfile = p`
    n'était posé qu'à l'étape 12. Switch VERS //c/c+ → cartes embarquées (SSC/
    souris/SmartPort/Disk II) non forcées (pas de contrôleur de boot, y compris
    au démarrage où le ctor appelle `applyProfile(saved)`) ; switch DEPUIS //c/c+
    → fuite des built-in dans un II+/IIe propre. `activeProfile = p` remonté en
    étape 0.
  * **[Threading] lecture inter-threads non verrouillée des registres CPU**
    (`MainWindow.cpp`, panneau Emulation) — PC/A/X/Y/SP + compteur de cycles lus
    sans verrou pendant que le worker les écrit sous `stateMtx` (data race / UB ;
    tous les autres lecteurs prennent le verrou). Snapshot sous `stateMutex()`.
  * **[Profils] RamWorks III désactivé sur le //e Unenhanced** — la grille ne
    testait que `AppleIIe` (Enhanced) ; le //e 1983 a le même bus aux. Étendu aux
    deux variantes //e.
  * **[Profils] RAM auxiliaire non effacée au cold-switch VERS un profil IIe** —
    `clearRam()` n'efface l'aux que si `iieMode` est déjà vrai, mais tournait
    AVANT `setIIEMode`. Réordonné : `setIIEMode` → `clearRam` → `resetSoftSwitches`
    (et `setIIEMode` reste avant `loadAppleIIRom`).
  * **[Threading] `stop()` ne bloque pas jusqu'à l'arrêt du worker** (latent) —
    documenté : les appelants exigeant l'exclusivité après `stop()` doivent
    prendre `stateMutex()` (c'est le verrou, pas `stop()`, qui sérialise).
  * **Note** : les correctifs profils/threading vivent dans `MainWindow*` (classe
    GUI sans harnais de test unitaire) — validés par compilation + suite verte +
    vérification structurelle, comme les autres changements MainWindow du repo.

- **Bug hunt round 8 — correctifs (SmartPort / sécurité ProDOS / threading
  audio / ATA)**. Audit des sous-systèmes encore vierges : pile bloc SmartPort,
  backing bloc partagé + ATA/HDV, chemin audio (Speaker/Cassette), entrées
  (joystick/paddles/souris), moteur d'affichage. **Entrées** et **affichage**
  ressortent **propres** (seuls des commentaires périmés). 87 tests passent
  (3 nouvelles cibles : `smartport_io_error`, `prodos_decode_safety`,
  `speaker_overflow`).
  * **[Sécurité — ProDOS synth] traversée de chemin au write-back**
    (`ProDOSVolume.cpp` `decodeVolumeToFolder`) — le nom de chaque entrée de
    répertoire était lu depuis l'image **inscriptible par l'invité** et joint
    au dossier hôte sans validation : un nom forgé `../PWNED` s'échappait du
    bac à sable quand le write-back est actif (même classe que les évasions
    symlink/`goUp` corrigées R4/R6). Nouveau garde `isHostSafeProDOSName`
    (rejette séparateurs, NUL, `.`/`..`, hors `[A-Za-z0-9.]`) appliqué aux
    deux sites de jonction (fichier + sous-répertoire). Pin :
    `prodos_decode_safety`.
  * **[SmartPort] READBLOCK/WRITEBLOCK silencieusement « réussis »**
    (`SmartPortCard.cpp`) — un `readBlock`/`writeBlock` hors-plage renvoyait
    `CLC` (succès) à ProDOS : lecture → buffer de `0xFF` + carry clair, et
    désync du flux d'octets ; écriture → données perdues sans erreur. La carte
    verrouille désormais un bit d'erreur I/O ($C0n4 bit 0) ; les routines ROM
    lecture/écriture le testent et renvoient carry-set ($27). Le flux reste en
    phase (cache 0xFF). L'allongement de la routine de lecture (34→45 o) a
    décalé le bloc d'écriture → opérande `BEQ write` recalculé `$2E`→`$39`
    (revalidé par `smartport_write_dispatch`). Pin : `smartport_io_error`.
  * **[SmartPort] sélection d'unité par masque, buffer d'écriture non purgé**
    (`SmartPortCard.cpp`) — `$C0n0` utilisait `& (kMaxUnits-1)` (faux si
    kMaxUnits non puissance de 2) → `% kMaxUnits` + `static_assert` ; et ne
    purgeait pas `writeBufPrimed_`/`readCacheValid_` → un write à moitié
    streamé pouvait commiter des octets périmés. Les deux corrigés.
  * **[Audio — Cassette] data race sur les drapeaux de mode**
    (`CassetteDevice`) — `audioStreamMode` (lu sans verrou sur le thread audio,
    muté sur le thread UI) et le compteur de rampe (touché sous deux mutex
    différents) rendus `std::atomic` (comme `playbackPaused`/`muted`). Pin :
    `static_assert` de discipline threading dans le constructeur (revertir vers
    un type nu casse la compilation).
  * **[Stockage — ATA] écriture sur médium write-protected « réussie »**
    (`AtaBlockDevice.cpp`) — une commande WRITE accordait DRQ puis jetait
    silencieusement les données (`flushBufferToSector`) sur un backing WP, sans
    `ERR`. ATA-1 : abort avec `ERR`+`DF` en statut et `ABRT` dans le registre
    d'erreur, pas de DRQ. Atteignable via une image 2IMG WP sous CFFA. Pin :
    `ata_block_device` (cas WP).
  * **[Stockage] plafond de blocs off-by-one** (`Block512Backing.h`) —
    `kMaxBlocks` acceptait 65536 blocs alors que les numéros de bloc ProDOS
    sont 16 bits (max 65535, et `selectedBlock` est un `uint16_t`). Ramené à
    `0xFFFF` (+ `static_assert`). Pin : `hdv_mass_storage` (frontière 65535
    accepté / 65536 rejeté).
  * **[Audio — Speaker] perte de toggle à l'overflow inversait la parité**
    (`SpeakerDevice.cpp`) — le haut-parleur est une bascule 1 bit : chaque
    événement est un flip de parité. Jeter UN toggle à l'overflow inversait le
    niveau reconstruit de tous les échantillons suivants → on jette désormais
    par **paires**. Pin : `speaker_overflow`. + suppression de l'accesseur mort
    `getAudioCpuCursor()` (lecture 64-bit déchirée, zéro appelant).
  * **Vérifié propre** : entrées (timing RC paddle = MAME `apple2.cpp`,
    quadrature souris MAME-fidèle), moteur d'affichage (interleave HGR/texte,
    DHGR aux/main, lookup char-ROM 2K/4K), bornes lecture/écriture du backing
    bloc, dispatch/persistance/propriété SmartPort.

- **Bug hunt round 7 — correctifs (SnapshotIO / clavier-paste / char-ROM /
  désassembleur)**. Audit des sous-systèmes restants : I/O snapshot,
  pipeline clavier/coller, chargeur de ROM de caractères, désassembleur du
  Memory Viewer. Le cœur NMOS du désassembleur ressort **correct** ; seul le
  jeu 65C02 manquait. 84 tests passent (2 nouvelles cibles : `disasm_cmos`,
  `char_rom`).
  * **[SnapshotIO] longueur de section non bornée → crash** (`SnapshotIO.cpp`
    `nextSection`) — un `.snap` forgé (ou tronqué) déclarant une longueur de
    section gigantesque pilotait une allocation/`readBytes` dimensionnée par
    le fichier lui-même (`bad_alloc` → crash, ou over-read sur section
    tronquée). La taille du fichier est désormais relevée à l'ouverture et
    **chaque** section est rejetée si `sectionEnd > fileSize` (ou overflow).
    Une seule borne couvre crash + over-read pour tous les consommateurs.
    Atteignable via l'API AI-control (localhost). Défense en profondeur
    additionnelle : cap 16 Mio sur la section MEX (`AiControlServer.cpp`) +
    `try/catch` autour de `handleClient` dans le worker. Pin :
    `snapshot_io_smoke` (4 cas malformés : longueur géante, longueur>fichier,
    longueur tronquée, taille exacte acceptée).
  * **[Clavier] frappe live pendant un coller écrasait la FIFO**
    (`Memory::queueKey`) — une touche arrivant en plein coller faisait
    `lastKey=…` et clobberait l'octet de coller latché (saut de file). Elle
    est maintenant **appendée** derrière le coller si une file est en cours,
    et garde le comportement matériel « dernière touche gagne » sinon. Pin :
    `paste_smoke`.
  * **[Clavier] un reset n'abandonnait pas le coller en cours**
    (`Memory::resetSoftSwitches` IIe + `resetSoftSwitchesWarm` II/II+) — la
    `pasteQueue` survivait à F11/F12 et continuait à se vider après le reset.
    `pasteQueue.clear()` ajouté aux deux chemins. Pin : `paste_smoke`.
  * **[Clavier] cap du coller par-appel et non sur la file vivante**
    (`Memory::pasteText` + `pasteRawKeys`) — des collers répétés pouvaient
    faire croître `pasteQueue` sans borne (DoS mémoire via AI-control /
    presse-papiers). Le cap `kPasteMaxChars` est désormais calculé contre
    `pasteQueue.size() + (keyReady?1:0)`. Pin : `paste_smoke`.
  * **[Clavier] coller minuscules sur ][/][+** (`Memory::pasteText`) — le
    clavier II/II+ n'a pas de minuscules ; `a-z` → `A-Z` au coller quand
    `!iieMode` (un vrai clavier ne peut émettre `$61-$7A`), les claviers
    IIe-class gardent la casse. Pin : `paste_smoke`.
  * **[char-ROM] dump 8K accepté puis rendu en garbage** (`Memory::loadCharRom`)
    — seuls 2K (II/II+) et 4K (IIe) ont un chemin de normalisation ; toute
    autre taille était stockée brute et dessinée en glyphes corrompus. Le
    gate rejette désormais proprement tout ce qui n'est pas 2K/4K (8K, taille
    impaire, vide) et vide `characterRom` sur lecture courte. Pin :
    `char_rom`.
  * **[Désassembleur] jeu 65C02 décodé en `???` 1 octet** (`Disassembler6502`)
    — l'émulateur tourne en 65C02 par défaut mais le désassembleur était
    NMOS-only ; les opcodes CMOS sortaient en `???` de 1 octet, **désynchro-
    nisant** tout le listing Disasm (piège classique des BBR/BBS 3 octets
    traités comme 1). Ajout d'une table CMOS (TSB/TRB/STZ/BRA/PHX/PHY/PLX/PLY/
    INC A/DEC A/(zp)/JMP (abs,X)/RMB/SMB/BBR/BBS/WAI/STP…) sélectionnée par un
    flag `cmos` câblé depuis `MainWindow` via `MemoryViewer::setCmosMode`
    (poussé chaque frame selon `M6502::getCpuMode`). Pin : `disasm_cmos`
    (mnémonique + longueur, dont la longueur 3 octets des BBR/BBS, + repli
    NMOS → `???`).

- **Bug hunt round 6 — correctifs (Floppy Emu / SSC threading / ProDOS / son)**.
  Audit des sous-systèmes encore vierges (gadget BMOW Floppy Emu, ClockCard,
  pont TCP/telnet SSC, décode ProDOS, son disque). ClockCard ressort
  **propre** (seul un octet d'année superflu dans le shift, inoffensif pour
  ProDOS). 82 tests passent.
  * **[Floppy Emu] évasion du sandbox SD par symlink** (`FloppyEmuDevice.cpp`
    `listing`) — `directory_iterator`/`is_directory` suivent les liens ;
    skip des symlinks pour rester dans `floppyemu/`. Pin : `floppy_emu_smoke`.
  * **[Floppy Emu] `goUp` testait la LONGUEUR de chaîne** (pas un préfixe) —
    un frère plus long que la racine échappait ; check par composants.
  * **[Floppy Emu] taille d'un favori manquant = garbage** (`file_size` `ec`
    ignoré → `(uint64_t)-1`) → 0. Pin : `floppy_emu_smoke`.
  * **[Floppy Emu] curseur du browser non remis à 0 au changement de listing**
    (entrée/sortie de dossier, toggle Favoris, switch de mode) →
    surbrillance sur une entrée périmée (`FloppyEmu_ImGui.cpp`).
  * **[SSC] machine à états IAC telnet persistante** (`SuperSerialCard.cpp`
    `processTelnetRx`) remplace le filtre sans état : gère `IAC SB … IAC SE`
    (sous-négociation longueur variable — la rafale NAWS d'un client telnet
    fuyait ~5 octets en RX) ET une séquence IAC coupée entre deux `recv()`.
    Pin : `ssc_acia_smoke`.
  * **[SSC] data race sur la ligne IRQ** — le thread TCP appelait
    `setIrqLine`/`assertIrq` (RMW non-atomiques) en concurrence avec le
    thread CPU. `M6502::IRQ`/`irqSourceMask` rendus atomiques + l'IRQ du
    worker est *marshalée* sur le thread CPU via `SuperSerialCard::
    advanceCycles` (assertIrq devient CPU-thread-only).
  * **[SSC] race de cycle de vie des sockets** — le thread UI fermait
    `clientFd`/`listenFd` pendant que le worker était dans `recv`/`accept`
    (use-after-close / double-close). fds atomiques ; l'UI ne fait que
    `shutdown()` (réveil), le worker est seul à `close()`, `listenFd` fermé
    après `join()`.
  * **[ProDOS] round-trip de nom de fichier non idempotent** (décode) — un
    nom à extension conservée (ex. `NOTES.DATA`) accumulait `.bin` à chaque
    sauvegarde ; fichier sans extension → `.bin`. Fix : extensionless →
    type 0x00 (typeless) à l'encode, `0x00` → "" au décode, et pas d'ajout
    d'extension si le nom en a déjà une. Round-trip lossless pour les cas
    courants. (Chemin atteint via le write-back host-folder synth.)
  * **[ProDOS] trous de fichier sparse tronquaient** (décode, défensif) — une
    entrée d'index `0x0000` faisait `break` au lieu de zéro-remplir +
    continuer. Corrigé (non atteignable sur volumes synth — 0 bloc libre —
    mais correct si le décode est un jour étendu aux images réelles).
    Seedling `eof>512` → warn.
  * **[Son disque] `kSeekJoinMs == kSeekTimeoutMs == 100`** →
    chevauchement : un flux de pas à la cadence-join max pouvait déclencher
    le timeout entre deux pas → clic « atterrissage » parasite en plein
    seek ; + bande morte 50-100 ms (gap classé seek mais sans sample →
    clic). `kSeekJoinMs` abaissé à 50 (= plage de `pickSeekSample`), timeout
    100 > join → plus de chevauchement, plus de bande morte (résultat
    audible inchangé). (`FloppySoundDevice.h`)
  * **Reporté** : Floppy Emu `automount` (jamais câblé — *feature*, pas un
    bug de correction ; wire-up délicat à l'ordre de boot) ; garde anti-cycle
    de répertoire ProDOS (le plafond de profondeur 16 empêche déjà le hang ;
    défensif sur image corrompue, impossible sur volume synth) ; fichiers
    *tree* ProDOS (>128 Ko) déjà skippés proprement.

- **Bug hunt round 5 — 9 correctifs (affichage / persistance / cassette)**.
  Audit des dernières zones non couvertes (pipeline vidéo, parseurs de
  formats disque non-WOZ2, orchestration boot/slot/persistance MainWindow,
  cassette + joystick). Les parseurs disque sont ressortis **propres** (aucun
  over-read sur taille/offset fournis par le fichier). 82 tests passent.
  1. **[Affichage] texte-40-col / lo-res / HGR simple lisaient l'AUX sous
     80STORE+PAGE2** (`Apple2Display.cpp`). Le scanner vidéo //e ne
     multiplexe l'aux qu'en 80-col/DHGR ; en 40-col (80COL off) il lit
     TOUJOURS la page 1 PRINCIPALE. La base page1/page2 est déjà choisie par
     `videoTextPage2/videoHgrPage2` (= `page2 && !80store`, MAME
     `use_page_2()`) ; lire cette base depuis l'aux était un bug montrant du
     garbage aux pour les programmes qui font du page-flip 80STORE hors
     80-col. 4 redirections supprimées (renderText/renderLoRes/renderHiRes/
     renderHiResChatMauve80). Pin : `hgr_render` (MAIN=blanc, AUX=noir).
  2. **[GUI] le mode kiosque n'était pas read-only pour 3.5"/HDV**
     (`MainWindow.cpp` routeMount35/routeMountHdv) — `settings->save()`
     inconditionnel écrasait `state.cfg` depuis un lancement kiosque. Gardé
     sur `!kiosk_`.
  3. **[GUI] `POM2 game.hdv` (sans --kiosk) fuyait une carte fantôme dans la
     config sauvée** — `ensureHdvCardForBoot` est « non persisté » par
     contrat, mais `~MainWindow` écrivait `slot_N_card="hdv"` + `hdv_path`.
     Slot auto-provisionné tracé (`autoProvisionedHdvSlot_`) et exclu de la
     persistance.
  4. **[GUI] persistance CFFA multi-instance** — `~MainWindow` ne sauvait que
     la carte CFFA primaire ; une 2e carte perdait son image. Boucle sur
     `blockCards()` (comme DiskII).
  5. **[GUI] `restartEmulationFromSettings` perdait le média live** — un
     Insert menu / mount HDV/CFFA ne met à jour que la carte live (clés
     écrites au shutdown), donc un « Apply » de Slot Config reconstruisait
     depuis des clés périmées → disque monté perdu. Snapshot du média live
     dans les settings avant le teardown (symétrique avec `applyProfile`).
  6. **[Cassette] dernière impulsion de chaque bande WAV/MP3 perdue**
     (`CassetteDevice.cpp` `pcmToDurations`) — une durée n'était émise que
     SUR un changement de signe ; le segment tenu après le dernier
     zéro-crossing (leader final / demi-bit) n'était jamais converti. Flush
     de la queue après la boucle (symétrique avec le trailer 0,1 s du save).
     Pin : `cassette_wav_tail`.
  - **LOWs** : `diskDialogTargetSlot` remis à -1 à la fermeture du popup ;
    sniff ProDOS `next >= 280` (vol 280-blocs = blocs 0..279) ; valeur paddle
    centrée 127→128 (cohérence avec la valeur live + défaut Memory).
  - **Reporté** : SmartPort multi-instance demi-supporté (pas de persistance
    au shutdown — MED, involved) ; `loadAudioStream` mode flux mort (jamais
    dispatché — décision feature, pas un bug de correction) ; overload
    `loadFile(path,order)` Force-secteur-ordre limité au 143360 brut (latent,
    aucun appelant UI).

- **Bug hunt round 4 — 8 correctifs (serveur HTTP AI / souris / VIA / banking //c)**.
  Audit des sous-systèmes restants (serveur de contrôle HTTP, MC6821 + souris,
  6522 VIA, banking //c/c+). Le **bug X de la souris a été circonscrit** : la
  carte (MC6821 + quadrature + M68705) est fidèle à MAME (prouvé end-to-end
  contre le vrai firmware) — l'asymétrie X/Y est dans le GUI. 81 tests passent.
  1. **[AI HTTP] `/speed` gèle l'émulateur** (`AiControlServer.cpp`).
     `cycles_per_frame` (un `long`) était casté en `int` sans borne sup →
     `9999999999`→1,4 G cycles/frame (UI gelée) ; `4294967296`→0 (CPU en
     pause). Bornage [1, 2 000 000] + rejet 400 hors plage. Pin :
     `ai_control_server_smoke`.
  2. **[AI HTTP] `jsonGetString` matchait la clé n'importe où** (substring
     `body.find("\"key\"")`) → une valeur contenant le nom d'un autre champ
     détournait le match (toutes les routes). Réécrit en scan structurel qui
     saute les valeurs-chaîne (la clé ne matche qu'en position de clé, suivie
     de `:`). Pin : `ai_control_server_smoke`.
  3. **[AI HTTP] évasion du jail cwd via symlink** (sauvegarde snapshot).
     `weakly_canonical` rend un symlink final lexicalement (dans cwd) mais
     `ofstream` le suit hors du jail → écriture de fichier arbitraire. Rejet
     du symlink final (`symlink_status`). Pin : `ai_control_server_smoke`.
  4. **[GUI souris] X 2× trop sensible en 80-col/DHGR** (`MainWindow.cpp`).
     Le widget est toujours dessiné en `kWidth`(280)×`kHeight`(192), mais
     `sxRatio` divisait par `display->width()`=560 en DHGR (vs 192 pour Y) →
     X suivait 2× plus vite que Y en 80-col (là où tourne A2Desktop). Utilise
     `kWidth/kHeight` pour les deux axes. (Le « stuck à 8 px » résiduel est
     très probablement le clamp X d'A2Desktop, hors émulateur.)
  5. **[AI HTTP] `/status` échantillonnait CPU et disque sous deux locks**
     séparés → snapshot incohérent (le worker relâche le lock entre chunks
     de 4096 cycles). Lecture sous un seul `lock_guard`.
  6. **[VIA] `t1FireArmed` init membre (`true`) ≠ `reset()` (`false`)**
     (`Mockingboard.cpp`) — landmine latente (ctor reset, inoffensif
     aujourd'hui). Aligné à `false`.
  7. **[//c] décode IOUDIS incomplet** (`Memory.cpp`). MAME : sur //c TOUT
     `$C078/A/C/E` est SETIOUDIS et tout `$C079/B/D/F` CLRIOUDIS ; POM2 ne
     décodait que `$C078/E` / `$C079/F`. Décodage par parité sur
     `$C078-$C07F`.
  8. **[//c] reset MIG non gardé sur `isPlus_`** (`MemoryProfile_IIcClass.cpp`).
     MAME garde le reset MIG sur `m_isiicplus` ; un //c 32 K (rev 0/3/4) a une
     banque alt mais pas de MIG. Gardé sur `isPlus_`.
  - **[#4/#5/#6/#7/#8]** correctifs GUI / refactor / banking LOW non pinnés
    unitairement (intégration MainWindow, cohérence concurrente, profil //c
    coûteux à monter) — couverts en régression (iic/iicplus boot traces).
  - **Reporté sans corriger** (avec raisons) : 6522 SR/CA-CB/PCR non modélisés
    (features hors scope, déjà dans TODO) ; reload T1 continu `+3` vs `+2`
    (disputé, impact <0,5 % sub-audible — ne pas toucher un timing fonctionnel
    sans oracle MAME cycle-trace) ; `jsonGetInt` hex négatif + DoS mono-thread
    (mineurs).

- **Bug hunt round 3 — 9 correctifs (CLI / audio / 6805 souris)**. Audit des
  sous-systèmes restants (cœur 6805 du MC68705P3, pile audio, profils + CLI,
  synthèse ProDOS + RamWorks). La synthèse ProDOS et RamWorks sont ressorties
  **propres** (re-vérifiées vs spec + 3 images réelles). 81 tests passent.
  1. **[CLI] `parseAddr16` adresses + garbage** (`CliDispatcher.cpp`). Décodage
     incohérent (branche décimale gatée sur la longueur > 4) + `std::stol` sans
     vérif de consommation totale → `"12ZZ"`→$12, `"  5"`→$5 acceptés
     silencieusement. Réécrit : adresses **hex** (convention Apple II, comme
     `README` « hex address »), préfixe `$`/`0x` optionnel, premier caractère
     hex obligatoire, token entièrement consommé, plage $0000-$FFFF. Pin :
     `cli_kiosk`.
  2. (avec #1) rejet du garbage / hors-plage.
  3. **[CLI] `--preset iie-u` injoignable** (`CliDispatcher.{h,cpp}` +
     `main.cpp`). `CliPreset` n'avait pas `AppleIIeUnenhanced` et
     `parsePresetName` aucun cas — alors que `profileFromKey` (menu) et la doc
     les listent. Ajout de la valeur enum, des alias
     (`iie-u`/`iieunenhanced`/`apple2e-1983`/`//e-u`) et du `case` du switch
     `main.cpp`. Pin : `cli_kiosk`.
  4. **[CLI] `--ii-plus` écrasé par le profil persistant** (`MainWindow` ctor).
     `forceIIPlus` n'était honoré que dans l'auto-probe legacy ; le rattrapage
     « saved profile » réappliquait ensuite un `iie/iic` sauvegardé. Gardé : le
     profil persistant est ignoré quand `forceIIPlus`.
  5. **[CLI] `--35-disk1/2` sans gate de profil** (`main.cpp mount35Cli`).
     Montait dans le hub 3.5" on-board même hors //c+ (contrat documenté //c+
     only). Gate `currentProfile()==AppleIIcPlus` + warning sinon.
  6. **[Audio] requeue d'événements speaker en ordre inversé**
     (`SpeakerDevice.cpp`). `push_front` en k croissant inversait la queue des
     événements résiduels (fin de buffer) → invariant ascendant rompu →
     glitches de timing de clic. Reverse-iteration.
  7. **[Audio] enveloppe AY non re-déclenchée sur réécriture R13 même valeur**
     (`Mockingboard.{h,cpp}`). Un vrai AY-3-8910 redémarre l'enveloppe à
     CHAQUE écriture R13 ; POM2 ne re-init que sur valeur changée. Le thread
     audio ne voit pas une écriture même-valeur via le snapshot de registres →
     compteur monotone `ayEnvWriteCount_` (calque de `ayResetCount_`). Pin :
     `mockingboard_smoke` (la sortie est AC-couplée comme la vraie carte, donc
     le test mesure le transitoire de rampe).
  8. **[6805] CLR mémoire faisait une lecture parasite** (`M68705P3.cpp`).
     MAME `clr` est write-only (ARGADDR) ; POM2 passait par `rmw_mem` (rdmem
     d'abord) → sur une adresse de port ($00/$01/$02) ça déclenchait une
     lecture (synthèse de front quadrature côté souris). Helper `clr_mem`
     write-only pour `$3F/$6F/$7F`. Pin : `m68705_decode`.
  9. **[6805] MUL ($42) absent** → décodé en NOP. Opcode HMOS réel (X:A = X×A,
     efface H et C). Ajouté. Pin : `m68705_decode`.
  - **[#4/#5/#6]** correctifs GUI/arrondi non pinnés unitairement (intégration
    MainWindow / chemin de fin de buffer) — couverts en régression.

- **Bug hunt round 2 — 6 correctifs (stockage / FDC / snapshot)**. Audit des
  sous-systèmes non couverts au round 1 (IWM, pile 3.5"/SmartPort, cartes
  bloc ProDOS, glue système). 81 tests passent.
  1. **[SmartPort] WRITE_BLOCK firmware exécutait la routine READ**
     (`SmartPortCard.cpp` `buildRom`). Erreur d'offset d'un octet dans le
     dispatch ProDOS `$Cn50` : `BEQ write` encodé `$22` (saute à
     l'offset 53 = milieu de la routine READ) au lieu de `$2E` (offset 65 =
     routine WRITE). Toute écriture ProDOS via le firmware (la carte
     s'annonce write-capable, `$CnFE=$13`, et ProDOS dispatche par
     `$CnFF=$50`) **lisait** le bloc dans le buffer appelant, n'écrivait
     rien sur le média, et renvoyait « succès » → perte de données
     silencieuse + corruption du buffer. Pin CPU-driven :
     `smartport_write_dispatch`. (Les pins existants n'exécutaient que le
     protocole brut `$C0nX`, jamais le dispatch `$Cn50`.)
  2. **[Sony 3.5"] tête bloquée en pas-vers-track-0** (`Sony35Drive.cpp`
     `strobeWriteRegister`). `directionIn_` n'était jamais remis à false :
     reg 0x0 → inward, reg 0x4 → eject, **aucun chemin outward** (la branche
     cyl+1 du step était morte). Réaligné sur MAME `mac_floppy_device::
     seek_phase_w` : reg 0x0 = DirNext (cyl+1, outward), reg 0x4 = DirPrev
     (cyl-1, vers track 0), reg 0x7 = StartEject. Les tracks 1-79 du 3.5"
     étaient inatteignables via le protocole. Pin : `sony_seek_direction`.
  3. **[Snapshot] restore incomplet** (`AiControlServer` + `M6502` +
     `Memory` + `SnapshotIO`). Le load (a) jetait A/X/Y/P/SP (M6502 n'avait
     que `setProgramCounter`) et (b) la section MEM ne couvrait que les
     64 Ko principaux visibles — aux RAM, Language-Card RAM, banques
     RamWorks, soft-switches de pagination (`iieMemMode`) et DisplayState
     n'étaient jamais sérialisés. Tout programme IIe/aux/LC restaurait du
     garbage. Ajout des setters registres M6502, de
     `Memory::appendSnapshotState`/`loadSnapshotState` (nouvelle section
     « MEX », format **v2** — les v1 se chargent toujours) et de
     `restoreMainRam` (respecte `writable[]` → la ROM n'est plus écrasée).
     Pin : `snapshot_state_roundtrip`.
  4. **[IWM] délai async `+14` non mis à l'échelle** (`IWMDevice.cpp`).
     POM2 fait tourner la FSM IWM sur l'horloge CPU et divise les
     constantes de fenêtre MAME (base 7,16 MHz) par 7 ; ce `+14` était resté
     brut → le timer « data devient stale → 0 » des lectures 3.5" async se
     déclenchait ~7× trop tard. 14/7 = 2.
  5. **[CFFA] octets de config EEPROM non patchés** (`CffaCard.cpp`
     `loadRom`). MAME `a2cffa.cpp` device_start() force `m_rom[0x800]=13`,
     `m_rom[0x801]=13` à chaque boot (le dump brut porte `0x04`/`0x00`,
     `$C801=0` désactive le 2e connecteur) ; POM2 chargeait verbatim → le
     firmware scannait une EEPROM différente de l'oracle MAME. Pin ajouté à
     `cffa_card_smoke`.
  6. **[IWM] sous-dépassement `halfWindowSize() - 7`** (`IWMDevice.cpp`,
     FSM d'écriture). `halfWindowSize()` mis à l'échelle ∈ {1,2} moins `7`
     brut sous-dépassait en `uint64_t`. 7/7 = 1 ; corrige aussi les deux
     offsets de chargement `+7` → `+1`.
  - **[#4/#6]** pinnés au niveau régression (`iwm_device_smoke` +
    `iic`/`iicplus` boot traces) — un test de timing dédié exigerait un
    harnais flux-replay disproportionné.

## 2026-05-24

- **Bug hunt — 5 correctifs de parité (audit multi-sous-systèmes)**. Tous
  pinnés ; 78 tests passent.
  1. **[CPU] Cycles d'entrée IRQ/NMI perdus** (`M6502.cpp` `step`).
     `handleIRQ`/`handleNMI` accumulaient les 7 cycles d'entrée dans
     `cycles`, mais `executeOpcode()` réinitialise `cycles = 1` juste après
     → les 7 cycles étaient **écrasés** avant `advanceCycles`. Chaque
     interruption coûtait donc **0 cycle**, désynchronisant toutes les
     horloges dérivées de `cycleCounter` (VBL, timers de slot type
     Mockingboard 6522 T1, cassette) sur le logiciel piloté par IRQ. Corrigé
     en capturant le coût d'entrée puis en le réinjectant après
     l'instruction. Même classe que le bug RMW de Mr. Robot. Pin :
     `cpu_cycle_count` (IRQ + NMI = 7 + instr).
  2. **[Disques] `writeFlux` largeur de cellule codée en dur à 8 LSS**
     (`DiskImage.cpp`). Le chemin de lecture espace les cellules à
     `i*lssCyclesPerCell()`, mais le write-back divisait par `8`. Pour un
     WOZ2 dont `optimal_bit_timing != 32` (cyc ≠ 8) les transitions étaient
     éparpillées dans les mauvaises cellules → **corruption silencieuse** de
     la piste au write-back. (Le test `woz_writeback` fixe `info[39]=32`,
     d'où l'angle mort.) Corrigé via `lssCyclesPerCell()` dans les deux
     branches. Pin : `woz_writeflux_smoke` (obt=40/28 round-trip).
  3. **[CPU] `INC A`/`DEC A` 65C02 ($1A/$3A) facturés 4 cycles** au lieu de
     2 (`M6502.cpp`) : le corps ajoutait `+2` en plus du fetch + `Imp`.
     Aligné sur INX/DEX (corps = 0). Pin : `cpu_cycle_count`.
  4. **[SSC] CR NUL telnet → CR parasite** (`SuperSerialCard.cpp`
     `normalizeLineEndings`). RFC 854 : un retour-chariot nu arrive en
     `CR NUL`. Le NUL exécutait `prevCR = false` **avant** d'être supprimé,
     si bien qu'un `CR NUL LF` produisait `CR CR` au lieu de `CR` (ENTER
     doublé en session telnet interactive). Corrigé en supprimant le NUL en
     premier ; fonction rendue `static` publique pour le test. Pin :
     `ssc_acia_smoke`.
  5. **[Memory] Floating bus ignore 80STORE** (`Memory.cpp` `floatingBus`).
     Le scanner vidéo n'honore PAGE2 que si 80STORE est OFF
     (MAME `apple2video.cpp` `use_page_2() = page2 && !80store`) ; avec
     80STORE ON, PAGE2 redirige la sélection de banque aux et non la page
     affichée. `floatingBus` lisait donc la mauvaise page DRAM sous tout
     programme 80-col/DHGR. Pin : `floatingbus_page2_smoke`.

- **Infra de release Linux** (branche `release-infra`). Nouveau résolveur
  d'assets `ResourcePaths` (`.h/.cpp`) : centralise la recherche des assets
  *bundlés* (roms/, fonts/, floppy_samples/) avec, en plus des racines
  historiques relatives au CWD (`.`, `..`, `../..` — comportement dev
  inchangé), des racines **relatives à l'exécutable** (`<exe>`, `<exe>/..`,
  `<exe>/../share/POM2`) et la racine **utilisateur** `~/.local/share/POM2`.
  Indispensable pour qu'un binaire *installé* (AppImage, `.deb` → `/usr/bin`,
  tarball lancé par chemin absolu) trouve ses fichiers. Câblé sur les
  chokepoints : `RomLoader::probe*`, `MainWindow::firstExistingPath`,
  `CharRomCatalog::resolveCharRomPath`, le probe ROM CFFA, les boucles
  inline disk2/LSS/mouse/char/auto-détect, et les polices de `main.cpp`.
  **Pourquoi** : tous les sites répétaient `X` / `../X` / `../../X`, qui ne
  marche que si le CWD est la racine du dépôt — ce qu'une release casse.
  Les 76 tests passent inchangés. CMake gagne `install()` (bin/ +
  share/POM2) + **CPack** (TGZ relocatable + DEB shlibdeps) ; `build_dist.sh`
  produit `DIST/POM2-v0.6-linux-x86_64.{tar.gz,deb}` (+ AppImage si
  `linuxdeploy` présent). **Les ROMs Apple ne sont jamais incluses**
  (copyright) — un `roms/README.txt` indique où les déposer. Pièges :
  la cible CMake GUI est `pom2_imgui` (sortie `POM2`) — `make POM2` est un
  no-op, le script build via `--target pom2_imgui` ; `pom2_headless` et les
  tests compilent `RomLoader.cpp` donc linkent aussi `ResourcePaths.cpp`.

- **Release v0.6** — jalon regroupant CFFA 2.0 (carte IDE MAME-fidèle),
  Floppy Emu (BMOW), Slot Manager consolidé, rendu Video-7 / Le Chat Mauve
  complet, et le support Disk II 13-secteurs (DOS 3.1/3.2). Version bumpée
  dans `main.cpp` (bannière + titre fenêtre), `MainWindow.cpp` (À propos),
  `CMakeLists.txt` (`project VERSION`) et `CLAUDE.md`.

- **Floppy Emu (BMOW) — modèle du gadget SD/OLED** (`floppy_emu_smoke`).
  Nouveaux `FloppyEmuDevice` + `FloppyEmu_ImGui` modélisant le Big Mess o'
  Wires Floppy Emu : carte SD + OLED 128×64 + 3 boutons (PREV/NEXT/SELECT)
  qui *devient* un lecteur. POM2 émule déjà tous les types de lecteurs que
  l'Emu peut présenter — la valeur ajoutée est donc le comportement *propre*
  du gadget : le **mode d'émulation** persistant (sa NVRAM ; 4 modes 5.25 /
  3.5 / Unidisk 3.5 / Smartport HD), l'**explorateur de fichiers SD** (borné
  à la racine, filtré par mode), les **favoris** (`favdisks.txt` + directive
  `automount`), le workflow de swap. Le montage réel est **routé par
  MainWindow** vers les cartes existantes (`DiskIICard` / unités
  `SmartPortCard`) — le device choisit l'image + le mode, rien d'autre. Le
  cœur est volontairement agnostique UI/émulateur (zéro ImGui / MainWindow /
  SlotBus) → testable en isolation (filtrage de format, navigation SD,
  parsing favdisks). La « carte SD » est le dossier `floppyemu/`, **séparé**
  des dossiers Disk Library (`disks/` `disks35/` `hdv/`) car l'Emu est sa
  propre carte. Réf : manuel BMOW Floppy Emu Model C §3 + §5. Menu : Devices
  → Floppy Emu. Réglages `floppyemu_mode` / `floppyemu_sd_root` /
  `show_floppy_emu`. Hors scope v1 : modes Dual-5.25 et Smartport-Unit-2
  (boot daisy-chain IIgs). Voir `DEV.md § Host control center`.

- **Slot Manager — panneau bus consolidé + `MountableMediaCard`**
  (`slot_multi_card_smoke`). Nouveau `SlotManager_ImGui` : une seule fenêtre
  liste tous les slots (1-7 + ligne AUX 80-col en IIe), la carte par slot
  (dropdown ou badge built-in verrouillé), et — pour toute carte exposant des
  baies média — mount / eject / write-back / type-select / boot inline par
  baie. **Pourquoi** : le bus est multi-instance (plusieurs cartes bloc,
  plusieurs SmartPort) mais les anciens panneaux par-carte supposaient chacun
  un pointeur global unique. Le Slot Manager se construit depuis une
  **énumération du SlotBus** (`bus.peripheral(s)` +
  `dynamic_cast<MountableMediaCard*>`), donc reste correct quel que soit le
  nombre de cartes plugguées. `MountableMediaCard.h` = mix-in **côté hôte**
  (pas une préoccupation bus, même motif que `ProDOSBlockCard`) :
  `bayCount/bayInfo/mountBay/ejectBay/setBayWriteBack` + type-select
  (`bayTypeOptions/setBayType`). `ProDOSBlockCard` l'implémente comme une
  baie fixe → HDV + CFFA l'obtiennent gratuitement ; `SmartPortCard`
  l'implémente sur ses 2 unités (type empty / 3.5" / hdv). `SlotCardCatalog.h`
  extrait la liste unique `kCardTypes` + les sondes ROM
  (`mouseRomsPresent`/`cffaRomPresent`), partagée entre le panneau Slot Config
  hérité et le Slot Manager. Multi-instance élargie : le Slot Manager autorise
  `diskii`/`cffa`/`smartport35` en plusieurs slots (persistance par slot). Les
  panneaux par-carte détaillés subsistent pour l'état profond (LED de piste,
  moteur, Disk Library) via « Open detailed panel ». Menu : Machine → Slot
  Manager. Réglage `show_slot_manager`. Voir `DEV.md § Host control center`.

- **SmartPortHdvUnit — adossé au `Block512Backing` partagé**. L'unité HDV
  d'une carte SmartPort réimplémentait son propre stockage ; elle est
  désormais un fin adaptateur sur le `Block512Backing` commun (le store
  derrière `ProDOSHardDiskCard` / `CffaCard`) → envelope 2IMG, suivi des
  blocs sales, WP médium et write-back opt-in obtenus sans duplication. Chaque
  `SmartPortUnit` possède ses octets (fin du `thread_local`). Pinné par
  `smartport_mixed_units_smoke`.

- **Rendu Video-7 / Le Chat Mauve complet (4 rgbmodes DHGR + texte
  foreground-background)**. `Apple2Display::renderDhgr` consulte désormais le
  mode FIFO AN3 de la carte (`LeChatMauveCard::currentMode()`) pour choisir
  l'un des quatre rgbmodes de MAME `apple2video.cpp` `dhgr_update()` :
  `COL140`(3, couleur 140-cellules, déjà présent), `Mixed`(1, le MSB de chaque
  octet source choisit couleur vs mono 7-points), `Chunky160`(2, `aux+(main<<8)`
  → quatre pixels 4-bit de 3 points, 480 centrés dans 560) et `BW560`(0, mono
  strict). Nouveau `renderTextChatMauveFgBg` : mode **texte couleur Video-7**
  (texte 40-col + AN3 actif) — le code char vient de la RAM principale, les
  couleurs fg/bg par cellule de l'octet aux à la même adresse (quartet haut =
  fg, bas = bg ; glyphe 7 bits doublé en 14 points). Porté verbatim de MAME
  (`dhgr_update` `:885-980`, `render_line_color_array` `:571-583`, `text_update`
  `:788-791`), palette Feline AppleWin conservée (deux gris distincts). Le
  refactor de `COL140` est bit-identique à l'ancien chemin (les tests existants
  passent inchangés). Épinglé par `tests/video7_parity_smoke_test.cpp` (les 4
  rgbmodes + fg/bg comparés ligne par ligne — 560 points — à un oracle MAME
  `dhgr_update` autonome embarqué dans le test). 72 tests OK.

- **`bootFromSlot` réplique l'init zéro-page du Monitor F8 (corrige Mr. Robot
  4am)**. `EmulationController::bootFromSlot` (raccourci `coldBoot + PC=$Cn00`
  utilisé par le kiosk/CLI, les boutons « Boot » de la Disk Library ET « Boot
  HDV ») saute le cold-start du Monitor F8 — donc n'initialisait PAS la
  zéro-page texte/I-O que les routines écran du Monitor attendent. Concret :
  CLREOP (`$FCA0`) boucle sur `CPY WNDWDTH($21)` ; avec `$21` laissé à 0 elle
  **déborde la ligne et écrase le loader en `$0800`** → le crack Mr. Robot 4am
  (`.dsk` **et** `.woz`) calait (boot1 effacé → `$A0`, moteur tournant). Le
  même disque **bootait via l'autostart naturel**. Fix : après `clearRam`,
  `bootFromSlot` initialise la fenêtre texte (`$20-$23`), `CH/CV` (`$24/$25`),
  `BASL/BASH` (`$28/$29`=$0400), `INVFLG` (`$32`), et les hooks `CSW/KSW`
  (`$36-$39` → COUT1/KEYIN) — valeurs vérifiées contre un dump autostart→BASIC.
  Corrige **tous** les chemins (kiosk, GUI Disk Library, HDV) tout en gardant
  le ciblage de slot. Diagnostic via **MAME oracle** (flatpak `apple2p`, romset
  reconstitué depuis `roms/` — [[reference_mame_oracle]]) : MAME bootait
  l'image, POM2 non ; diff de trace POM2↔MAME puis bootFromSlot↔autostart →
  confounds éliminés un par un (profil, turbo, image, opcodes illégaux, phase
  d'échantillonnage, moniteur ROM, long-cycle, LSS) jusqu'à isoler l'init F8.
  Mr. Robot affiche son écran-titre ; Choplifter + Total Replay (HDV) bootent
  toujours (71/71 tests verts). Le **bug CPU INC/DEC** trouvé en chemin (cf.
  ci-dessous) était réel mais distinct — gardé.

## 2026-05-23

- **M6502 — INC/DEC mémoire : 1 cycle manquant corrigé** (`cpu_cycle_count`).
  `INC`/`DEC` en zp/abs/zp,X/abs,X chargeaient 1 cycle de trop peu (le
  cycle « dummy » read-modify-write : `INC abs` = 5 au lieu de 6). Les
  autres RMW (`ASL/LSR/ROL/ROR/TSB/TRB`) étaient déjà corrects (`+3`) ;
  seuls `INC`/`DEC` faisaient `+2`. Klaus ne l'attrapait pas (il teste les
  *résultats*, pas le *timing*). Trouvé via un diff de trace cycle-à-cycle
  contre MAME (`apple2p`, oracle reconstitué) sur le boot d'un loader
  sensible au timing : les cycles du boot0 collent désormais exactement à
  MAME. Réf MAME `om6502.lst` / `ow65c02.lst` (INC mem = 5/6/6/7). Affecte
  NMOS **et** 65C02. Nouveau test `cpu_cycle_count` épingle les timings RMW
  (premier test de *comptage de cycles* — leur absence avait laissé passer
  le bug). Voir `TODO.md § High [DiskII]` pour le contexte (Mr. Robot 4am).
- **ClockCard — interruptions TP (Timing-Pulse) câblées** (`clock_card_smoke`,
  5 nouveaux cas). La sortie TP de l'uPD1990AC bascule à une cadence
  sélectionnable et la ThunderClock+ la route (via une bascule d'activation)
  vers la ligne IRQ du slot — source d'interruption périodique pour les
  utilitaires horloge/scheduler, qui ne tickaient jamais avant. *Rates* =
  source de vérité MAME `upd1990a.cpp:248-257` (modes TP) + `:176-181`
  (REGISTER_HOLD) : diviseurs 512/128/16/8 sur le XTAL 32.768 kHz →
  **64/256/2048/4096 Hz** (modes 4-7) ; SHIFT/TIME_SET/TIME_READ laissent
  TP inchangé (comportement normal-mode MAME). `programTpTimer()` décode le
  mode latché sur front montant STB, `setTpRate()` convertit en cycles
  CPU/bascule, `advanceCycles()` pilote la bascule et lève l'IRQ sur chaque
  front montant. *Câblage IRQ* = **POM2-original** (MAME `a2thunderclock.cpp`
  ne lie jamais `tp_callback`) suivant le **manuel ThunderClock Plus ch. V** :
  `$C0n0` bit 6 (`$40`) = latch d'activation ; tout accès device-select
  efface la requête (le latch persiste → tick continu) ; lecture `$C0n0`
  bit 5 = drapeau « interrupt asserted » (polling manuel 5-3) ; RESET
  désactive. **Hors portée** : les interval timers (1/10/30/60 s, modes
  8-15) exigent la commande série 4-bit de l'uPD4990A, inatteignable sur
  l'uPD1990AC parallèle de la carte. Voir `DEV.md § ProDOS clock card`.
- **Disk II 13-secteurs (pré-DOS 3.3) — DOS 3.2 boote** (`dos32_boot_trace`,
  `d13_roundtrip_smoke`). Support complet des disquettes DOS 3.1/3.2/3.2.1 :
  détection `.d13` / 116480 o → `ImageKind::Dos32_13` (+ détection 13s des
  `.nib` par scan `D5 AA B5`), encode/decode GCR **5-and-3**
  (`nibblizeTrack13`/`decodeTrack13`, port verbatim MAME
  `formats/ap2_dsk.cpp` `a2_13sect_format` — `kTranslate5`, prologue
  adresse `D5 AA B5`, champ data 411 nibbles), write-back,
  `sectorsPerTrack_`/`is13Sector()`. `DiskIICard` sert la boot PROM
  **341-0009** (`roms/disk2_13.rom`) quand une 13s est montée
  (`serving13_`), force la LSS bit-level, et **lit via le séquenceur P6
  16-secteurs** (341-0028) — la LSS est agnostique de l'encodage, le
  décodage 5-and-3 est logiciel (boot/RWTS) ; le dump brut 341-0010 ne
  pilote pas `lssSync`. Un vrai master **DOS 3.2 boote** (`DOS32STD.d13`)
  via l'Autostart ][+ jusqu'au prompt `]` (greeting « LANGUAGE NOT
  AVAILABLE » = HELLO Integer sur II+ Applesoft). Round-trip encode↔decode
  pinné byte-pour-byte sur 35 pistes ; boot pinné end-to-end. Images de
  test dans `disks/dsk/` (DOS32STD.d13, DOS32PLS.D13, dos32std.nib…).
- **HDV mass-storage — pinning 32 MB** (`hdv_mass_storage_smoke_test`).
  Nouveau smoke test couvrant les trous restants du stockage de masse
  ProDOS : borne de capacité (65536 blocs = 32 MiB acceptés, 65537
  rejetés, fichier non-multiple de 512 rejeté), adressage bloc **16-bit**
  (l'octet haut du sélecteur `$C0D1` — un volume 32 Mo a besoin de blocs
  jusqu'à 65535, les autres tests ne touchaient que les blocs 0-1), et
  les `.2mg` avec data-offset ≠ 64. La préservation en-tête/trailer/WP
  au write-back était déjà pinnée par `hdv_writeback_smoke_test`. Reste
  hors scope : les images **multi-partition** (CFFA3000-style, 1 image =
  N volumes ProDOS) — feature, pas un test. Voir `DEV.md § HDV`.
- **Memory — stratégie de profil //c-class extraite** (`MemoryProfile`).
  Toutes les spécificités //c / //c+ qui fuyaient dans le dispatcher
  générique `Memory::memRead/memWrite` (alt-firmware `$C028` ROMBANK,
  IWM on-board `$C0E0-$C0EF`, MIG `$CC00/$CE00`, INTCXROM forcé, alt
  firmware sur `$C100-$FFFF`) passent derrière une interface
  `MemoryProfile` (`MemoryProfile.h` + `MemoryProfile_IIcClass.{h,cpp}`).
  `Memory::iicProfile_` est non-null **uniquement** sur //c/c+
  (créé/détruit dans `loadAppleIIRom`) ; II/II+/IIe → un seul
  `if (iicProfile_)` sur le hot path, **zéro virtual call**. `migRead`/
  `migWrite` (MAME `apple2e.cpp:532-624`) déplacés verbatim dans le
  profil. Les façades `setIWM/setSmartPortHub/setIWMAuthoritative`
  forwardent vers le profil (câblage des tests inchangé = garde-fou).
  Supprime les prédicats `isIIcClass`/`isIIcPlus`/`iicHasAltBank`
  éparpillés ; prérequis du split `Memory` god-object restant
  (`Keyboard`/`PaddleInputs`) et du futur profil IIgs. Vérifié : 62/62
  ctest + boot //c (U4boot.dsk → écran-titre) + banner //c+. Voir
  `DEV.md § MemoryProfile`.

## 2026-05-16

- **Cassette auto-rewind opt-in** (`3f42efc`). L'auto-rewind 500 ms
  (POM2-only, MAME ne rewind jamais) cassait les loaders custom à
  polling sporadique de `$C060` (Penguin Software fast loaders, etc.).
  Désormais gaté derrière `autoRewindEnabled`, **default off**
  (`CassetteDevice.cpp:470`). Setter `setAutoRewindEnabled(bool)`,
  toggle UI dans le panel Cassette.
- **SSC — LF→CR symétrique sur RX** (`3f42efc`). La normalisation
  CR LF → CR + bare LF → CR + drop NUL est appliquée **une fois**
  sur `scratch[]` brut avant que les bytes ne soient livrés à `rxBuf`
  ET au `keyboardSink` (`SuperSerialCard.cpp:91-104, 225-246`).
  Avant : les terminal apps lisant via `$C0A8` voyaient des paires
  CRLF et des LF orphelins.
- **SSC — raw mode toggle** (`3f42efc`). Nouveau toggle `rawMode_`
  qui bypasse `swallowTelnetIac` ET `normalizeLineEndings` côté RX
  pour les protocoles binaires 8-bit (XMODEM/Kermit/ADTPro). UI
  toggle dans le panel SSC, persisté via `ssc_raw_mode`.
- **Mockingboard — compteur de tons AY entier** (`3f42efc`). Les
  compteurs de phase des 3 canaux de tons (et du bruit) passent de
  `float` à `uint16_t` + accumulateur fractionnaire séparé (MAME
  `ay8910.cpp:998-1015` utilise des compteurs uint). Le float pur
  accumulait une erreur d'arrondi dans la boucle de résolve
  `while (counter >= period)`, ce qui faisait dériver/aliaser les
  périodes 1-3 — les tricks PWM (style Cosmic Bouncer) sonnaient faux.
  Compteur entier ⇒ plus de dérive (`Mockingboard.cpp:495, 607-617`).
  NB : pas encore pinné par un test (synthèse sur le thread audio).
- **SmartPortCard — refactor multi-unités** (`7db6861`). Chaque
  carte SmartPort possède maintenant ses propres `SmartPortUnit`
  polymorphes (3.5" Sony 800K ou ProDOS HDV au choix par unité), avec
  buffers de bloc par unité (plus de `thread_local`). Per-unit settings
  `smartport_slotN_unitK_{type,path,writeback}`. Pinné par
  `smartport_card_smoke_test.cpp` + `smartport_mixed_units_smoke_test.cpp`.
- **Disk Library — locale char ROM + eject** (`71cec5d`). Sélection
  de locale pour le character ROM dans la Disk Library + bouton
  d'éjection.
- **Audio peak monitoring** (`af4fad5`, `fad8e13`). `AudioDevice`
  suit les peaks par source (chaque carte) en plus du master peak.
  Diagnostic des saturations / cards qui dominent le mix.
- **DiskII multi-instances (option C)** (`5f7f209`). `MainWindow`
  héberge `std::vector<DiskIICard*> diskCards` ; permet
  `Disk II slot 6 + Disk II slot 4` (4 drives 5.25" sur un //e).
  Per-slot settings `disk_path_slotN`.
- **Shamus DHGR — entrée erronée retirée**. Shamus est un jeu HGR
  (Synapse 1982) qui tourne correctement ; l'item TODO précédent était
  basé sur une confusion HGR/DHGR ou déjà résolu silencieusement.

## 2026-05-14 / 2026-05-15

### UI

- **UI Toolbar + Disk Library unifiée**. Nouveau `Toolbar_ImGui` pinné
  sous la menu bar (10 boutons d'accès rapide + 2 combos), nouveau
  `DiskLibrary_ImGui` (panneau 3-onglets avec recherche + tri + table
  + markers d'images montées + click→insert/mount + boot). Toggles
  persistés.

### SmartPort 3.5" pluggable

- **SmartPortCard slot //e**. Nouveau `SmartPortCard.{h,cpp}`, slot 5
  par défaut, expose les 2 `Disk35Image` d'`EmulationController` en
  block-device ProDOS via slot ROM signée + dispatcher $Cn50 +
  soft-switches $C0nX. Driver $Cn50 examine `$43` bit-7 pour router
  drive 1 / drive 2. PR#N boote drive 1. Pinné par
  `smartport_card_smoke_test.cpp`.
- **Sons mécaniques 3.5"**. `Sony35Drive` reçoit un `FloppySoundSink*`
  (réutilise le `FloppySoundDevice` du Disk II). `seekPhaseW(uint8_t,
  uint64_t emuCycles)` propagé via `SmartPortHub` depuis
  `IWMDevice::emuCycles()`. `strobeWriteRegister` émet step / motor on
  / motor off / click avec stamping cycle CPU émulé (parité MAME).
- **Disk35 panel UX**. LED moteur rouge par drive, marqueur `* ` sur
  les images chargées, write-back checkbox per-drive, left-click =
  mount + cold boot, right-click = menu contextuel drive 1/2 × boot/
  no-boot.

### Disques

- **cc65-Chess.po hang après `LOADING CHESS`** résolu (2026-05-15).
  Les fixes round 1 (`kSyncMinRun=5` + `revolutionStartLssCycle`
  per-drive) combinés aux corrections de chemin de boot (B1-B5)
  suffisent — le `JSR $A403` ne tape plus sur `$00`.
- **Floppy sound step cadence** mesurée en cycles CPU émulés (MAME
  `floppy_sound_device::step` via `machine().time()`,
  `floppy.cpp:1532-1620`), pas en frames audio. **Pourquoi** : en
  turbo 60×, toutes les ~80 pulses du PROM de boot tombaient dans le
  même buffer audio de 5 ms → `audioFrameCounter_` constant →
  fallback `STEP_1_1` avec `stepPos_=0` réinitialisé par event →
  l'attaque de `step_1_1` (5 ms) rejouée buffer après buffer = buzz /
  son haché. `FloppySoundSink::step()` prend maintenant `emuCycles`,
  `DiskIICard::seekPhaseW` passe son `cpuCycleTotal`. Pinné par
  `floppy_sound_smoke_test.cpp` (`testRapidStepsNoHang` +
  `testSameCycleStepsClampGracefully`).

### MAME-parity round 1 (Mockingboard / IIe / //c)

- **HIGH-1** Mockingboard AY tone/noise/envelope rate ×2 (clock/16 →
  clock/8 + noise prescaler MAME `ay8910.cpp:1086-1104`). Toutes les
  musiques Mockingboard (Ultima IV, Nox Archaist, Total Replay,
  Skyfox) jouent enfin à la bonne fréquence — avant, 1 octave trop
  bas + enveloppes 2× trop lentes.
- **HIGH-2** $C019 RDVBLBAR clear-on-read divergence (Apple IIc Tech
  Note #9 : « does not reset »). Retiré le clear-IRQ-on-read de
  `Memory.cpp`.
- **MED-1** //c $C02x ROMBANK range étendu de $C028 seul à
  $C020-$C02F entier (MAME `apple2e.cpp:1894-1922` `(offset & 0xf0)
  == 0x20`). Cassette toggle gated sur `!iicHasAltBank`.
- **MED-2** Drive-switch revolution_start per-drive
  (`revolutionStartLssCycle[2]` + `kNeverRev`). MAME
  `wozfdc.cpp:264-291` + `floppy.cpp:809-839 mon_w`.
- **Floating bus** port verbatim `scanner_address` (MAME
  `apple2video.cpp:124-201`). addend0=0x0D + h-carries + v_4/v_3
  doublés dans addend2 + HBL phantom row $1000 gated sur !//e.
- **LOW-1** AY LFSR re-seed sur PB2=0 reset (MAME `ay8910.cpp:1309`
  `m_rng=1`) via cross-thread `ayResetCount_` signal.
- **LOW-2** VIA T1 continuous reload `+2` → `+3` (MAME `IFR_DELAY`).
- **LOW-3** AY register strobe fire sur tout call (drop debounce).
- **LOW-4** AY `reset_w` wipe regs 0..13 (R14/R15 préservés, MAME
  `ay8910_reset_ym`).

### RamWorks III

- Port verbatim `a2eramworks3.cpp`, jusqu'à 8 MB, pinné par
  `ramworks_smoke_test.cpp`.

### Coordination boot paths

- **B1** AI server dangling pointers pendant `applyProfile` →
  `aiServer.detach()` sous `stateMutex`, handlers null-check sous
  verrou, `attach()` ré-armé après re-plug.
- **B2** Disques inserted mid-session perdus sur profile switch →
  lit `diskCard->getDiskPath()` / `hdvCard->getImagePath()` au lieu
  du setting persisté.
- **B3** Use-after-free deferred-actions thread → tracké + cancel
  atomic + join avant teardown.
- **B4** Reset RamWorks bank → 0 (MAME `device_reset` parity).
- **B5** RamWorks gate strict sur profil `AppleIIe` (pas //c/c+ qui
  n'ont pas d'aux slot).
