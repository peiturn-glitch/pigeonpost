#include "api.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "db.hpp"
#include "mail_service.hpp"
#include "mime.hpp"
#include "oauth.hpp"

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
        const char* port = std::getenv("TBK_PORT");
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
        tbk::Config config = tbk::Config::fromEnvironment();
        tbk::mime::init();
        curl_global_init(CURL_GLOBAL_DEFAULT);

        tbk::Database db((std::filesystem::path(config.dataDir) / "thunderbirdklon.db").string(),
                         config.encryptionKey);
        tbk::Auth auth(db);
        tbk::GoogleOAuth oauth(config);
        tbk::MailService mail(db, oauth);
        std::cout << "[main] Google-inloggning för Gmail: " << (oauth.configured() ? "aktiverad" : "inte konfigurerad")
                  << std::endl;

        if (config.legacyAppPassword) {
            std::cerr << "[main] APP_PASSWORD används inte längre; logga in med användarkonton. "
                         "Använd ADMIN_USERNAME/ADMIN_PASSWORD för att skapa en första administratör.\n";
        }
        if (auth.setupRequired()) {
            if (!config.adminUsername.empty() && !config.adminPassword.empty()) {
                tbk::Auth::validateUsername(config.adminUsername);
                tbk::Auth::validatePassword(config.adminPassword);
                db.createFirstAdmin(config.adminUsername, tbk::crypto::hashPassword(config.adminPassword));
                std::cout << "[main] Skapade administratören " << config.adminUsername << " från miljövariabler"
                          << std::endl;
            } else {
                std::cerr << "[main] VARNING: inga användare finns. Den som först öppnar webbgränssnittet blir "
                             "administratör. Sätt ADMIN_USERNAME/ADMIN_PASSWORD för att undvika det.\n";
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

        tbk::registerApi(server, db, auth, mail, oauth);

        // Statiska filer från Vue-bygget, och index.html för alla övriga sökvägar (SPA-routing).
        if (std::filesystem::exists(config.staticDir)) {
            server.set_mount_point("/", config.staticDir);
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
            std::cerr << "[main] Hittar inte " << config.staticDir << "; kör endast API\n";
        }

        server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
            if (req.path.rfind("/api/", 0) == 0) {
                std::cout << req.method << " " << req.path << " " << res.status << std::endl;
            }
        });

        std::thread signalThread([&server, signals]() {
            int sig = 0;
            sigwait(&signals, &sig);
            std::cout << "Signal " << sig << " mottagen, stänger ned" << std::endl;
            server.stop();
        });
        signalThread.detach();

        std::cout << "ThunderbirdKlon lyssnar på http://" << config.host << ":" << config.port << std::endl;
        if (!server.listen(config.host, config.port)) {
            std::cerr << "Kunde inte lyssna på " << config.host << ":" << config.port << "\n";
            return 1;
        }
        curl_global_cleanup();
    } catch (const std::exception& e) {
        std::cerr << "Fel vid uppstart: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
