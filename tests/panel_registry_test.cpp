// Panel catalog + registry — pins src/PanelCatalog.h and src/PanelRegistry.*.
//
// The UI used to describe every panel in six places at once: the settings
// load, the settings save, the palette's command list, the palette's dispatch
// table, the menu rows, and the browser build's "hide everything" block. The
// registry exists so there is one. These cases pin the properties that make
// the single list trustworthy — because the failures they describe are not
// hypothetical, they are what the six lists had already produced:
//
//   1. Ids and settings keys are unique. A duplicate id makes one of the two
//      panels unreachable from the palette; a duplicate key makes two panels
//      share one visibility bit, so opening either opens both next launch.
//   2. Every panel persists, except the one that documents why it must not.
//      Seven panels had no key at all — the user opened them and they were
//      gone after a restart, and nothing said whether that was a decision.
//   3. Order follows the CATALOG, not the order things happen to be bound.
//      The menus, the palette and the settings file all iterate the registry,
//      so a bind-order-dependent sequence would reshuffle the user's menus
//      whenever an unrelated binding moved.
//   4. A catalog entry nobody bound is reported. That is a menu row that
//      toggles nothing, and it is silent in every other way.

#include "PanelCatalog.h"
#include "PanelRegistry.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

// ── 1. Catalog invariants ────────────────────────────────────────────────
void testCatalogIsWellFormed()
{
    assert(pom2::kPanelCount > 0);

    std::set<std::string> ids, keys, titles;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        assert(p.id && *p.id);
        assert(p.title && *p.title);
        // The id is what the palette, the settings file and any future key
        // binding name the panel by — it outlives the title, so it has a
        // shape rather than being free text.
        assert(std::strncmp(p.id, "panel.", 6) == 0 && "ids are namespaced");
        assert(ids.insert(p.id).second && "duplicate panel id");
        if (p.settingsKey) {
            assert(*p.settingsKey);
            assert(std::strncmp(p.settingsKey, "show_", 5) == 0);
            assert(keys.insert(p.settingsKey).second && "duplicate settings key");
        }
        // Two rows with the same label in the same menu is a UI bug that
        // compiles: the user cannot tell which window they are opening.
        assert(titles.insert(p.title).second && "duplicate panel title");
    }

    std::printf("[ OK ] catalog: %zu panels, ids/keys/titles unique\n",
                pom2::kPanelCount);
}

// ── 2. Persistence is the rule; not persisting is the exception ──────────
void testEveryPanelPersistsExceptTheDocumentedOne()
{
    std::size_t unpersisted = 0;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog)
        if (!p.settingsKey) ++unpersisted;

    // Exactly one: the Welcome panel, which a first launch with NO ROM opens
    // from the constructor before settings are read — a stored `false` would
    // cancel the greeting. Any other panel arriving without a key is the old
    // bug coming back (opened by the user, gone next launch), so this counts
    // rather than merely allowing.
    assert(unpersisted == 1 &&
           "a panel with no settings key must be a documented decision");
    std::printf("[ OK ] every panel persists but the one that documents why not\n");
}

// ── 3. Binding, lookup, and catalog order ────────────────────────────────
void testBindingAndOrder()
{
    pom2::PanelRegistry reg;
    bool a = false, b = false, c = false;

    // Bind three, deliberately in the WRONG order relative to the catalog.
    const char* last  = pom2::kPanelCatalog[pom2::kPanelCount - 1].id;
    const char* first = pom2::kPanelCatalog[0].id;
    const char* mid   = pom2::kPanelCatalog[pom2::kPanelCount / 2].id;
    reg.bind(last, &c);
    reg.bind(first, &a);
    reg.bind(mid, &b);

    assert(reg.all().size() == 3);
    assert(reg.all()[0].info->id == first && "registry must follow catalog order");
    assert(reg.all()[1].info->id == mid);
    assert(reg.all()[2].info->id == last);

    assert(reg.flag(first) == &a);
    assert(reg.flag("panel.nope") == nullptr);
    assert(reg.find("panel.nope") == nullptr);

    assert(reg.toggle(first) && a);
    assert(reg.toggle(first) && !a);
    assert(!reg.toggle("panel.nope") && "an unknown id must fall through");

    // Re-binding replaces: two rows for one window is the duplication this
    // whole class exists to remove.
    bool a2 = false;
    reg.bind(first, &a2);
    assert(reg.all().size() == 3);
    assert(reg.flag(first) == &a2);

    // A typo'd id creates no binding and is reported, rather than silently
    // producing a panel nothing can open.
    assert(reg.unknownBinds().empty());
    reg.bind("panel.typo", &a);
    assert(reg.all().size() == 3);
    assert(reg.unknownBinds().size() == 1);

    std::printf("[ OK ] bind / find / toggle / catalog order / typo reporting\n");
}

// ── 4. Titles, availability, hiding ──────────────────────────────────────
void testTitleAvailabilityAndHiding()
{
    pom2::PanelRegistry reg;
    bool flag = false;
    const char* id = pom2::kPanelCatalog[0].id;

    reg.bind(id, &flag);
    assert(reg.title(*reg.find(id)) == pom2::kPanelCatalog[0].title);
    assert(reg.available(*reg.find(id)) && "no predicate = always available");
    assert(!reg.find(id)->hidden);

    bool plugged = false;
    reg.bind(id, &flag, [&plugged] { return plugged; },
             [&plugged] { return plugged ? std::string("Card (slot 4)")
                                         : std::string("Card (no card plugged)"); });
    assert(!reg.available(*reg.find(id)));
    assert(reg.title(*reg.find(id)) == "Card (no card plugged)");
    plugged = true;
    assert(reg.available(*reg.find(id)));
    assert(reg.title(*reg.find(id)) == "Card (slot 4)");

    // hidden is not "unavailable": greyed says "plug something in", hidden
    // says "this cannot exist on this platform".
    reg.hideFromUi(id);
    assert(reg.find(id)->hidden);
    assert(reg.available(*reg.find(id)) && "hiding must not change availability");

    std::printf("[ OK ] dynamic titles, availability, hide-from-UI\n");
}

// ── 5. The derived views: persistence and hide-all ───────────────────────
void testDerivedViews()
{
    pom2::PanelRegistry reg;
    bool keyed = true, unkeyed = true;

    // Find one catalog entry with a settings key and one without.
    const pom2::PanelInfo* withKey = nullptr;
    const pom2::PanelInfo* noKey   = nullptr;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        if (p.settingsKey && !withKey) withKey = &p;
        if (!p.settingsKey && !noKey)  noKey   = &p;
    }
    assert(withKey && noKey);
    reg.bind(withKey->id, &keyed);
    reg.bind(noKey->id,   &unkeyed);

    int visited = 0;
    std::string seenKey;
    reg.forEachPersisted([&](const char* key, bool* f) {
        ++visited;
        seenKey = key;
        assert(f == &keyed);
    });
    assert(visited == 1 && seenKey == withKey->settingsKey &&
           "forEachPersisted must skip the keyless panel, not crash on it");

    // hideAll closes everything bound — including panels added after it was
    // written, which is exactly what the 28 hand-written assignments it
    // replaced could not do.
    reg.hideAll();
    assert(!keyed && !unkeyed);

    // Anything in the catalog nobody bound is reported by name.
    const auto missing = reg.unbound();
    assert(missing.size() == pom2::kPanelCount - 2);
    for (const std::string& m : missing)
        assert(m != withKey->id && m != noKey->id);

    std::printf("[ OK ] persistence loop, hide-all, unbound reporting\n");
}

}  // namespace

int main()
{
    testCatalogIsWellFormed();
    testEveryPanelPersistsExceptTheDocumentedOne();
    testBindingAndOrder();
    testTitleAvailabilityAndHiding();
    testDerivedViews();
    std::printf("panel_registry: all assertions passed\n");
    return 0;
}
