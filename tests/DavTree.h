#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

// Minimal in-memory WebDAV-like tree for tests (no disk, no VFS)
struct DavNode
{
    bool isDir = false;
    std::string name;                                                    // last segment
    std::string content;                                                 // files only
    std::string mime;                                                    // optional mime type for files
    std::optional<std::chrono::system_clock::time_point> mtime;          // optional
    std::unordered_map<std::string, std::unique_ptr<DavNode>> children;  // for dirs
};

class DavTree
{
public:
    DavTree() {
        _root = std::make_unique<DavNode>();
        _root->isDir = true;
        _root->name = "";
    }

    void addDir(std::string path) {
        normalize(path, /*asCollection=*/true);
        auto* n = ensure(path);
        n->isDir = true;
    }

    void addFile(std::string path, std::string data, std::string mime = {}) {
        normalize(path, /*asCollection=*/false);
        auto [parent, name] = split(path);
        auto* p = ensure(parent + "/");
        auto node = std::make_unique<DavNode>();
        node->isDir = false;
        node->name = name;
        node->content = std::move(data);
        node->mime = std::move(mime);
        p->children[name] = std::move(node);
    }

    const DavNode* find(const std::string& path) const {
        std::string p = path;
        normalize(p, /*asCollection=*/false);
        if (p == "/" || p.empty()) return _root.get();
        const DavNode* cur = _root.get();
        size_t i = 1;  // skip leading '/'
        while (i < p.size()) {
            auto j = p.find('/', i);
            std::string seg = (j == std::string::npos) ? p.substr(i) : p.substr(i, j - i);
            if (seg.empty()) break;
            auto it = cur->children.find(seg);
            if (it == cur->children.end()) return nullptr;
            cur = it->second.get();
            if (j == std::string::npos)
                break;
            else
                i = j + 1;
        }
        return cur;
    }

private:
    std::unique_ptr<DavNode> _root;

    static void normalize(std::string& path, bool asCollection) {
        if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');
        if (path.find("..") != std::string::npos) throw std::invalid_argument("bad path");
        if (asCollection) {
            if (path.back() != '/') path.push_back('/');
        } else {
            if (path.size() > 1 && path.back() == '/') path.pop_back();
        }
    }

    static std::pair<std::string, std::string> split(const std::string& p) {
        std::string s = p;
        if (!s.empty() && s.back() == '/') s.pop_back();
        auto pos = s.find_last_of('/');
        if (pos == std::string::npos) return {"/", s};
        return {pos == 0 ? std::string("/") : s.substr(0, pos), s.substr(pos + 1)};
    }

    DavNode* ensure(std::string path) {
        normalize(path, /*asCollection=*/true);
        DavNode* cur = _root.get();
        size_t i = 1;
        while (i < path.size()) {
            auto j = path.find('/', i);
            std::string seg = path.substr(i, j - i);
            if (seg.empty()) break;
            auto it = cur->children.find(seg);
            if (it == cur->children.end()) {
                auto dir = std::make_unique<DavNode>();
                dir->isDir = true;
                dir->name = seg;
                DavNode* raw = dir.get();
                cur->children.emplace(seg, std::move(dir));
                cur = raw;
            } else {
                cur = it->second.get();
            }
            if (j == std::string::npos)
                break;
            else
                i = j + 1;
        }
        return cur;
    }
};
