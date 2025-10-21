#pragma once

#include <httplib.h>
#include <tinyxml2.h>
#include <thread>
#include <string>
#include <unordered_map>
#include <chrono>

#include "DavTree.h"

// Minimal in-process WebDAV-like endpoint for tests only.
// Supports OPTIONS, PROPFIND (Depth 0/1), GET, HEAD over an in-memory DavTree.
class MiniDavServer {
public:
    MiniDavServer(DavTree& tree, std::string mount = "/dav/")
        : _tree(tree), _base(std::move(mount)) {}

    void start() {
        // Routing
        _svr.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res){
            if (!handles(req.path)) return httplib::Server::HandlerResponse::Unhandled;
            if (req.method == "OPTIONS") { reply(options(), res); return httplib::Server::HandlerResponse::Handled; }
            if (req.method == "PROPFIND") { reply(propfind(req), res); return httplib::Server::HandlerResponse::Handled; }
            if (req.method == "HEAD") { reply(head(req), res); return httplib::Server::HandlerResponse::Handled; }
            if (req.method == "GET") { reply(get(req), res); return httplib::Server::HandlerResponse::Handled; }
            HttpResponse out; out.status=405; out.headers["Allow"]="OPTIONS, PROPFIND, GET, HEAD"; reply(out,res); return httplib::Server::HandlerResponse::Handled;
        });

        // Bind to loopback on an available port
        uint16_t p = 19100;
        for (int i=0;i<100 && !_svr.bind_to_port("127.0.0.1", p); ++i, ++p) {}
        _port = _svr.is_valid()? p : 0;
        if (!_port) throw std::runtime_error("MiniDavServer bind failed");

        _th = std::thread([this]{ _svr.listen_after_bind(); });
        for (int i=0;i<100 && !_svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!_svr.is_running()) throw std::runtime_error("MiniDavServer not running");
    }

    void stop(){ _svr.stop(); if (_th.joinable()) _th.join(); }

    uint16_t port() const { return _port; }

private:
    struct HttpResponse { int status=500; std::unordered_map<std::string,std::string> headers; std::string body; };

    bool handles(const std::string& p) const { return p.rfind(_base, 0) == 0; }

    std::string toTreePath(std::string p) const {
        if (!handles(p)) return {};
        // Keep leading '/'
        p = p.substr(_base.size()-1);
        if (p.find("..") != std::string::npos) return {};
        return p;
    }

    static std::string httpDate(const std::optional<std::chrono::system_clock::time_point>& tp) {
        if (!tp) return {};
        std::time_t t = std::chrono::system_clock::to_time_t(*tp);
        char buf[64]{}; std::tm g{};
    #ifdef _WIN32
        gmtime_s(&g, &t);
    #else
        g = *std::gmtime(&t);
    #endif
        std::strftime(buf,sizeof(buf),"%a, %d %b %Y %H:%M:%S GMT", &g);
        return buf;
    }

    static void addResponse(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* ms,
                            const std::string& href, const DavNode& n) {
        using namespace tinyxml2;
        auto* resp = doc.NewElement("D:response"); ms->InsertEndChild(resp);
        auto* hrefEl = doc.NewElement("D:href"); hrefEl->SetText(href.c_str()); resp->InsertEndChild(hrefEl);
        auto* ps = doc.NewElement("D:propstat"); resp->InsertEndChild(ps);
        auto* prop = doc.NewElement("D:prop"); ps->InsertEndChild(prop);
        auto* disp = doc.NewElement("D:displayname"); disp->SetText(n.name.c_str()); prop->InsertEndChild(disp);
        auto* rt = doc.NewElement("D:resourcetype"); if (n.isDir) { auto* c = doc.NewElement("D:collection"); rt->InsertEndChild(c);} prop->InsertEndChild(rt);
        if (!n.isDir) { auto* gcl = doc.NewElement("D:getcontentlength"); gcl->SetText(std::to_string(n.content.size()).c_str()); prop->InsertEndChild(gcl);}        
        if (auto d = httpDate(n.mtime); !d.empty()) { auto* glm = doc.NewElement("D:getlastmodified"); glm->SetText(d.c_str()); prop->InsertEndChild(glm);}        
        if (!n.isDir && !n.mime.empty()) { auto* gct = doc.NewElement("D:getcontenttype"); gct->SetText(n.mime.c_str()); prop->InsertEndChild(gct);}        
        auto* st = doc.NewElement("D:status"); st->SetText("HTTP/1.1 200 OK"); ps->InsertEndChild(st);
    }

    HttpResponse options() const {
        HttpResponse r; r.status=200; r.headers["DAV"]="1,2"; r.headers["Allow"]="OPTIONS, PROPFIND, GET, HEAD"; r.headers["Accept-Ranges"]="bytes"; return r;
    }

    HttpResponse propfind(const httplib::Request& req) const {
        HttpResponse out; int depth = 0; if (auto it=req.headers.find("Depth"); it!=req.headers.end()){ if (it->second=="1") depth=1; else if (it->second=="infinity") { out.status=400; out.headers["Content-Type"]="application/xml; charset=utf-8"; out.body="<error>Depth infinity not supported</error>"; return out; }}
        std::string p = toTreePath(req.path); if (p.empty()) { out.status=400; return out; }
        const DavNode* n = _tree.find(p); if (!n) { out.status=404; return out; }
        tinyxml2::XMLDocument doc; auto* decl = doc.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\""); doc.InsertFirstChild(decl);
        auto* ms = doc.NewElement("D:multistatus"); ms->SetAttribute("xmlns:D","DAV:"); doc.InsertEndChild(ms);
        std::string selfHref = req.path; if (n->isDir && selfHref.back()!='/') selfHref.push_back('/');
        addResponse(doc, ms, selfHref, *n);
        if (depth>=1 && n->isDir) {
            for (auto& kv : n->children) {
                const auto& c = *kv.second; std::string href = selfHref + c.name + (c.isDir? "/" : "");
                addResponse(doc, ms, href, c);
            }
        }
        tinyxml2::XMLPrinter pr(nullptr,false,0); doc.Print(&pr);
        out.status=207; out.headers["Content-Type"]="application/xml; charset=utf-8"; out.body.assign(pr.CStr(), pr.CStrSize()? pr.CStrSize()-1 : 0); return out;
    }

    HttpResponse head(const httplib::Request& req) const {
        HttpResponse out; std::string p = toTreePath(req.path); if (p.empty()) { out.status=400; return out; }
        auto* n = _tree.find(p); if (!n) { out.status=404; return out; }
        if (n->isDir) { out.status=405; out.headers["Allow"]="PROPFIND, OPTIONS"; return out; }
        out.status=200; out.headers["Content-Type"] = n->mime.empty()? "application/octet-stream" : n->mime; out.headers["Accept-Ranges"] = "bytes"; out.headers["Content-Length"] = std::to_string(n->content.size()); return out;
    }

    HttpResponse get(const httplib::Request& req) const {
        HttpResponse out; std::string p = toTreePath(req.path); if (p.empty()) { out.status=400; return out; }
        auto* n = _tree.find(p); if (!n) { out.status=404; return out; }
        if (n->isDir) { out.status=405; out.headers["Allow"]="PROPFIND, OPTIONS"; return out; }
        out.status=200; out.headers["Content-Type"] = n->mime.empty()? "application/octet-stream" : n->mime; /* Content-Length set by set_content */ out.body = n->content; return out;
    }

    static void reply(const HttpResponse& s, httplib::Response& res) {
        res.status = s.status;
        // Set content first (to let httplib populate default headers), then override with explicit headers.
        res.set_content(s.body, s.headers.count("Content-Type")? s.headers.at("Content-Type") : "application/octet-stream");
        for (auto& kv : s.headers) res.set_header(kv.first.c_str(), kv.second.c_str());
    }

    DavTree& _tree;
    std::string _base;
    httplib::Server _svr;
    std::thread _th;
    uint16_t _port = 0;
};
