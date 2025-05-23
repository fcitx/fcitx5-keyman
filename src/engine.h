/*
 * SPDX-FileCopyrightText: 2021~2021 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#ifndef _FCITX5_KEYMAN_ENGINE_H_
#define _FCITX5_KEYMAN_ENGINE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/library.h>
#include <fcitx-utils/log.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/instance.h>
#include <fcitx/menu.h>
#include <keyman_core_api.h>
#include <km_types.h>
#include "kmpmetadata.h"

namespace fcitx {

class KeymanState;
class KeymanKeyboard;

FCITX_CONFIGURATION(KeymanConfig,
                    ExternalOption config{this, "Configuration",
                                          _("Configuration"), "km-config"};);

class KeymanKeyboardData {
public:
    KeymanKeyboardData(Instance *instance, const KeymanKeyboard &metadata);
    ~KeymanKeyboardData();

    void load();
    const auto &metadata() const { return metadata_; }
    auto *kbpKeyboard() const { return keyboard_; }
    const auto &factory() const { return factory_; }
    void setOption(const km_core_cu *key, const km_core_cu *value);

private:
    Instance *instance_;
    bool loaded_ = false;
    std::string ldmlFile_;
    const KeymanKeyboard &metadata_;
    km_core_keyboard *keyboard_ = nullptr;
    FactoryFor<KeymanState> factory_;
    RawConfig config_;
};

class KeymanKeyboard : public InputMethodEntryUserData {
public:
    KeymanKeyboard(Instance *instance, const KmpKeyboardMetadata &keyboard,
                   const KmpMetadata &metadata, std::filesystem::path dir)
        : id(keyboard.id), version(keyboard.version), baseDir(std::move(dir)),
          name(keyboard.name),
          language(keyboard.languages.empty() ? ""
                                              : keyboard.languages[0].first),
          readme(metadata.readmeFile()), graphic(metadata.graphicFile()),
          data_(instance, *this) {}
    const std::string id;
    const std::string version;
    const std::filesystem::path baseDir;
    const std::string name;
    const std::string language;
    const std::string readme;
    const std::string graphic;

    void load() const { data_.load(); }
    KeymanKeyboardData &data() const { return data_; }

private:
    mutable KeymanKeyboardData data_;
};

class KeymanEngine final : public InputMethodEngineV2 {
public:
    KeymanEngine(Instance *instance);
    Instance *instance() { return instance_; }
    void activate(const fcitx::InputMethodEntry & /*entry*/,
                  fcitx::InputContextEvent & /*event*/) override;
    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;
    std::vector<InputMethodEntry> listInputMethods() override;
    void reset(const fcitx::InputMethodEntry & /*entry*/,
               fcitx::InputContextEvent & /*event*/) override;
    const fcitx::Configuration *getConfig() const override { return &config_; }
    std::string subMode(const fcitx::InputMethodEntry & /*entry*/,
                        fcitx::InputContext & /*inputContext*/) override;

private:
    FCITX_ADDON_DEPENDENCY_LOADER(dbus, instance_->addonManager());
    KeymanState *state(const fcitx::InputMethodEntry &entry,
                       fcitx::InputContext &ic);

    Instance *instance_;
    KeymanConfig config_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> updateHandler_;
    int64_t timestamp_ = 0;
    bool emit_keystroke = false;
};

class KeymanEngineFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        registerDomain("fcitx5-keyman", FCITX_INSTALL_LOCALEDIR);
        return new KeymanEngine(manager->instance());
    }
};
} // namespace fcitx

#endif // _FCITX5_KEYMAN_ENGINE_H_
