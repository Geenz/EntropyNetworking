/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "Networking/WebDAV/WebDAVPropfindParser.h"

#include <tinyxml2.h>
#include "Networking/WebDAV/WebDAVUtils.h"

namespace EntropyEngine::Networking::WebDAV {

using tinyxml2::XMLElement;

static std::string_view localName(const char* qn) {
    std::string_view q = qn ? qn : "";
    auto p = q.find_last_of(':');
    return (p == std::string_view::npos) ? q : q.substr(p + 1);
}

static XMLElement* firstByLocal(XMLElement* parent, std::string_view name) {
    for (auto* e = parent ? parent->FirstChildElement() : nullptr; e; e = e->NextSiblingElement()) {
        if (localName(e->Name()) == name) return e;
    }
    return nullptr;
}

std::vector<DavResourceInfo> parsePropfindXml(const std::vector<uint8_t>& xmlBytes) {
    std::vector<DavResourceInfo> out;
    if (xmlBytes.empty()) return out;

    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(xmlBytes.data()), xmlBytes.size()) != tinyxml2::XML_SUCCESS) {
        return out;
    }

    auto* ms = doc.FirstChildElement();
    if (!ms || localName(ms->Name()) != "multistatus") return out;

    for (auto* resp = ms->FirstChildElement(); resp; resp = resp->NextSiblingElement()) {
        if (localName(resp->Name()) != "response") continue;

        DavResourceInfo ri;
        if (auto* href = firstByLocal(resp, "href"); href && href->GetText()) {
            ri.href = href->GetText();
        } else {
            continue; // skip entries without href
        }

        XMLElement* okProp = nullptr;
        for (auto* ps = firstByLocal(resp, "propstat"); ps; ps = ps->NextSiblingElement()) {
            if (localName(ps->Name()) != "propstat") continue;
            if (auto* st = firstByLocal(ps, "status"); st && st->GetText()) {
                if (std::string_view(st->GetText()).find(" 200 ") != std::string_view::npos) {
                    okProp = firstByLocal(ps, "prop");
                    break;
                }
            }
        }
        if (!okProp) okProp = firstByLocal(resp, "prop");

        if (auto* rt = firstByLocal(okProp, "resourcetype")) {
            ri.isCollection = (firstByLocal(rt, "collection") != nullptr);
        }
        if (auto* gcl = firstByLocal(okProp, "getcontentlength"); gcl && gcl->GetText()) {
            ri.contentLength = std::strtoull(gcl->GetText(), nullptr, 10);
        }
        if (auto* gct = firstByLocal(okProp, "getcontenttype"); gct && gct->GetText()) {
            ri.contentType = gct->GetText();
        }
        if (auto* glm = firstByLocal(okProp, "getlastmodified"); glm && glm->GetText()) {
            ri.lastModified = Utils::parseHttpDate(glm->GetText());
        }

        out.push_back(std::move(ri));
    }

    return out;
}

} // namespace EntropyEngine::Networking::WebDAV
