/*
 * SPDX-FileCopyrightText: 2021~2021 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#include "kmpmetadata.h"
#include <cstddef>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <fcitx-utils/fdstreambuf.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/stringutils.h>
#include <nlohmann/json.hpp>

namespace fcitx {

using json = nlohmann::json;

std::string readStringValue(const json &object, const char *field,
                            std::string_view defaultValue = "") {
    return object.value(field, std::string{defaultValue});
}

std::string readDescriptionValue(const json &object, const char *field,
                                 std::string_view defaultValue = "") {
    if (object.contains(field) && object[field].is_object()) {
        return readStringValue(object[field], "description", defaultValue);
    }
    return std::string{defaultValue};
}

KmpMetadata::KmpMetadata(int fd) {
    IFDStreamBuf buf(fd);
    std::istream in(&buf);
    json obj;

    try {
        in >> obj;
    } catch (...) {
        throw std::runtime_error("Failed to parse kmp.json");
    }

    if (obj.contains("system") && obj["system"].is_object()) {
        const auto &kmpSystem = obj["system"];
        keymanDeveloperVersion_ =
            readStringValue(kmpSystem, "keymanDeveloperVersion");
        fileVersion_ = readStringValue(kmpSystem, "fileVersion");
    }

    if (obj.contains("info") && obj["info"].is_object()) {
        const auto &kmpInfo = obj["info"];
        name_ = readDescriptionValue(kmpInfo, "name");
        version_ = readDescriptionValue(kmpInfo, "version");
        copyright_ = readDescriptionValue(kmpInfo, "copyright");
        author_ = readDescriptionValue(kmpInfo, "author");
        website_ = readDescriptionValue(kmpInfo, "website");
    }

    if (obj.contains("files") && obj["files"].is_array()) {
        for (const auto &file : obj["files"]) {
            auto name = readStringValue(file, "name");
            auto description = readStringValue(file, "description");
            if (!name.empty()) {
                files_[name] = description;
            }
        }
    }

    if (obj.contains("options") && obj["options"].is_object()) {
        const auto &kmpOptions = obj["options"];
        readmeFile_ = readStringValue(kmpOptions, "readmeFile");
        graphicFile_ = readStringValue(kmpOptions, "graphicFile");
        if (!files_.contains(readmeFile_)) {
            readmeFile_ = std::string();
        }
        if (!files_.contains(graphicFile_)) {
            graphicFile_ = std::string();
        }
    }

    if (obj.contains("keyboards") && obj["keyboards"].is_array()) {
        for (const auto &file : obj["keyboards"]) {
            KmpKeyboardMetadata keyboard;
            keyboard.id = readStringValue(file, "id");
            if (keyboard.id.empty()) {
                continue;
            }
            keyboard.name = readStringValue(file, "name");
            if (keyboard.name.empty()) {
                keyboard.name = keyboard.id;
            }
            keyboard.version = readStringValue(file, "version");
            if (file.contains("languages") && file["languages"].is_array()) {
                for (const auto &language : file["languages"]) {
                    auto languageName = readStringValue(language, "name");
                    auto languageId = readStringValue(language, "id");
                    if (languageId.empty()) {
                        continue;
                    }
                    keyboard.languages.emplace_back(languageId, languageName);
                }
            }

            auto kmxFile = stringutils::concat(keyboard.id, ".kmx");
            if (!files_.contains(kmxFile)) {
                continue;
            }
            keyboards_[keyboard.id] = std::move(keyboard);
        }
    }
}

} // namespace fcitx
