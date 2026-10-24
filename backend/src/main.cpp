#include "ai_sort.hpp"
#include "api.hpp"
#include "auth.hpp"
#include "backup.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "db.hpp"
#include "mail_cache.hpp"
#include "rules.hpp"
#include "mail_service.hpp"
#include "mime.hpp"
#include "oauth.hpp"
#include "push.hpp"
#include "text_util.hpp"
#include "trash_purger.hpp"

#include <curl/curl.h>
#include <httplib.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--healthcheck") {
        const char* port = std::getenv("BREVDUVA_PORT");
        httplib::Client client("127.0.0.1", port ? std::atoi(port) : 8080);
        client.set_connection_timeout(3, 0);
        auto res = client.Get("/api/session");
        return res && res->status == 200 ? 0 : 1;
    }

    // Blockera signaler innan trådar skapas så att en dedikerad tråd kan vänta på dem.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        brevduva::Config config = brevduva::Config::fromEnvironment();
        brevduva::mime::init();
        curl_global_init(CURL_GLOBAL_DEFAULT);

        brevduva::Database db(config);
        brevduva::Auth auth(db);
        brevduva::GoogleOAuth oauth(config);
        brevduva::RawCache rawCache(config.rawCacheDir, static_cast<std::uint64_t>(config.rawCacheMb) * 1024 * 1024);
        brevduva::MailService mail(db, oauth, rawCache);
        brevduva::PushService push(config, db);
        brevduva::MailCache mailCache(config, db, mail, rawCache, push.configured() ? &push : nullptr);
        if (config.mailCache) mail.setCache(&mailCache);
        std::cout << "[main] Mail cache: " << (config.mailCache ? "on" : "off (MAIL_CACHE=0)") << std::endl;
        std::cout << "[main] Push to the Android app: "
                  << (push.configured() ? "enabled (Firebase project " + push.projectId() + ")"
                                        : "not configured (FCM_SERVICE_ACCOUNT_FILE)")
                  << (push.configured() && push.clientConfig().is_null()
                          ? "; FIREBASE_CONFIG_FILE missing, so only an app built with this project's google-services.json can register"
                          : "")
                  << std::endl;
        brevduva::BackupService backup(config, db, mail, oauth);
        brevduva::RuleEngine rules(config, db, mail, config.mailCache ? &mailCache : nullptr);
        brevduva::AiSorter aiSorter(config, db, mail, rules);
        brevduva::TrashPurger trashPurger(config, db, mail, config.mailCache ? &mailCache : nullptr);
        std::cout << "[main] Google sign-in for Gmail: " << (oauth.configured() ? "enabled" : "not configured")
                  << std::endl;

        if (config.legacyAppPassword) {
            std::cerr << "[main] APP_PASSWORD is no longer used; log in with user accounts. "
                         "Use ADMIN_USERNAME/ADMIN_PASSWORD to create the first administrator.\n";
        }
        if (auth.setupRequired()) {
            if (!config.adminUsername.empty() && !config.adminPassword.empty()) {
                brevduva::Auth::validateUsername(config.adminUsername);
                brevduva::Auth::validatePassword(config.adminPassword);
                db.createFirstAdmin(config.adminUsername, brevduva::crypto::hashPassword(config.adminPassword));
                std::cout << "[main] Created administrator " << config.adminUsername << " from environment variables"
                          << std::endl;
            } else {
                std::cerr << "[main] WARNING: no users exist. Whoever opens the web interface first becomes "
                             "an administrator. Set ADMIN_USERNAME/ADMIN_PASSWORD to avoid this.\n";
            }
        }

        httplib::Server server;
        server.set_payload_max_length(64 * 1024 * 1024);
        server.set_read_timeout(60, 0);
        server.set_write_timeout(300, 0);
        server.set_default_headers({
            {"X-Content-Type-Options", "nosniff"},
            {"Referrer-Policy", "no-referrer"},
            {"X-Frame-Options", "DENY"},
        });

        brevduva::registerApi(server, db, auth, mail, oauth, backup, aiSorter, rules, trashPurger,
                              config.mailCache ? &mailCache : nullptr, push.configured() ? &push : nullptr);
        backup.start();
        rules.start();
        aiSorter.start();
        trashPurger.start();
        push.start();
        if (config.mailCache) mailCache.start();

        // APK och version.json för Android-appen, från en katalog utanför webbygget (BREVDUVA_ANDROID_DIR).
        server.set_file_extension_and_mimetype_mapping("apk", "application/vnd.android.package-archive");
        if (std::filesystem::is_directory(config.androidDir)) {
            server.set_mount_point("/android", config.androidDir, {{"Cache-Control", "no-cache"}});
            // Filer serveras före handlers; en fil som saknas ska ge 404 och inte index.html från SPA-fallbacken nedan.
            server.Get(R"(^/android/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 404; });
            std::cout << "[main] Serving the Android app from " << config.androidDir << " at /android" << std::endl;
        } else {
            std::cout << "[main] No Android directory (" << config.androidDir << "); /android is disabled" << std::endl;
        }

        // Statiska filer från Vue-bygget, och index.html för alla övriga sökvägar (SPA-routing).
        if (std::filesystem::exists(config.staticDir)) {
            server.set_mount_point("/", config.staticDir);
            // Integritetspolicy och användarvillkor: mallar med {{SERVICE_URL}}, {{OPERATOR_NAME}} och
            // {{OPERATOR_CONTACT_HTML}} som fylls i från miljön. En egen privacy.html/terms.html i den statiska
            // katalogen (t.ex. monterad i containern) serveras i stället, eftersom filer går före handlers.
            for (const char* page : {"privacy", "terms"}) {
                std::string tmplPath = (std::filesystem::path(config.staticDir) / (std::string(page) + ".tmpl.html")).string();
                server.Get("/" + std::string(page) + ".html", [tmplPath, &config](const httplib::Request& req, httplib::Response& res) {
                    std::ifstream in(tmplPath, std::ios::binary);
                    if (!in) {
                        res.status = 404;
                        return;
                    }
                    std::stringstream ss;
                    ss << in.rdbuf();
                    std::string html = ss.str();
                    std::string url = config.publicUrl;
                    if (url.empty()) {
                        std::string host = req.get_header_value("X-Forwarded-Host");
                        if (host.empty()) host = req.get_header_value("Host");
                        std::string scheme = req.get_header_value("X-Forwarded-Proto");
                        if (scheme.empty()) scheme = "http";
                        url = host.empty() ? "" : scheme + "://" + host;
                    }
                    std::string contact = brevduva::textutil::htmlEscape(config.operatorContact);
                    std::string contactHtml = contact.empty() ? "the operator of this service"
                        : contact.find('@') != std::string::npos ? "<a href=\"mailto:" + contact + "\">" + contact + "</a>"
                        : "<a href=\"" + contact + "\">" + contact + "</a>";
                    auto replaceAll = [&html](const std::string& from, const std::string& to) {
                        for (std::size_t pos = html.find(from); pos != std::string::npos; pos = html.find(from, pos + to.size())) {
                            html.replace(pos, from.size(), to);
                        }
                    };
                    replaceAll("{{SERVICE_URL}}", brevduva::textutil::htmlEscape(url));
                    replaceAll("{{OPERATOR_NAME}}", brevduva::textutil::htmlEscape(config.operatorName));
                    replaceAll("{{OPERATOR_CONTACT_HTML}}", contactHtml);
                    res.set_header("Cache-Control", "no-cache");
                    res.set_content(html, "text/html; charset=utf-8");
                });
            }
            std::string indexPath = (std::filesystem::path(config.staticDir) / "index.html").string();
            server.Get(R"(^/(?!api/).*)", [indexPath](const httplib::Request&, httplib::Response& res) {
                std::ifstream in(indexPath, std::ios::binary);
                if (!in) {
                    res.status = 404;
                    return;
                }
                std::stringstream ss;
                ss << in.rdbuf();
                res.set_header("Cache-Control", "no-cache");
                res.set_content(ss.str(), "text/html; charset=utf-8");
            });
        } else {
            std::cerr << "[main] Cannot find " << config.staticDir << "; running the API only\n";
        }

        server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
            if (req.path.rfind("/api/", 0) == 0) {
                std::cout << req.method << " " << req.path << " " << res.status << std::endl;
            }
        });

        std::thread signalThread([&server, signals]() {
            int sig = 0;
            sigwait(&signals, &sig);
            std::cout << "Signal " << sig << " received, shutting down" << std::endl;
            server.stop();
        });
        signalThread.detach();

        std::cout << "PigeonPost listening on http://" << config.host << ":" << config.port << std::endl;
        if (!server.listen(config.host, config.port)) {
            std::cerr << "Could not listen on " << config.host << ":" << config.port << "\n";
            return 1;
        }
        mailCache.stop();
        push.stop();
        aiSorter.stop();
        rules.stop();
        trashPurger.stop();
        backup.stop();
        curl_global_cleanup();
    } catch (const std::exception& e) {
        std::cerr << "Startup error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
