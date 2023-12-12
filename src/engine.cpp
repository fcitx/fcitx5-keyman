/*
 * SPDX-FileCopyrightText: 2018 SIL International
 * SPDX-FileCopyrightText: 2021~2021 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#include "engine.h"
#include <fcntl.h>
#include <algorithm>
#include <fcitx-utils/inputbuffer.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/utf8.h>
#include <keyman_core_api.h>
#include "kmpdata.h"
#include "kmpmetadata.h"

#define MAXCONTEXT_ITEMS 128
#define KEYMAN_BACKSPACE 14
#define KEYMAN_BACKSPACE_KEYSYM 0xff08
#define KEYMAN_LCTRL 29
#define KEYMAN_LALT 56
#define KEYMAN_RCTRL 97
#define KEYMAN_RALT 100

FCITX_DEFINE_LOG_CATEGORY(keyman, "keyman");
#define FCITX_KEYMAN_DEBUG() FCITX_LOGC(::keyman, Debug)
#define FCITX_KEYMAN_ERROR() FCITX_LOGC(::keyman, Error)

namespace fcitx {

namespace {

std::vector<char16_t> utf8ToUTF16(std::string_view str) {
    if (!utf8::validate(str)) {
        return {};
    }
    std::vector<char16_t> result;
    for (const auto ucs4 : utf8::MakeUTF8CharRange(str)) {
        if (ucs4 < 0x10000) {
            result.push_back(static_cast<char16_t>(ucs4));
        } else if (ucs4 < 0x110000) {
            result.push_back(0xD800 | (((ucs4 - 0x10000) >> 10) & 0x3ff));
            result.push_back(0xDC00 | (ucs4 & 0x3ff));
        } else {
            return {};
        }
    }
    result.push_back(0);
    return result;
}

template <typename Iter>
std::string utf16ToUTF8(Iter start, Iter end) {
    std::string result;
    while (start != end) {
        uint32_t ucs4 = 0;
        if (*start < 0xD800 || *start > 0xDFFF) {
            ucs4 = *start;
            start = std::next(start);
        } else if (0xD800 <= *start && *start <= 0xDBFF) {
            if (std::next(start) == end) {
                return {};
            }
            auto cur = *start;
            auto next = std::next(start);
            if (0xDC00 <= *next && *next <= 0xDFFF) {
                /* We have a valid surrogate pair.  */
                ucs4 = (((cur & 0x3FF) << 10) | (*next & 0x3FF)) + (1 << 16);
            } else {
                return {};
            }
            start = std::next(next);
        } else if (0xDC00 <= *start && *start <= 0xDFFF) {
            return {};
        }
        result.append(utf8::UCS4ToUTF8(ucs4));
    }
    return result;
}

std::string get_current_context_text(km_core_context *context) {
    std::string result;
    UniqueCPtr<km_core_context_item, km_core_context_items_dispose>
        context_items_ptr;
    km_core_status status = KM_CORE_STATUS_OK;
    {
        km_core_context_item *context_items = nullptr;
        status = km_core_context_get(context, &context_items);
        context_items_ptr.reset(context_items);
    }
    if (status == KM_CORE_STATUS_OK) {
        size_t buf_size = 0;
        km_core_context_items_to_utf8(context_items_ptr.get(), nullptr,
                                      &buf_size);
        if (buf_size) {
            std::vector<char> buf;
            buf.resize(buf_size + 1);
            km_core_context_items_to_utf8(context_items_ptr.get(), buf.data(),
                                          &buf_size);
            return {buf.data()};
        }
    }
    return "";
}

std::set<std::string> listKeymapDirs() {
    // Locate all directory under $XDG_DATA/keyman
    std::set<std::string> keymapDirs;
    StandardPath::global().scanFiles(
        StandardPath::Type::Data, "keyman",
        [&keymapDirs](const std::string &path, const std::string &dir, bool) {
            if (fs::isdir(stringutils::joinPath(dir, path))) {
                keymapDirs.insert(path);
            }
            return true;
        });
    return keymapDirs;
}

} // namespace

class KeymanState : public InputContextProperty {
public:
    KeymanState(KeymanKeyboardData *keyboard, InputContext *ic)
        : keyboard_(keyboard), ic_(ic) {
        std::vector<km_core_option_item> keyboard_opts;

        keyboard_opts.emplace_back();
        keyboard_opts.back().scope = KM_CORE_OPT_ENVIRONMENT;
        const auto platform = utf8ToUTF16("platform");
        keyboard_opts.back().key = platform.data();
        const auto platformValue = utf8ToUTF16("linux desktop hardware native");
        keyboard_opts.back().value = platformValue.data();

        keyboard_opts.emplace_back();
        keyboard_opts.back().scope = KM_CORE_OPT_ENVIRONMENT;
        const auto baseLayout = utf8ToUTF16("baseLayout");
        keyboard_opts.back().key = baseLayout.data();
        const auto baseLayoutValue = utf8ToUTF16("kbdus.dll");
        keyboard_opts.back().value = baseLayoutValue.data();

        keyboard_opts.emplace_back();
        keyboard_opts.back().scope = KM_CORE_OPT_ENVIRONMENT;
        const auto baseLayoutAlt = utf8ToUTF16("baseLayoutAlt");
        keyboard_opts.back().key = baseLayoutAlt.data();
        const auto baseLayoutAltValue = utf8ToUTF16("en-US");
        keyboard_opts.back().value = baseLayoutAltValue.data();

        keyboard_opts.emplace_back();
        keyboard_opts.back().scope = 0;
        keyboard_opts.back().key = nullptr;
        keyboard_opts.back().value = nullptr;
        km_core_status status_state = km_core_state_create(
            keyboard_->kbpKeyboard(), keyboard_opts.data(), &state);
        if (status_state != KM_CORE_STATUS_OK) {
            FCITX_KEYMAN_ERROR() << "problem creating km_core_state for "
                                 << keyboard_->metadata().id;
            return;
        };
        resetContext();
    }

    // Update context from surrounding if possible.
    void resetContext() {
        if (ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
            ic_->surroundingText().isValid()) {
            auto text = ic_->surroundingText().text();
            auto context_pos = std::min(ic_->surroundingText().anchor(),
                                        ic_->surroundingText().cursor());
            auto context_start = context_pos > MAXCONTEXT_ITEMS
                                     ? context_pos - MAXCONTEXT_ITEMS
                                     : 0;

            auto startIter = utf8::nextNChar(text.begin(), context_start);
            auto endIter =
                utf8::nextNChar(startIter, context_pos - context_start);
            std::string new_context(startIter, endIter);
            auto utf16Context = utf8ToUTF16(new_context);
            km_core_state_context_set_if_needed(
                state, reinterpret_cast<km_core_cp *>(utf16Context.data()));
            FCITX_KEYMAN_DEBUG()
                << "Set context from application: " << new_context;
        } else {
            km_core_cp empty[1] = {0};
            FCITX_KEYMAN_DEBUG() << "Clear context";
            km_core_state_context_set_if_needed(state, empty);
        }
    }

    void reset() {
        lctrl_pressed = false;
        rctrl_pressed = false;
        lalt_pressed = false;
        ralt_pressed = false;
    }

    auto *keyboard() { return keyboard_; }

    km_core_state *state = nullptr;
    bool lctrl_pressed = false;
    bool rctrl_pressed = false;
    bool lalt_pressed = false;
    bool ralt_pressed = false;

private:
    KeymanKeyboardData *keyboard_;
    InputContext *ic_;
};

KeymanEngine::KeymanEngine(Instance *instance) : instance_(instance) {
    updateHandler_ = instance_->watchEvent(
        EventType::CheckUpdate, EventWatcherPhase::Default,
        [this](Event &event) {
            auto &update = static_cast<CheckUpdateEvent &>(event);
            // Locate all directory under $XDG_DATA/keyman
            std::set<std::string> keymapDirs = listKeymapDirs();
            FCITX_KEYMAN_DEBUG() << "Keyman directories: " << keymapDirs;
            std::unordered_map<std::string, std::unique_ptr<KeymanKeyboard>>
                keyboards;
            for (const auto &keymapDir : keymapDirs) {
                auto kmpJsonFiles = StandardPath::global().locateAll(
                    StandardPath::Type::Data,
                    stringutils::joinPath("keyman", keymapDir, "kmp.json"));
                for (const auto &kmpJsonFile : kmpJsonFiles) {
                    if (timestamp_ < fs::modifiedTime(kmpJsonFile)) {
                        update.setHasUpdate();
                        return;
                    }
                }
            }
        });
}

std::vector<InputMethodEntry> KeymanEngine::listInputMethods() {
    // Locate all directory under $XDG_DATA/keyman
    std::set<std::string> keymapDirs = listKeymapDirs();
    FCITX_KEYMAN_DEBUG() << "Keyman directories: " << keymapDirs;
    std::unordered_map<std::string, std::unique_ptr<KeymanKeyboard>> keyboards;
    for (const auto &keymapDir : keymapDirs) {
        auto kmpJsonFiles = StandardPath::global().openAll(
            StandardPath::Type::Data,
            stringutils::joinPath("keyman", keymapDir, "kmp.json"), O_RDONLY);
        for (const auto &kmpJsonFile : kmpJsonFiles) {
            try {
                timestamp_ =
                    std::max(timestamp_, fs::modifiedTime(kmpJsonFile.path()));
                KmpMetadata metadata(kmpJsonFile.fd());
                for (const auto &[id, keyboard] : metadata.keyboards()) {
                    if (auto iter = keyboards.find(id);
                        iter != keyboards.end() &&
                        iter->second->version < keyboard.version) {
                        continue;
                    }
                    keyboards[id] = std::make_unique<KeymanKeyboard>(
                        instance_, keyboard, metadata,
                        fs::dirName(kmpJsonFile.path()));
                }
            } catch (...) {
            }
        }
    }
    std::vector<InputMethodEntry> result;
    for (auto &[id, keyboard] : keyboards) {
        std::string icon = "km-config";
        // Check if icon file exists, otherwise fallback to keyman's icon.
        for (auto *suffix : {".bmp.png", ".icon.png"}) {
            auto path = stringutils::joinPath(keyboard->baseDir,
                                              stringutils::concat(id, suffix));
            if (fs::isreg(path)) {
                icon = std::move(path);
                break;
            }
        }

        result.emplace_back(stringutils::concat("keyman:", id),
                            stringutils::concat(keyboard->name, " (Keyman)"),
                            keyboard->language, "keyman");
        result.back().setIcon(icon).setConfigurable(true).setUserData(
            std::move(keyboard));
    }
    return result;
}

FCITX_ADDON_FACTORY(fcitx::KeymanEngineFactory);

} // namespace fcitx

void fcitx::KeymanKeyboardData::load() {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    auto kmxPath = stringutils::joinPath(
        metadata_.baseDir, stringutils::concat(metadata_.id, ".kmx"));
    auto ldmlFile = stringutils::joinPath(
        metadata_.baseDir, stringutils::concat(metadata_.id, ".ldml"));
    if (!fs::isreg(ldmlFile)) {
        ldmlFile.clear();
    }
    ldmlFile_ = ldmlFile;
    if (!fs::isreg(kmxPath)) {
        FCITX_KEYMAN_ERROR() << "Failed to find kmx file. " << metadata_.id;
        return;
    }

    km_core_status status_keyboard =
        km_core_keyboard_load(kmxPath.data(), &keyboard_);

    if (status_keyboard != KM_CORE_STATUS_OK) {
        FCITX_KEYMAN_ERROR()
            << "problem creating km_core_keyboard" << metadata_.id;
        return;
    }

    instance_->inputContextManager().registerProperty(
        stringutils::concat("keymanState", metadata_.id), &factory_);

    config_ = RawConfig();
    readAsIni(config_, stringutils::concat("keyman/", metadata_.id, ".conf"));

    FCITX_KEYMAN_DEBUG() << config_;
}

void fcitx::KeymanKeyboardData::setOption(const km_core_cp *key,
                                          const km_core_cp *value) {
    auto keyEnd = key;
    while (*keyEnd) {
        ++keyEnd;
    }
    auto valueEnd = value;
    while (*valueEnd) {
        ++valueEnd;
    }

    auto utf8Key = utf16ToUTF8(key, keyEnd);
    auto utf8Value = utf16ToUTF8(value, valueEnd);

    if (!utf8Key.empty()) {
        config_.setValueByPath(utf8Key, utf8Value);
        safeSaveAsIni(config_,
                      stringutils::concat("keyman/", metadata_.id, ".conf"));
    }
}

fcitx::KeymanKeyboardData::KeymanKeyboardData(
    Instance *instance, const fcitx::KeymanKeyboard &metadata)
    : instance_(instance), metadata_(metadata),
      factory_(
          [this](InputContext &ic) { return new KeymanState(this, &ic); }) {}

fcitx::KeymanKeyboardData::~KeymanKeyboardData() { factory_.unregister(); }

void fcitx::KeymanEngine::activate(const fcitx::InputMethodEntry &entry,
                                   fcitx::InputContextEvent &event) {
    auto data = static_cast<const KeymanKeyboard *>(entry.userData());
    data->load();
    auto keyman = state(entry, *event.inputContext());
    if (!keyman) {
        return;
    }
    keyman->resetContext();
}

void fcitx::KeymanEngine::keyEvent(const fcitx::InputMethodEntry &entry,
                                   fcitx::KeyEvent &keyEvent) {
    auto ic = keyEvent.inputContext();
    auto keyman = state(entry, *ic);
    if (!keyman) {
        return;
    }
    auto keycode = keyEvent.key().code() - 8;
    auto state = keyEvent.rawKey().states();
    switch (keycode) {
    case KEYMAN_LCTRL:
        keyman->lctrl_pressed = !keyEvent.isRelease();
        break;
    case KEYMAN_RCTRL:
        keyman->rctrl_pressed = !keyEvent.isRelease();
        break;
    case KEYMAN_LALT:
        keyman->lalt_pressed = !keyEvent.isRelease();
        break;
    case KEYMAN_RALT:
        keyman->ralt_pressed = !keyEvent.isRelease();
        break;
    default:
        break;
    }

    if (keycode < 0 || keycode > 255) {
        return;
    }

    if (keycode_to_vk[keycode] == 0) {
        // key that we don't handles
        if (keyEvent.key().isCursorMove()) {
            km_core_context_clear(km_core_state_context(keyman->state));
            keyman->resetContext();
        }
        return;
    }

    // keyman modifiers are different from X11
    uint16_t km_mod_state = 0;
    if (state.test(KeyState::Shift)) {
        km_mod_state |= KM_CORE_MODIFIER_SHIFT;
    }
    if (state.test(KeyState::Mod5)) {
        km_mod_state |= KM_CORE_MODIFIER_RALT;
        FCITX_KEYMAN_DEBUG() << "modstate KM_CORE_MODIFIER_RALT from Mod5";
    }
    if (state.test(KeyState::Mod1)) {
        if (keyman->ralt_pressed) {
            km_mod_state |= KM_CORE_MODIFIER_RALT;
            FCITX_KEYMAN_DEBUG()
                << "modstate KM_CORE_MODIFIER_RALT from ralt_pressed";
        }
        if (keyman->lalt_pressed) {
            km_mod_state |= KM_CORE_MODIFIER_LALT;
            FCITX_KEYMAN_DEBUG()
                << "modstate KM_CORE_MODIFIER_LALT from lalt_pressed";
        }
    }
    if (state.test(KeyState::Ctrl)) {
        if (keyman->rctrl_pressed) {
            km_mod_state |= KM_CORE_MODIFIER_RCTRL;
            FCITX_KEYMAN_DEBUG()
                << "modstate KM_CORE_MODIFIER_RCTRL from rctrl_pressed";
        }
        if (keyman->lctrl_pressed) {
            km_mod_state |= KM_CORE_MODIFIER_LCTRL;
            FCITX_KEYMAN_DEBUG()
                << "modstate KM_CORE_MODIFIER_LCTRL from lctrl_pressed";
        }
    }

    if (ic->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
        keyman->resetContext();
    }

    auto context = km_core_state_context(keyman->state);
    FCITX_KEYMAN_DEBUG() << "before process key event context: "
                         << get_current_context_text(context);
    FCITX_KEYMAN_DEBUG() << "km_mod_state=" << km_mod_state;
    km_core_process_event(keyman->state, keycode_to_vk[keycode], km_mod_state,
                          !keyEvent.isRelease(), 0);
    FCITX_KEYMAN_DEBUG() << "after process key event context: "
                         << get_current_context_text(context);

    UniqueCPtr<km_core_actions, &km_core_actions_dispose> actions(
        km_core_state_get_actions(keyman->state));

    auto numOfDelete = actions->code_points_to_delete;
    FCITX_KEYMAN_DEBUG() << "BACK action " << numOfDelete;

    if (numOfDelete > 0) {
        if (numOfDelete == 1 && keyEvent.key().check(FcitxKey_BackSpace)) {
            actions->emit_keystroke = KM_CORE_TRUE;
        } else if (ic->capabilityFlags().test(CapabilityFlag::SurroundingText)) {
            ic->deleteSurroundingText(-numOfDelete, numOfDelete);
            FCITX_KEYMAN_DEBUG()
                << "deleting surrounding text " << numOfDelete << " char(s)";
        } else {
            FCITX_KEYMAN_DEBUG() << "forwarding backspace with reset context";
            km_core_context_item *context_items;
            km_core_context_get(km_core_state_context(keyman->state),
                                &context_items);
            keyman->resetContext();
            while (numOfDelete) {
                ic->forwardKey(Key(FcitxKey_BackSpace));
                numOfDelete -= 1;
            }
            km_core_context_set(km_core_state_context(keyman->state),
                                context_items);
            km_core_context_items_dispose(context_items);
        }
    }

    std::string output;
    for (size_t i = 0; actions->output[i]; i++) {
        output.append(utf8::UCS4ToUTF8(actions->output[i]));
    }

    if (actions->do_alert) {
        FCITX_KEYMAN_DEBUG() << "ALERT action";
    }

    if (!output.empty()) {
        ic->commitString(output);
    }
    if (actions->emit_keystroke) {
        FCITX_KEYMAN_DEBUG() << "EMIT_KEYSTROKE action";
    } else {
        keyEvent.filterAndAccept();
    }

    FCITX_KEYMAN_DEBUG() << "PERSIST_OPT action";
    instance_->inputContextManager().foreach(
        [this, &entry, &actions](InputContext *ic) {
            if (auto keyman = this->state(entry, *ic)) {
                auto event_status = km_core_state_options_update(
                    keyman->state, actions->persist_options);
                if (event_status != KM_CORE_STATUS_OK) {
                    FCITX_KEYMAN_DEBUG()
                        << "problem saving option for km_core_keyboard";
                }
            }
            return true;
        });

    for (size_t i = 0; actions->persist_options[i].scope; i++) {
        if (actions->persist_options[i].key &&
            actions->persist_options[i].value) {
            // Put the keyboard option into config
            FCITX_KEYMAN_DEBUG() << "Saving keyboard option to Config";
            // Load the current keyboard options from DConf
            keyman->keyboard()->setOption(actions->persist_options[i].key,
                                          actions->persist_options[i].value);
        }
    }

    context = km_core_state_context(keyman->state);
    // FIXME: remove this once invalidate issue is resolved:
    // https://github.com/keymanapp/keyman/issues/10182
    size_t num;
    auto actionItems = km_core_state_action_items(keyman->state, &num);
    const bool hasInvalidate = std::any_of(
        actionItems, actionItems + num, [](const km_core_action_item &item) {
            return item.type == KM_CORE_IT_INVALIDATE_CONTEXT;
        });
    if (hasInvalidate) {
        FCITX_KEYMAN_DEBUG() << "invalidate context";
        km_core_context_clear(km_core_state_context(keyman->state));
    }
    FCITX_KEYMAN_DEBUG() << "after processing all actions";
}

void fcitx::KeymanEngine::reset(const fcitx::InputMethodEntry &entry,
                                fcitx::InputContextEvent &event) {
    auto ic = event.inputContext();
    auto keyman = state(entry, *ic);
    if (!keyman) {
        return;
    }
    keyman->resetContext();
    keyman->reset();
}

fcitx::KeymanState *
fcitx::KeymanEngine::state(const fcitx::InputMethodEntry &entry,
                           fcitx::InputContext &ic) {

    auto userData = static_cast<const KeymanKeyboard *>(entry.userData());
    auto &data = userData->data();
    // Check if data is ready.
    if (!data.kbpKeyboard() || !data.factory().registered()) {
        return nullptr;
    }
    auto keyman = ic.propertyFor(&data.factory());
    if (!keyman->state) {
        return nullptr;
    }
    return keyman;
}

std::string fcitx::KeymanEngine::subMode(const fcitx::InputMethodEntry &entry,
                                         fcitx::InputContext &ic) {
    auto keyman = state(entry, ic);
    if (!keyman) {
        return _("Not available");
    }
    return "";
}
