#include "VirtualKeyboard.hpp"
#include <sys/mman.h>
#include "../devices/IKeyboard.hpp"
#include "../helpers/time/Time.hpp"
using namespace Hyprutils::OS;

CVirtualKeyboardV1Resource::CVirtualKeyboardV1Resource(SP<CZwpVirtualKeyboardV1> resource_) : resource(resource_) {
    if UNLIKELY (!good())
        return;

    resource->setDestroy([this](CZwpVirtualKeyboardV1* r) {
        releasePressed();
        events.destroy.emit();
        PROTO::virtualKeyboard->destroyResource(this);
    });
    resource->setOnDestroy([this](CZwpVirtualKeyboardV1* r) {
        releasePressed();
        events.destroy.emit();
        PROTO::virtualKeyboard->destroyResource(this);
    });

    resource->setKey([this](CZwpVirtualKeyboardV1* r, uint32_t timeMs, uint32_t key, uint32_t state) {
        if UNLIKELY (!hasKeymap) {
            r->error(ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, "Key event received before a keymap was set");
            return;
        }

        events.key.emit(IKeyboard::SKeyEvent{
            .timeMs  = timeMs,
            .keycode = key,
            .state   = (wl_keyboard_key_state)state,
        });

        const bool CONTAINS = std::find(pressed.begin(), pressed.end(), key) != pressed.end();
        if (state && !CONTAINS)
            pressed.emplace_back(key);
        else if (!state && CONTAINS)
            std::erase(pressed, key);
    });

    resource->setModifiers([this](CZwpVirtualKeyboardV1* r, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
        if UNLIKELY (!hasKeymap) {
            r->error(ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, "Mods event received before a keymap was set");
            return;
        }

        events.modifiers.emit(IKeyboard::SModifiersEvent{
            .depressed = depressed,
            .latched   = latched,
            .locked    = locked,
            .group     = group,
        });
    });

    resource->setKeymap([this](CZwpVirtualKeyboardV1* r, uint32_t fmt, int32_t fd, uint32_t len) {
        auto            xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        CFileDescriptor keymapFd{fd};
        if UNLIKELY (!xkbContext) {
            LOGM(ERR, "xkbContext creation failed");
            r->noMemory();
            return;
        }

        auto keymapData = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, keymapFd.get(), 0);
        if UNLIKELY (keymapData == MAP_FAILED) {
            LOGM(ERR, "keymapData alloc failed");
            xkb_context_unref(xkbContext);
            r->noMemory();
            return;
        }

        auto xkbKeymap = xkb_keymap_new_from_string(xkbContext, (const char*)keymapData, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(keymapData, len);

        if UNLIKELY (!xkbKeymap) {
            LOGM(ERR, "xkbKeymap creation failed");
            xkb_context_unref(xkbContext);
            r->noMemory();
            return;
        }

        events.keymap.emit(IKeyboard::SKeymapEvent{
            .keymap = xkbKeymap,
        });
        hasKeymap = true;

        xkb_keymap_unref(xkbKeymap);
        xkb_context_unref(xkbContext);
    });

    name = "hl-virtual-keyboard";
}

CVirtualKeyboardV1Resource::~CVirtualKeyboardV1Resource() {
    events.destroy.emit();
}

bool CVirtualKeyboardV1Resource::good() {
    return resource->resource();
}

wl_client* CVirtualKeyboardV1Resource::client() {
    return resource->resource() ? resource->client() : nullptr;
}

void CVirtualKeyboardV1Resource::releasePressed() {
    for (auto const& p : pressed) {
        events.key.emit(IKeyboard::SKeyEvent{
            .timeMs  = Time::millis(Time::steadyNow()),
            .keycode = p,
            .state   = WL_KEYBOARD_KEY_STATE_RELEASED,
        });
    }

    pressed.clear();
}

CVirtualKeyboardProtocol::CVirtualKeyboardProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CVirtualKeyboardProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_vManagers.emplace_back(makeUnique<CZwpVirtualKeyboardManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CZwpVirtualKeyboardManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setCreateVirtualKeyboard([this](CZwpVirtualKeyboardManagerV1* pMgr, wl_resource* seat, uint32_t id) { this->onCreateKeeb(pMgr, seat, id); });
}

void CVirtualKeyboardProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_vManagers, [&](const auto& other) { return other->resource() == res; });
}

void CVirtualKeyboardProtocol::destroyResource(CVirtualKeyboardV1Resource* keeb) {
    std::erase_if(m_vKeyboards, [&](const auto& other) { return other.get() == keeb; });
}

void CVirtualKeyboardProtocol::onCreateKeeb(CZwpVirtualKeyboardManagerV1* pMgr, wl_resource* seat, uint32_t id) {

    const auto RESOURCE = m_vKeyboards.emplace_back(makeShared<CVirtualKeyboardV1Resource>(makeShared<CZwpVirtualKeyboardV1>(pMgr->client(), pMgr->version(), id)));

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_vKeyboards.pop_back();
        return;
    }

    LOGM(LOG, "New VKeyboard at id {}", id);

    events.newKeyboard.emit(RESOURCE);
}
