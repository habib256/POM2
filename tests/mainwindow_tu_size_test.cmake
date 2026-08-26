if(NOT DEFINED POM2_SOURCE_DIR)
    message(FATAL_ERROR "POM2_SOURCE_DIR is required")
endif()

include("${POM2_SOURCE_DIR}/cmake/Pom2Architecture.cmake")
pom2_enforce_mainwindow_line_limit("${POM2_SOURCE_DIR}" 3000)

# SlotBus is the storage source of truth. Pin the coordinator extraction so a
# future convenience edit cannot quietly rebuild the old lifetime hazard in
# MainWindow's long-lived state.
file(READ "${POM2_SOURCE_DIR}/src/MainWindow.h" mainwindow_header)
foreach(forbidden IN ITEMS
        "std::vector<DiskIICard\\*>[ \t]+diskCards"
        "DiskIICard\\*[ \t]+diskCard[ \t]*="
        "ProDOSHardDiskCard\\*[ \t]+hdvCard[ \t]*="
        "CffaCard\\*[ \t]+cffaCard[ \t]*="
        "SmartPortCard\\*[ \t]+smartPortCard[ \t]*=")
    if(mainwindow_header MATCHES "${forbidden}")
        message(FATAL_ERROR
            "MainWindow reintroduced a retained storage-card alias: ${forbidden}")
    endif()
endforeach()
if(mainwindow_header MATCHES "hdv(Path|Status)")
    message(FATAL_ERROR
        "MainWindow reintroduced a cached HDV path/status instead of a live snapshot")
endif()

# Audio cards are likewise discovered by AudioCoordinator under lock. Retained
# aliases would make panels race slot rebuilds and hide coexisting variants.
foreach(forbidden IN ITEMS
        "MockingboardCard[ \t]*\\*[ \t]+mockingboardCard[ \t]*="
        "PhasorCard[ \t]*\\*[ \t]+phasorCard[ \t]*="
        "EchoPlusCard[ \t]*\\*[ \t]+echoPlusCard[ \t]*="
        "EchoPlusTMS5220Card[ \t]*\\*[ \t]+echoPlusTmsCard[ \t]*=")
    if(mainwindow_header MATCHES "${forbidden}")
        message(FATAL_ERROR
            "MainWindow reintroduced a retained audio-card alias: ${forbidden}")
    endif()
endforeach()

# Mouse and clock topology also belongs to SlotBus. MouseCoordinator routes
# host input while holding the machine lock; DevicePanelCoordinator snapshots
# ClockCard status. MainWindow must not cache either concrete device.
foreach(forbidden IN ITEMS
        "ClockCard[ \t]*\\*[ \t]+clockCard[ \t]*="
        "MouseCard[ \t]*\\*[ \t]+mouseCard[ \t]*="
        "MouseCardAppleWin[ \t]*\\*[ \t]+mouseAwCard[ \t]*=")
    if(mainwindow_header MATCHES "${forbidden}")
        message(FATAL_ERROR
            "MainWindow reintroduced a retained mouse/clock alias: ${forbidden}")
    endif()
endforeach()

# Slot configuration has three deliberately separate values:
# SlotConfigurationCoordinator::effectivePlan(), its staged draft(), and a
# live snapshot copied from SlotBus. Keep the old ambiguous map/API out of all
# MainWindow translation units so construction failures and session-only cards
# cannot silently rewrite persistent intent again.
file(GLOB mainwindow_sources
    "${POM2_SOURCE_DIR}/src/MainWindow.h"
    "${POM2_SOURCE_DIR}/src/MainWindow*.cpp")
foreach(source IN LISTS mainwindow_sources)
    file(READ "${source}" mainwindow_source)
    if(mainwindow_source MATCHES "slotCoordinator_->cards\\(" OR
       mainwindow_source MATCHES "slotCards")
        message(FATAL_ERROR
            "${source} reintroduced the ambiguous slotCards configuration map")
    endif()
    if(mainwindow_source MATCHES [=[slotBus\(\)\.clear\(]=])
        message(FATAL_ERROR
            "${source} bypasses SlotRebuildCoordinator to clear SlotBus")
    endif()
endforeach()

# Media which survive a profile/slot rebuild are a typed StorageCoordinator
# value. Keep the three former ad-hoc arrays out of MainWindow: besides
# duplicating policy, their path references were easy to read outside the
# machine lock and their CFFA/Disk II empty-state rules had drifted apart.
file(READ "${POM2_SOURCE_DIR}/src/MainWindow_Slots.cpp" mainwindow_slots)
foreach(forbidden IN ITEMS
        "savedDiskPaths"
        "savedHdvPath"
        "savedCffaPaths")
    if(mainwindow_slots MATCHES "${forbidden}")
        message(FATAL_ERROR
            "MainWindow reintroduced an ad-hoc storage rebuild snapshot: ${forbidden}")
    endif()
endforeach()
foreach(required IN ITEMS
        "captureRebuildSnapshot"
        "persistRebuildSettings"
        "mountDiskII"
        "ejectDiskII"
        "mountMediaBay"
        "ejectMediaBay"
        "setMediaBayWriteBack"
        "restoreSlotMediaFromSettings"
        "restoreRebuildSnapshot")
    if(NOT mainwindow_slots MATCHES "${required}")
        message(FATAL_ERROR
            "MainWindow bypasses StorageCoordinator rebuild policy: ${required}")
    endif()
endforeach()

# Immediate media mutation belongs to StorageCoordinator too. MainWindow may
# snapshot concrete cards for rendering, but mount/eject/write-back commands
# must resolve their target by slot under the coordinator-owned state lock.
foreach(source IN LISTS mainwindow_sources)
    file(READ "${source}" mainwindow_source)
    foreach(forbidden IN ITEMS
            "->insertDisk\\("
            "->ejectDisk\\("
            "->ejectImage\\("
            "->loadImageFromBytes\\("
            "->mountBay\\("
            "->ejectBay\\("
            "->setBayWriteBack\\("
            "->setBayType\\("
            "->setUnit\\("
            "->loadImage\\("
            "->eject\\(\\)"
            "->setWriteBackEnabled\\("
            "controller->mount35\\("
            "controller->eject35\\("
            "hdvCard->loadImage\\("
            "dev->loadImage\\(")
        if(mainwindow_source MATCHES "${forbidden}")
            message(FATAL_ERROR
                "${source} bypasses StorageCoordinator media commands: ${forbidden}")
        endif()
    endforeach()
endforeach()
file(READ "${POM2_SOURCE_DIR}/src/MainWindow_Media.cpp" mainwindow_media)
foreach(required IN ITEMS
        "captureDisk35"
        "mountDisk35"
        "ejectDisk35"
        "setDisk35WriteBack"
        "convertDisk35WozToPo"
        "mountHdv")
    if(NOT mainwindow_media MATCHES "${required}")
        message(FATAL_ERROR
            "MainWindow bypasses StorageCoordinator routed media policy: ${required}")
    endif()
endforeach()
if(mainwindow_media MATCHES "routeMount(35|Hdv)")
    message(FATAL_ERROR
        "MainWindow reintroduced local 3.5-inch/HDV routing policy")
endif()

# Explicit boot intent may add a controller for this session, but slot choice,
# profile policy, factory construction and session-only tracking belong to the
# additive SlotProvisioningCoordinator rather than MainWindow.
foreach(source IN LISTS mainwindow_sources)
    file(READ "${source}" mainwindow_source)
    foreach(forbidden IN ITEMS
            "ensureHdvCardForBoot"
            "ensureSmartPortCardForBoot"
            "findFreeSlot"
            "markAutoProvisioned"
            "autoProvisioned(Hdv|SmartPort)Slot"
            "clearAutoProvisioned"
            "make_unique<ProDOSHardDiskCard>"
            "make_unique<pom2::SmartPortCard>")
        if(mainwindow_source MATCHES "${forbidden}")
            message(FATAL_ERROR
                "${source} bypasses SlotProvisioningCoordinator: ${forbidden}")
        endif()
    endforeach()
endforeach()
foreach(required IN ITEMS
        "ensureHdvBootTarget"
        "ensureSmartPortBootTarget")
    if(NOT mainwindow_media MATCHES "${required}")
        message(FATAL_ERROR
            "MainWindow boot path bypasses SlotProvisioningCoordinator: ${required}")
    endif()
endforeach()
file(READ "${POM2_SOURCE_DIR}/src/MainWindow.cpp" mainwindow_composer)
foreach(required IN ITEMS
        "isSessionOnlySlot"
        "resetSessionTracking")
    if(NOT mainwindow_composer MATCHES "${required}")
        message(FATAL_ERROR
            "MainWindow lifecycle bypasses SlotProvisioningCoordinator: ${required}")
    endif()
endforeach()

# Factory-owned construction policy belongs to SlotCardFactory. MainWindow is
# still the composition root for runtime wiring, but must not resume probing
# resources or selecting fallback implementations for these configured cards.
if(NOT mainwindow_composer MATCHES "persistSessionSettings")
    message(FATAL_ERROR
        "MainWindow shutdown bypasses StorageCoordinator persistence policy")
endif()
if(NOT mainwindow_composer MATCHES "restoreMediaFromSettings")
    message(FATAL_ERROR
        "MainWindow construction bypasses StorageCoordinator media restore policy")
endif()
foreach(forbidden IN ITEMS
        "getString\\(\"disk_path"
        "getBool\\(\"disk_writeback"
        "getString\\(\"hdv_path"
        "getBool\\(\"hdv_writeback"
        "getString\\(\"cffa_slot"
        "getBool\\(\"cffa_slot"
        "getString\\(\"smartport_slot"
        "getBool\\(\"smartport_slot")
    if(mainwindow_composer MATCHES "${forbidden}")
        message(FATAL_ERROR
            "MainWindow construction reintroduced storage settings policy: ${forbidden}")
    endif()
endforeach()
foreach(forbidden IN ITEMS
        "make_unique<DiskIICard>"
        "make_unique<pom2::CffaCard>"
        "make_unique<GrapplerCard>"
        "make_unique<MouseCard>"
        "make_unique<MouseCardAppleWin>")
    if(mainwindow_composer MATCHES "${forbidden}")
        message(FATAL_ERROR
            "MainWindow reintroduced SlotCardFactory policy: ${forbidden}")
    endif()
endforeach()

file(READ "${POM2_SOURCE_DIR}/src/AiControlServer.h" ai_server_header)
if(ai_server_header MATCHES "(disk6_|hdv5_)")
    message(FATAL_ERROR
        "AiControlServer must resolve slot cards per request, not retain aliases")
endif()
